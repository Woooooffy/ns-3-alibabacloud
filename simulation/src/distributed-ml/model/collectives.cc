#include "collectives.h"

#include <sstream>
#include <iomanip>
#include <functional>
#include <algorithm>

#define MSCCL_MAX_ITER 65536
// L2 overhead (e.g. PPP/eth header) added by the NetDevice on top of our packet,
// reserved when sizing fragments and used to keep fragment pacing from
// outrunning the device's actual transmission time
#define MSCCL_L2_OVERHEAD_BYTES 14

// RDMA-fabric transport constants: sport is allocated once per (channel, peer) connection,
// at bootstrap (see MscclChannel::SetupRdmaSendPeer), and reused for every subsequent
// Send() to that peer -- a persistent qp/connection per (channel,peer), mirroring how real
// NCCL keeps one long-lived connection per channel rather than opening a new one per
// message. This also gives RdmaHw's per-connection byte sequencing (ReceiverCheckSeq) a
// stable ordering guarantee to rely on. dport/pg are fixed since uniqueness only needs to
// hold on sport.
#define MSCCL_RDMA_SPORT_BASE 20000
#define MSCCL_RDMA_DPORT 21000
#define MSCCL_RDMA_PG 3

// flags are a 3-tuple of (workindex, gridoffset_iter, step) and it follows a lexicographical order. a threadblock is ahead of another iff its flag is ahead
#define COMPUTE_FLAG(__WORKINDEX__,__GRIDOFFSET_ITER__,__STEP__) \
 	 MSCCL_MAX_ITER*MSCCL_MAX_NUM_STEPS*(uint64_t)__WORKINDEX__ + ((uint64_t)__GRIDOFFSET_ITER__ * MSCCL_MAX_NUM_STEPS + (uint64_t)__STEP__)

#define GET_WORKINDEX_FROM_FLAG(__FLAG__) \
  	(__FLAG__) / (MSCCL_MAX_ITER*MSCCL_MAX_NUM_STEPS)

namespace ns3 {

	NS_LOG_COMPONENT_DEFINE("CollectivesApplication");

	NS_OBJECT_ENSURE_REGISTERED(CollectivesApplication);
	MscclChannel::MscclChannel(){}
	MscclChannel::MscclChannel(int id, Ptr<CollectivesApplication> app): m_id(id), m_dataType(app->GetDataType()), m_socketType(app->GetSocketTypeId()), m_app(app){}
	MscclChannel::~MscclChannel(){}

/*	void MscclChannel::SetupListener() {
		m_listenSocket = Socket::CreateSocket(m_app->GetNode(), PacketSocketFactory::GetTypeId());
    // Accept callback: first decide whether to accept the connection
    m_listenSocket->SetAcceptCallback(
        MakeCallback(&MscclChannel::CanAcceptConnection, this),
        MakeCallback(&MscclChannel::OnNewConnection, this)
    );
		PacketSocketAddress addr;
    InetSocketAddress localAddr(Ipv4Address::GetAny(), m_app->GetPort());
    m_listenSocket->Bind(localAddr);
    m_listenSocket->Listen();
	} */

	#ifdef FLOW_ID_TEST
	uint32_t MscclChannel::GetFlowId(int srcId, int dstId){
		std::pair<int, int> key(srcId, dstId);
		if (m_flowIds == nullptr){
			NS_FATAL_ERROR("Flow id table not set in FLOW_ID_TEST mode.");
		}
		if (auto search = m_flowIds->find(key); search != m_flowIds->end()){
			return search->second;
		}
		// m_flowIds[key] = m_flowId_counter;
		// m_flowId_counter++;
		// return m_flowIds[key];
		NS_FATAL_ERROR("Flow Id not found for " << srcId << " to " << dstId << ".");
	}

	void MscclChannel::SetFlowIdTable(std::map<std::pair<int, int>, uint32_t>* table){
		m_flowIds = table;
	}
	#endif

	void MscclChannel::ConnectSendPeer(int peerId){
		Ptr<Socket> sock = Socket::CreateSocket(m_app->GetNode(), m_socketType);
		PacketSocketAddress addr;
		addr.SetSingleDevice(m_app->GetSendDevicePeer(peerId, m_id)->GetIfIndex());
		addr.SetPhysicalAddress(m_app->GetPeerAddr(peerId, m_id));
		addr.SetProtocol(COLLECTIVES_PROTOCOL);

		sock->Bind(addr);
		sock->Connect(addr);
		// sock->SetRecvCallback(MakeCallback(&MscclChannel::RecvCallback, this));
		sock->SetDataSentCallback(MakeCallback(&MscclChannel::SendCallback, this));
		m_sendPeerSockets[peerId] = sock;
	}

	void MscclChannel::SetupRecvPeer(int peerId) {
		Ptr<Socket> sock = Socket::CreateSocket(m_app->GetNode(), m_socketType);
		PacketSocketAddress addr;
		addr.SetSingleDevice(m_app->GetRecvDevicePeer(peerId, m_id)->GetIfIndex());
		addr.SetProtocol(COLLECTIVES_PROTOCOL);
		sock->Bind(addr);
		m_recvSocketPeers[sock] = peerId;
		sock->SetRecvCallback(MakeCallback(&MscclChannel::RecvCallback, this));
		// sock->SetDataSentCallback(MakeCallback(&MscclChannel::SendCallback, this));
	}

	void MscclChannel::SendCallback(Ptr<Socket> sock, uint32_t bytes){
		NS_LOG_FUNCTION(this);
		uint32_t sendSize = bytes;
		std::queue<PendingTransfer>& sendQueue = m_pendingSends.at(sock);
		NS_LOG_DEBUG("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": SendCallback bytes=" << bytes << " queueSize=" << sendQueue.size() << (sendQueue.empty() ? " EMPTY" : " frontPending=" + std::to_string(sendQueue.front().pendingBytes)));
		while (sendSize > 0 && !sendQueue.empty()){
			auto& cur = sendQueue.front();
			uint32_t take = std::min(sendSize, cur.pendingBytes);
			cur.pendingBytes -= take;
			sendSize -= take;
			if (cur.pendingBytes == 0){
				// logical packet fully sent
				NS_LOG_INFO("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": send step complete bid=" << (int)cur.bid << " sid=" << cur.sid);
				sendQueue.pop();
				// send should always be last op
				Simulator::ScheduleNow(&CollectivesApplication::StepCompletionCallback, m_app, cur.bid, cur.sid);
			}
		}
		if (sendSize > 0){
			NS_LOG_WARN("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": SendCallback has " << sendSize << " unaccounted bytes (queue empty). Leftover bytes lost.");
		}
	}

	static void ReduceAdd(void* dst, const void* src, uint32_t bytes, DataType::Type dtype){
		switch (dtype){
			case DataType::INT32: {
				int32_t* d = static_cast<int32_t*>(dst);
				const int32_t* s = static_cast<const int32_t*>(src);
				for (uint32_t i = 0; i < bytes / sizeof(int32_t); ++i) d[i] += s[i];
				break;
			}
			case DataType::INT16: {
				int16_t* d = static_cast<int16_t*>(dst);
				const int16_t* s = static_cast<const int16_t*>(src);
				for (uint32_t i = 0; i < bytes / sizeof(int16_t); ++i) d[i] += s[i];
				break;
			}
			case DataType::FLOAT32: {
				float* d = static_cast<float*>(dst);
				const float* s = static_cast<const float*>(src);
				for (uint32_t i = 0; i < bytes / sizeof(float); ++i) d[i] += s[i];
				break;
			}
			case DataType::FLOAT64: {
				double* d = static_cast<double*>(dst);
				const double* s = static_cast<const double*>(src);
				for (uint32_t i = 0; i < bytes / sizeof(double); ++i) d[i] += s[i];
				break;
			}
			default:
				NS_FATAL_ERROR("Unsupported data type for reduction");
		}
	}

	void MscclChannel::RecvCallback(Ptr<Socket> sock){
		NS_LOG_FUNCTION(this);
		while (sock->GetRxAvailable() > 0){
			Address from;
      Ptr<Packet> packet = sock->RecvFrom(from);
			uint32_t recvSize = packet->GetSize();
			MscclHeader hdr;
			if (recvSize >= hdr.GetSerializedSize()){
				packet->RemoveHeader(hdr);
				recvSize = packet->GetSize();
			}
			else NS_FATAL_ERROR("Received packet with incomplete header");
			uint8_t* tmp = (uint8_t*) malloc(recvSize);
			packet->CopyData(tmp, recvSize);
			// peer
			uint16_t peerId = static_cast<uint16_t>(m_recvSocketPeers.at(sock));
			if (peerId != hdr.GetSrcGpu() || m_app->GetNode()->GetId() != hdr.GetDstGpu()){
					// debug prints for now
					// TODO: forwarding not yet handled
				// happens rn because all sockets on the node receives and forwards up.
				// to be fixed
				NS_LOG_DEBUG("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": discarding packet from " << hdr.GetSrcGpu() << " to " << hdr.GetDstGpu() << ", expected peer " << peerId << ". fragOff=" << hdr.GetFragByteOffset() << " totalBytes=" << hdr.GetBytes());
				free(tmp);
				continue;
			}
			if (m_id != hdr.GetChannel()){
				// expected to happen sometimes
				// TODO: handle multiple links channel-socket-device assignments
				NS_LOG_DEBUG("Node " << m_app->GetNode()->GetId() << ": discarding packet for channel " << hdr.GetChannel() << " on channel " << (int)m_id << ". src=" << hdr.GetSrcGpu() << " fragOff=" << hdr.GetFragByteOffset() << " totalBytes=" << hdr.GetBytes());
				free(tmp);
				continue;
			}
			// optional debug cross-check only: the wire-carried dst buf/offset is never used
			// to decide where bytes go (see OnBytesArrivedFromPeer) -- it's compared here only
			// to surface a send/recv order or count mismatch between ranks' algorithm XML,
			// exactly the class of bug real hardware would silently misdeliver on.
			int16_t peerIdSigned = static_cast<int16_t>(peerId);
			auto sockLaneKey = std::make_pair(peerIdSigned, (uint8_t) 0);
			if (!m_pendingRecvQueue[sockLaneKey].empty()){
				const PendingTransfer& front = m_pendingRecvQueue[sockLaneKey].front();
				if (front.dstBuf != hdr.GetDstBuf() || front.dstOffset != (int16_t)hdr.GetDstOff()){
					NS_LOG_WARN("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id
						<< ": wire-carried dst=(" << hdr.GetDstBuf() << "," << hdr.GetDstOff()
						<< ") disagrees with locally-posted recv dst=(" << front.dstBuf << "," << front.dstOffset
						<< ") from peer " << peerId << " -- possible algorithm XML order/count mismatch.");
				}
			}
			// the p2p socket transport is always a single stream, i.e. lane 0
			OnBytesArrivedFromPeer(static_cast<int16_t>(peerId), 0, tmp, recvSize);
			free(tmp);
		}
	}

	// shared per-fragment matching logic for both transports (called directly from
	// RecvCallback for sockets, and from the perPktFn lambda registered in
	// SetupRdmaSendPeer for RDMA): matches incoming bytes against `peer`'s locally-posted
	// Recv()/RecvRedCp() order using a purely local write-offset accumulator
	// (PendingTransfer::receivedBytes) -- no fragment offset or transfer-size info from the
	// sender is used or needed. Stages bytes in m_unclaimedBytes if no matching
	// Recv()/RecvRedCp() has been posted yet.
	void MscclChannel::OnBytesArrivedFromPeer(int16_t peer, uint8_t lane, const uint8_t* payload, uint32_t fragSize){
		auto laneKey = std::make_pair(peer, lane);
		auto& pendingQ = m_pendingRecvQueue[laneKey];
		if (!pendingQ.empty()){
			PendingTransfer& cur = pendingQ.front();
			if (m_app->GetCorrectnessCheck()){
				if (cur.op == MSCCL_RECV_REDUCE_COPY){
					if (cur.scratchBuf == nullptr) cur.scratchBuf = (uint8_t*) malloc(cur.pendingBytes);
					memcpy(cur.scratchBuf + cur.receivedBytes, payload, fragSize);
				} else {
					uint8_t* dst = (uint8_t*) m_app->GetBufferPtr(cur.dstBuf, cur.dstOffset);
					memcpy(dst + cur.dstByteShift + cur.receivedBytes, payload, fragSize);
				}
			}
			cur.receivedBytes += fragSize;
			NS_LOG_DEBUG("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": recv frag from peer "
				<< peer << " lane " << (int)lane << " dst=(" << cur.dstBuf << "," << cur.dstOffset << ") fragBytes=" << fragSize
				<< " accum=" << cur.receivedBytes << "/" << cur.pendingBytes);
			if (cur.receivedBytes < cur.pendingBytes) return;
			if (cur.receivedBytes > cur.pendingBytes){
				NS_FATAL_ERROR("Node " << m_app->GetNode()->GetId() << ": accumulator overshot for peer " << peer
					<< " dst=(" << cur.dstBuf << "," << cur.dstOffset << "): got " << cur.receivedBytes
					<< " expected " << cur.pendingBytes << ". Possible scheduling bug or duplicate packet.");
			}
			PendingTransfer done = cur; // copy out before pop invalidates the reference
			pendingQ.pop();
			if (m_app->GetCorrectnessCheck() && done.op == MSCCL_RECV_REDUCE_COPY){
				uint8_t* dst = (uint8_t*) m_app->GetBufferPtr(done.dstBuf, done.dstOffset);
				ReduceAdd(dst + done.dstByteShift, done.scratchBuf, done.pendingBytes, m_dataType);
				free(done.scratchBuf);
			}
			NS_LOG_INFO("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id
				<< ": transfer complete from peer " << peer << " dst=(" << done.dstBuf << "," << done.dstOffset
				<< ") totalBytes=" << done.pendingBytes << " at t=" << Simulator::Now().GetNanoSeconds());
			// The step is done only when every lane's slice has landed; with one lane this is
			// an immediate fire, exactly as before.
			NoteRecvLaneComplete(done.bid, done.sid, 1);
			return;
		}
		// early arrival: no Recv()/RecvRedCp() posted for this peer yet -- accumulate into
		// the unclaimed byte stream; a future Recv()/RecvRedCp() call will carve off exactly
		// the bytes it locally expects from the front, in FIFO order. Nothing about transfer
		// boundaries crosses the wire, so none is needed here.
		UnclaimedBytes& unclaimed = m_unclaimedBytes[laneKey];
		if (m_app->GetCorrectnessCheck()){
			unclaimed.stagingBuf = (uint8_t*) realloc(unclaimed.stagingBuf, unclaimed.gotBytes + fragSize);
			memcpy(unclaimed.stagingBuf + unclaimed.gotBytes, payload, fragSize);
		}
		unclaimed.gotBytes += fragSize;
	}

	// shared body of Recv()/RecvRedCp(): claims bytes already sitting in m_unclaimedBytes
	// for `recvPeer` if any (a full or partial claim, in FIFO byte order), else registers a
	// new pending recv to be matched by a future arrival.
	void MscclChannel::ClaimOrRegisterPendingRecv(int16_t bid, int16_t sid, int16_t recvPeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff, int8_t op){
		if (dstoff < 0) NS_FATAL_ERROR("Invalid offset");
		uint32_t bytes = nElems * DataType::GetSizeBytes(m_dataType);

		// One posted recv per lane. The sender cut this transfer into the same contiguous
		// slices (LaneSlice is a pure function of the byte count, and both ends derive the
		// byte count from their own copy of the algorithm), and sends slice i on lane i, so
		// matching lane i's stream against lane i's slice reassembles the transfer with no
		// per-fragment placement information on the wire. The step completes when the last
		// lane's slice does.
		const uint8_t nLanes = m_app->GetRdmaLaneCount(recvPeer);
		m_recvLanesLeft[std::make_pair(bid, sid)] = nLanes;

		for (uint8_t lane = 0; lane < nLanes; ++lane){
			uint32_t laneOff = 0, laneBytes = 0;
			LaneSlice(bytes, nLanes, lane, laneOff, laneBytes);
			UnclaimedBytes& unclaimed = m_unclaimedBytes[std::make_pair(recvPeer, lane)];

			if (unclaimed.gotBytes >= laneBytes){
				// fully available: claim the front `laneBytes` worth, leave any remainder (e.g. the
				// start of a later transfer that also arrived early) for the next claim
				if (m_app->GetCorrectnessCheck() && unclaimed.stagingBuf){
					uint8_t* dst = (uint8_t*) m_app->GetBufferPtr(dstbuf, dstoff) + laneOff;
					if (op == MSCCL_RECV_REDUCE_COPY) ReduceAdd(dst, unclaimed.stagingBuf, laneBytes, m_dataType);
					else memcpy(dst, unclaimed.stagingBuf, laneBytes);
				}
				uint32_t remaining = unclaimed.gotBytes - laneBytes;
				if (m_app->GetCorrectnessCheck() && unclaimed.stagingBuf){
					if (remaining > 0){
						memmove(unclaimed.stagingBuf, unclaimed.stagingBuf + laneBytes, remaining);
						unclaimed.stagingBuf = (uint8_t*) realloc(unclaimed.stagingBuf, remaining);
					} else {
						free(unclaimed.stagingBuf);
						unclaimed.stagingBuf = nullptr;
					}
				}
				unclaimed.gotBytes = remaining;
				NoteRecvLaneComplete(bid, sid, nLanes);
				continue;
			}

			PendingTransfer pt(bid, sid, laneBytes, op, 0, -1, dstbuf, dstoff);
			pt.dstByteShift = laneOff;
			if (unclaimed.gotBytes > 0){
				// still streaming in: promote to a pending transfer, carrying over what already
				// arrived, so future fragments from this peer (now matched via m_pendingRecvQueue)
				// land at the now-known destination
				pt.receivedBytes = unclaimed.gotBytes;
				if (op == MSCCL_RECV_REDUCE_COPY){
					pt.scratchBuf = (uint8_t*) malloc(laneBytes);
					if (unclaimed.stagingBuf) memcpy(pt.scratchBuf, unclaimed.stagingBuf, unclaimed.gotBytes);
				} else if (m_app->GetCorrectnessCheck() && unclaimed.stagingBuf){
					uint8_t* dst = (uint8_t*) m_app->GetBufferPtr(dstbuf, dstoff) + laneOff;
					memcpy(dst, unclaimed.stagingBuf, unclaimed.gotBytes);
				}
				free(unclaimed.stagingBuf);
				unclaimed.stagingBuf = nullptr;
				unclaimed.gotBytes = 0;
			}
			m_pendingRecvQueue[std::make_pair(recvPeer, lane)].push(pt);
		}
	}

	// A step's recv completes once every lane's slice has landed. The counter is seeded by
	// ClaimOrRegisterPendingRecv; arrivals decrement it without knowing the lane count, since
	// the entry already carries it.
	void MscclChannel::NoteRecvLaneComplete(int16_t bid, int16_t sid, uint8_t /*nLanes*/){
		auto key = std::make_pair(bid, sid);
		auto it = m_recvLanesLeft.find(key);
		if (it == m_recvLanesLeft.end()){
			// No counter seeded: this transfer was never posted through
			// ClaimOrRegisterPendingRecv. Treat it as single-lane and complete it directly.
			Simulator::ScheduleNow(&CollectivesApplication::StepCompletionCallback, m_app, bid, sid);
			return;
		}
		if (--(it->second) > 0) return;
		m_recvLanesLeft.erase(it);
		Simulator::ScheduleNow(&CollectivesApplication::StepCompletionCallback, m_app, bid, sid);
	}

	void MscclChannel::PushPendingSend(Ptr<Socket> sock, PendingTransfer send){
		m_pendingSends[sock].push(send);
	}

	void MscclChannel::Send(int16_t bid, int16_t sid, int16_t sendpeer, uint32_t nElems, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t mscclFlowId, double rateGBps){
		if (sendpeer < 0){
			NS_FATAL_ERROR("Send peer is negative in Send");
		}
		if (dstoff < 0){
			NS_FATAL_ERROR("Invalid dst offset in Send");
		}
		if (m_app->IsRdmaPeer(sendpeer)){
			SendRdma(bid, sid, sendpeer, nElems, srcbuf, srcoff, dstbuf, dstoff, mscclFlowId, rateGBps);
			return;
		}
		// gpu<->gpu p2p (socket) path: the host-side "rate" is ignored here, since this
		// direct link is paced by the device data rate (see SendNextFragment), not by the
		// schedule; only RDMA fabric transfers honor the XML rate.
		uint32_t totalBytes = nElems * DataType::GetSizeBytes(m_dataType);
		Ptr<Socket> sock = m_sendPeerSockets.at(sendpeer);
		int flowId = 0;
		#ifdef FLOW_ID_TEST
			flowId = GetFlowId(m_app->GetNode()->GetId(), sendpeer);
		#endif

		Ptr<NetDevice> dev = m_app->GetSendDevicePeer(sendpeer, m_id);
		uint32_t mtu = dev->GetMtu();
		MscclHeader templateHdr(m_app->GetNode()->GetId(), static_cast<uint16_t>(sendpeer), static_cast<uint16_t>(m_id), dstbuf, static_cast<uint16_t>(dstoff), totalBytes, flowId);
		uint32_t headerSize = templateHdr.GetSerializedSize();
		uint32_t maxPayload = mtu - headerSize - MSCCL_L2_OVERHEAD_BYTES;
		// round down to a multiple of the element size so fragment boundaries
		// never split an element; otherwise ReduceAdd misaligns across fragments
		uint32_t elemSize = DataType::GetSizeBytes(m_dataType);
		maxPayload -= maxPayload % elemSize;

		const uint8_t* srcData = m_app->GetCorrectnessCheck()
		    ? (const uint8_t*) m_app->GetBufferPtr(srcbuf, srcoff)
		    : nullptr;

		std::queue<PendingFragment> frags;
		uint32_t totalWireBytes = 0;
		uint32_t offset = 0;
		while (offset < totalBytes){
			uint32_t fragPayload = std::min(maxPayload, totalBytes - offset);
			Ptr<Packet> pkt = srcData
			    ? Create<ns3::Packet>(srcData + offset, fragPayload)
			    : Create<ns3::Packet>(fragPayload);
			MscclHeader fragHdr(m_app->GetNode()->GetId(), static_cast<uint16_t>(sendpeer), static_cast<uint16_t>(m_id), dstbuf, static_cast<uint16_t>(dstoff), totalBytes, flowId, offset);
			pkt->AddHeader(fragHdr);
			totalWireBytes += pkt->GetSize();
			frags.emplace(pkt, sock);
			offset += fragPayload;
		}

		PendingTransfer send(bid, sid, totalWireBytes, MSCCL_SEND, srcbuf, srcoff, dstbuf, dstoff);
		m_pendingSends[sock].push(send);

		// pacing happens per physical device, since multiple channels may share one
		m_app->QueueFragmentsForDevice(dev, std::move(frags));
	}

	// eagerly establishes this channel's persistent RDMA connection to `peer`: allocates
	// this connection's sport once (for its whole lifetime, not per Send() call -- the
	// receiver's ReceiverCheckSeq relies on all of this connection's bytes sharing one
	// sequence space to guarantee in-order delivery), force-creates the peer's rx qp and
	// hangs the byte-arrival callback on it once, and creates the persistent qp itself with
	// no message queued yet (autoClose=false, so idling between Send() calls never tears it
	// down -- see RdmaQueuePair::m_autoClose).
	void MscclChannel::SetupRdmaSendPeer(int16_t peer){
		Ptr<Node> peerNode = NodeList::GetNode(static_cast<uint32_t>(peer));
		Ptr<CollectivesApplication> peerApp =
			DynamicCast<CollectivesApplication>(peerNode->GetApplication(0));
		MscclChannel* peerChan = peerApp->GetChannel(m_id);
		int16_t myId = static_cast<int16_t>(m_app->GetNode()->GetId());

		// One lane per NIC this connection is striped over: 1 normally, or one per equal-cost
		// NIC when NIC merging is on (see CollectivesApplication::GetRdmaLaneCount). Both ends
		// compute the same count from the same rule, so lane i here is lane i there.
		const uint8_t nLanes = m_app->GetRdmaLaneCount(peer);

		// The NIC the schedule dictates, if any. On a multi-homed fabric that choice picks the
		// network plane, and a connection injected on the wrong plane meets no flow rules at all
		// along its path, silently reverting to ECMP. It is only meaningful single-lane: a
		// merged device deliberately spans every plane at once, which is why NIC merging and the
		// schedule pin are mutually exclusive (GetRdmaLaneCount returns 1 whenever a pin exists).
		uint32_t scheduledNic = RdmaQueuePair::NIC_UNPINNED;
		DynamicCast<GPU>(m_app->GetNode())->GetPeerNic(peer, m_id, scheduledNic);

		// NICs reaching this peer, in ifIndex order. Lane i takes nics[i], the same
		// "devIndex = qpIndex % ndevs" striping a merged device uses over its ports.
		std::vector<int> nics;
		m_app->GetRdmaDriver()->m_rdma->GetNicsToward(m_app->GetPeerIp(peer), nics);

		std::vector<Ptr<RdmaQueuePair>>& qps = m_rdmaQpByPeer[peer];
		qps.clear();
		for (uint8_t lane = 0; lane < nLanes; ++lane){
			// node-global (not per-channel) -- see CollectivesApplication::AllocateRdmaSport for why
			uint16_t sport = static_cast<uint16_t>(MSCCL_RDMA_SPORT_BASE + m_app->AllocateRdmaSport());

			// Force-create (rather than wait for the first packet to lazily create) the peer's
			// RdmaRxQueuePair for this connection and hang the byte-arrival callback directly off
			// it -- this piggybacks on RdmaHw's own (senderIp,senderSport,pg) rx-qp key instead of
			// deriving a second, independent key, so uniqueness is guaranteed by construction as
			// long as sport is unique (see AllocateRdmaSport). Parameter mapping mirrors
			// RdmaHw::ReceiveUdp's GetRxQp(ch.dip, ch.sip, ch.udp.dport, ch.udp.sport, ch.udp.pg,
			// true) call from the sender's side of that connection: this node is "ch.sip"/"dip",
			// the peer is "ch.dip"/"sip", MSCCL_RDMA_DPORT is "ch.udp.dport"/"sport", and this
			// connection's freshly allocated sport is "ch.udp.sport"/"dport".
			// perPktFn forwards straight into the shared matching logic -- RdmaHw's wire-level
			// sequence number is deliberately ignored; the receiver derives everything it needs
			// (write offset, transfer boundaries) from its own local accumulators instead (see
			// OnBytesArrivedFromPeer). Binding `lane` here is what ties the receiver's slice of
			// each transfer to the sender's: this qp carries slice `lane`, so its arrivals are
			// matched against the receiver's lane-`lane` queue and land at that slice's offset.
			Ptr<RdmaRxQueuePair> rxQp = peerApp->GetRdmaDriver()->m_rdma->GetRxQp(
				m_app->GetPeerIp(peer).Get(), m_app->GetMyIp().Get(), MSCCL_RDMA_DPORT, sport, MSCCL_RDMA_PG, true);
			rxQp->m_perPktFn = [peerChan, myId, lane](const uint8_t* payload, uint32_t sz, uint64_t /*seqOffset*/){
				peerChan->OnBytesArrivedFromPeer(myId, lane, payload, sz);
			};

			uint32_t pinnedNic = (nLanes > 1)
				? (lane < nics.size() ? (uint32_t) nics[lane] : RdmaQueuePair::NIC_UNPINNED)
				: scheduledNic;

			NS_LOG_INFO("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id
				<< ": established persistent rx qp (sport=" << sport << ") for peer " << peer
				<< " lane " << (int)lane << "/" << (int)nLanes
				<< " at t=" << Simulator::Now().GetNanoSeconds());

			qps.push_back(m_app->GetRdmaDriver()->AddQueuePair(
				m_app->GetNode()->GetId(), static_cast<uint32_t>(peer), /* tag */ 0, /* size */ 0, MSCCL_RDMA_PG,
				m_app->GetMyIp(), m_app->GetPeerIp(peer), sport, MSCCL_RDMA_DPORT,
				m_app->GetPeerWin(peer), m_app->GetPeerBaseRtt(peer), MSCCL_FLOW_ID_NONE, Callback<void>(), Callback<void>(),
				nullptr, /* autoClose */ false, pinnedNic));
		}
	}

	// Cuts on element boundaries, spreading any remainder one element at a time over the low
	// lanes, so every lane's slice is a whole number of elements and the slices tile the
	// transfer exactly. Depends only on its arguments, so sender and receiver agree without
	// exchanging anything -- the same property the peer-keyed FIFO matching already relies on.
	void MscclChannel::LaneSlice(uint32_t totalBytes, uint8_t nLanes, uint8_t lane, uint32_t& offset, uint32_t& size) const {
		uint32_t elemSize = DataType::GetSizeBytes(m_dataType);
		uint32_t nElems = totalBytes / elemSize;
		uint32_t base = nElems / nLanes, rem = nElems % nLanes;
		uint32_t startElem = (uint32_t) lane * base + std::min<uint32_t>(lane, rem);
		uint32_t myElems = base + (lane < rem ? 1 : 0);
		offset = startElem * elemSize;
		size = myElems * elemSize;
	}

	void MscclChannel::SendRdma(int16_t bid, int16_t sid, int16_t sendpeer, uint32_t nElems, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t mscclFlowId, double rateGBps){
		uint32_t totalBytes = nElems * DataType::GetSizeBytes(m_dataType);

		auto it = m_rdmaQpByPeer.find(sendpeer);
		if (it == m_rdmaQpByPeer.end()){
			NS_FATAL_ERROR("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id
				<< ": SendRdma to peer " << sendpeer << " but no persistent qp was established -- "
				<< "peer " << sendpeer << " is missing from this channel's sendPeerInfo in the algorithm XML.");
		}
		std::vector<Ptr<RdmaQueuePair>>& qps = it->second;
		const uint8_t nLanes = static_cast<uint8_t>(qps.size());

		// srcDataPtr is passed into PushMessage so RdmaHw can resolve it BEFORE the first
		// GetNxtPacket call for this message -- otherwise the first packet of the transfer
		// would embed zero bytes instead of real data.
		uint8_t* srcDataPtr = m_app->GetCorrectnessCheck()
			? (uint8_t*)m_app->GetBufferPtr(srcbuf, srcoff) : nullptr;

		// host-side pacing cap: XML "rate" is in GB/s (gigabytes/s); DataRate is in bits/s,
		// so convert bytes->bits. 0 (unspecified) maps to DataRate(0), i.e. no cap, and the
		// qp paces at its congestion-controlled rate (see RdmaHw::UpdateNextAvail).
		// Split across lanes with the bytes, so the connection's total target rate is what the
		// schedule asked for rather than nLanes times it.
		DataRate paceRate = rateGBps > 0.0 ? DataRate((uint64_t)(rateGBps * 8e9 / nLanes)) : DataRate(0);

		// This step's network gate opens only once every lane has physically drained, so count
		// the lanes up front and let OnRdmaSendComplete count them back down. Empty lanes (a
		// transfer of fewer elements than lanes) are excluded: a zero-byte message is never
		// acked, so it would never fire its completion and the gate would hang.
		uint8_t activeLanes = 0;
		for (uint8_t lane = 0; lane < nLanes; ++lane){
			uint32_t off = 0, sz = 0;
			LaneSlice(totalBytes, nLanes, lane, off, sz);
			if (sz > 0) ++activeLanes;
		}
		if (activeLanes == 0){
			// Nothing to send at all; the gate is the only thing waiting, so open it directly.
			Simulator::ScheduleNow(&CollectivesApplication::OpenGateForStep, m_app, bid, sid);
		} else {
			m_sendLanesLeft[std::make_pair(bid, sid)] = activeLanes;
		}

		for (uint8_t lane = 0; lane < nLanes; ++lane){
			uint32_t laneOff = 0, laneBytes = 0;
			LaneSlice(totalBytes, nLanes, lane, laneOff, laneBytes);
			if (laneBytes == 0) continue;

			// ns-3's Callback<void> can't wrap a capturing lambda here (FunctorCallbackImpl
			// requires operator!= on the functor, which closures don't have), so bind the
			// context onto a member-function Callback one argument at a time instead.
			Callback<void> finishCb = MakeCallback(&MscclChannel::OnRdmaSendComplete, this)
				.Bind(bid).Bind(sid).Bind(sendpeer).Bind(srcbuf).Bind(srcoff).Bind(dstbuf).Bind(dstoff).Bind(nElems);

			qps[lane]->PushMessage(laneBytes, srcDataPtr ? srcDataPtr + laneOff : nullptr,
				mscclFlowId, finishCb, Callback<void>(), paceRate);
			// PushMessage only enqueues onto the qp; unlike AddQueuePair (which calls NewQp once
			// at bootstrap for this persistent qp's creation), it doesn't wake up the NIC. Without
			// this, a qp that had drained to idle between Send() calls never gets re-polled and
			// the message sits forever -- see RdmaHw::TriggerTransmit.
			m_app->GetRdmaDriver()->m_rdma->TriggerTransmit(qps[lane]);
		}

		NS_LOG_INFO("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": RDMA send to "
			<< sendpeer << " totalBytes=" << totalBytes << " over " << (int)nLanes << " lane(s)"
			<< " rateGBps=" << rateGBps << " at t=" << Simulator::Now().GetNanoSeconds());

		// The send step completes as soon as the message is handed to the RDMA engine, not
		// when the data physically finishes transmitting: from the host algorithm's view its
		// job (posting the send) is done here. OnRdmaSendComplete still fires later; it does
		// not drive step completion, but it is where this step's network gate (if any) opens
		// -- that is the whole point of gates, since a *send* step's completion here says
		// nothing about the wire (see mscclTransfer::netGate/netWait).
		Simulator::ScheduleNow(&CollectivesApplication::StepCompletionCallback, m_app, bid, sid);
	}

	void MscclChannel::OnRdmaSendComplete(int16_t bid, int16_t sid, int16_t sendpeer, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t nElems){
		uint16_t dstoffU = static_cast<uint16_t>(dstoff);
		// The send step itself already completed in SendRdma, when the message was handed to
		// the RDMA engine; this fires later, on message completion (snd_una >= startSeq+size),
		// and does not drive step completion. It does open this step's network gate: message
		// completion is the only signal a real proxy thread can reap, so it is what a gate is
		// defined on.
		NS_LOG_INFO("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id
			<< ": RDMA send to " << sendpeer << " physically drained (sender side)"
			<< " bid=" << (int)bid << " sid=" << sid
			<< " dstInfo=(" << dstbuf << "," << dstoffU << ")"
			<< " at t=" << Simulator::Now().GetNanoSeconds());
		// The finishCb bind list is already full, so the gate id is looked up from the transfer
		// here rather than bound as a ninth argument -- bid and sid identify it uniquely.
		// With the message striped over several lanes this fires once per lane, but the gate
		// means "this step's data is off the wire", so it opens only when the last lane drains.
		auto key = std::make_pair(bid, sid);
		auto it = m_sendLanesLeft.find(key);
		if (it != m_sendLanesLeft.end()){
			if (--(it->second) > 0) return;
			m_sendLanesLeft.erase(it);
		}
		m_app->OpenGateForStep(bid, sid);
	}

	void MscclChannel::Recv(int16_t bid, int16_t sid, int16_t recvpeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff){
		ClaimOrRegisterPendingRecv(bid, sid, recvpeer, nElems, dstbuf, dstoff, MSCCL_RECV);
	}

	void MscclChannel::RecvCpSend(int16_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems){
	NS_FATAL_ERROR("RecvCpSend not yet implemented");
	}

	void MscclChannel::RecvRedSend(int16_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems){
	NS_FATAL_ERROR("RecvRedSend not yet implemented");
	}

	void MscclChannel::RecvRedCp(int16_t bid, int16_t sid, int16_t recvpeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff){
		ClaimOrRegisterPendingRecv(bid, sid, recvpeer, nElems, dstbuf, dstoff, MSCCL_RECV_REDUCE_COPY);
	}

	void MscclChannel::RecvRedCpSend(int16_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems){
	NS_FATAL_ERROR("RecvRedCpSend not yet implemented");
	}

	void MscclChannel::Close(){
		// check for unfinished sends
		for (auto& sendQueue : m_pendingSends){
			if (!sendQueue.second.empty()){
				NS_FATAL_ERROR("BUG: has pending send on node " << m_app->GetNode()->GetId() << " channel " << m_id << " at application close.");
			}
		}
		// unfinished recvs
		for (auto& recvQueue : m_pendingRecvQueue){
			if (!recvQueue.second.empty()){
				NS_FATAL_ERROR("BUG: has pending recv from peer " << recvQueue.first.first << " lane " << (int)recvQueue.first.second << " on node " << m_app->GetNode()->GetId() << " channel " << (int)m_id << " at application close.");
			}
		}
		for (auto& unclaimed : m_unclaimedBytes){
			if (unclaimed.second.gotBytes > 0){
				NS_FATAL_ERROR("BUG: has " << unclaimed.second.gotBytes << " unclaimed bytes from peer " << unclaimed.first.first << " lane " << (int)unclaimed.first.second << " on node " << m_app->GetNode()->GetId() << " channel " << (int)m_id << " at application close.");
			}
		}
		// socket close
		if (m_listenSocket) m_listenSocket->Close();
		for (auto& pair: m_sendPeerSockets){
			pair.second->Close();
		}
		for (auto& pair: m_recvSocketPeers){
			pair.first->Close();
		}
		// tear down persistent RDMA connections
		for (auto& pair: m_rdmaQpByPeer){
			for (Ptr<RdmaQueuePair>& qp : pair.second) m_app->GetRdmaDriver()->CloseQueuePair(qp);
		}
	}

	//////////////////////////////////////////////
	TypeId CollectivesApplication::GetTypeId()
	{
		static TypeId tid =
			TypeId("ns3::CollectivesApplication")
				.SetParent<Application>()
				.SetGroupName("Applications")
				.AddConstructor<CollectivesApplication>()
				.AddAttribute(
					"DataType",
					"Element datatype used in the collective operation",
					EnumValue(DataType::INT32),
					MakeEnumAccessor(&CollectivesApplication::m_dataType),
					MakeEnumChecker(
						DataType::FLOAT32, "FLOAT32",
						DataType::FLOAT64, "FLOAT64",
						DataType::INT32,   "INT32",
						DataType::INT16,   "INT16"))
				.AddAttribute(
					"Protocol",
					"The type of protocol to use. This should be a subclass of ns3::SocketFactory",
					TypeIdValue(PacketSocketFactory::GetTypeId()),
					MakeTypeIdAccessor(&CollectivesApplication::m_socket_tid),
					MakeTypeIdChecker())
				.AddAttribute(
					"ChunkSize", "Number of elements in a chunk",
				UintegerValue(1024),
				MakeUintegerAccessor(&CollectivesApplication::m_currChunkSize),
				MakeUintegerChecker<uint32_t>())
				.AddAttribute(
					"CorrectnessCheck",
					"When true, perform actual memcpy and reduce operations for correctness verification. "
					"Set false for large-chunk simulation where data values are irrelevant.",
					BooleanValue(false),
					MakeBooleanAccessor(&CollectivesApplication::m_correctnessCheck),
					MakeBooleanChecker())
				.AddAttribute(
					"MergeNics",
					"Model NCCL's merged virtual device (NCCL_IB_MERGE_NICS): fuse the NICs that "
					"reach a peer into one logical device, open one queue pair per NIC, and split "
					"every message across them. When false, each connection is bound to a single "
					"NIC. Ignored for connections whose NIC the switch JSON pins, which are "
					"deliberately confined to one network plane.",
					BooleanValue(false),
					MakeBooleanAccessor(&CollectivesApplication::m_mergeNics),
					MakeBooleanChecker());
		return tid;
	}


	CollectivesApplication::CollectivesApplication(): Application(){}

	CollectivesApplication::~CollectivesApplication(){
		free(m_srcBuf.dataBuffer);
		free(m_dstBuf.dataBuffer);
		free(m_scratchBuf.dataBuffer);
	}

	void CollectivesApplication::SetAlgo(mscclAlgorithm* algo){
		m_algo = algo;
	}

	void CollectivesApplication::SetCurrChunkSize(uint32_t chunksize){
		m_currChunkSize = chunksize;
	}

	void CollectivesApplication::SetCorrectnessCheck(bool enable){
		m_correctnessCheck = enable;
	}

	bool CollectivesApplication::GetCorrectnessCheck() const {
		return m_correctnessCheck;
	}

	Address CollectivesApplication::GetPeerAddr(int16_t peer, int ind){
		// TODO fix hard coding node downcast type
		return DynamicCast<GPU>(GetNode())->GetPeerAddr(peer, ind);
	}

	Ptr<NetDevice> CollectivesApplication::GetSendDevicePeer(int16_t peer, int ind){
		// TODO same as above
		return DynamicCast<GPU>(GetNode())->GetSendDevicePeer(peer, ind);
	}

	Ptr<NetDevice> CollectivesApplication::GetRecvDevicePeer(int16_t peer, int ind){
		// TODO same as above
		return DynamicCast<GPU>(GetNode())->GetRecvDevicePeer(peer, ind);
	}

	int CollectivesApplication::GetPort(){
		return m_port;
	}

	// Lanes = parallel RDMA connections to `peer`, i.e. how many NICs a transfer is striped
	// over. This models NCCL's merged virtual device: with NCCL_IB_MERGE_NICS the ports of one
	// physical NIC are fused into a single logical device, one qp is created per port
	// ("devIndex = qpIndex % ndevs"), and every message is split proportionally across them --
	// as opposed to the unmerged case, where a connection gets exactly one port.
	//
	// Both ends must agree, and there is no handshake to carry the number, so it is derived:
	// the min of the two ends' equal-cost NIC counts is symmetric, so each end computes the
	// same value from the same two inputs.
	uint8_t CollectivesApplication::GetRdmaLaneCount(int16_t peer){
		auto cached = m_rdmaLaneCount.find(peer);
		if (cached != m_rdmaLaneCount.end()) return cached->second;

		uint8_t lanes = 1;
		if (m_mergeNics && IsRdmaPeer(peer)){
			Ptr<Node> peerNode = NodeList::GetNode(static_cast<uint32_t>(peer));
			Ptr<CollectivesApplication> peerApp =
				DynamicCast<CollectivesApplication>(peerNode->GetApplication(0));
			// A schedule-pinned connection is deliberately confined to one plane, the opposite
			// of what merging does, so a pin wins and keeps the connection single-lane. Checked
			// on BOTH ends: a pin on either side alone would otherwise leave one end striping
			// over two lanes while the other expected one, desyncing the streams.
			if (HasScheduledNicToward(peer) || peerApp->HasScheduledNicToward(static_cast<int16_t>(GetNode()->GetId()))){
				m_rdmaLaneCount[peer] = 1;
				return 1;
			}
			std::vector<int> mine, theirs;
			GetRdmaDriver()->m_rdma->GetNicsToward(GetPeerIp(peer), mine);
			peerApp->GetRdmaDriver()->m_rdma->GetNicsToward(GetMyIp(), theirs);
			size_t n = std::min(mine.size(), theirs.size());
			if (n > MSCCL_MAX_RDMA_LANES) n = MSCCL_MAX_RDMA_LANES;
			if (n > 1) lanes = static_cast<uint8_t>(n);
		}
		m_rdmaLaneCount[peer] = lanes;
		return lanes;
	}

	// True if AlgoTopology::ParseSwitchJson pinned this node's connection to `peer` on any
	// channel (see GPU::PushPeerNic).
	bool CollectivesApplication::HasScheduledNicToward(int16_t peer){
		Ptr<GPU> self = DynamicCast<GPU>(GetNode());
		uint32_t pinned = RdmaQueuePair::NIC_UNPINNED;
		for (int c = 0; c < m_algo->nChannels; ++c){
			if (self->GetPeerNic(peer, c, pinned)) return true;
		}
		return false;
	}

	bool CollectivesApplication::IsRdmaPeer(int16_t peer){
		return DynamicCast<GPU>(GetNode())->HasPeerIpAddr(peer);
	}

	Ptr<RdmaDriver> CollectivesApplication::GetRdmaDriver(){
		return GetNode()->GetObject<RdmaDriver>();
	}

	Ipv4Address CollectivesApplication::GetMyIp(){
		return DynamicCast<GPU>(GetNode())->GetMyIp();
	}

	Ipv4Address CollectivesApplication::GetPeerIp(int16_t peer){
		return DynamicCast<GPU>(GetNode())->GetPeerIpAddr(peer);
	}
	uint32_t CollectivesApplication::GetPeerWin(int16_t peer){
		return DynamicCast<GPU>(GetNode())->GetPeerWin(peer);
	}
	uint64_t CollectivesApplication::GetPeerBaseRtt(int16_t peer){
		return DynamicCast<GPU>(GetNode())->GetPeerBaseRtt(peer);
	}

	MscclChannel* CollectivesApplication::GetChannel(int8_t chanId){
		return &m_channels.at(chanId);
	}

	uint16_t CollectivesApplication::AllocateRdmaSport(){
		return m_rdmaSportCounter++;
	}

	DataType::Type CollectivesApplication::GetDataType(){
		return m_dataType;
	}

	TypeId CollectivesApplication::GetSocketTypeId(){
		return m_socket_tid;
	}

/*	inline TransferState* CollectivesApplication::GetTransferState(int16_t bid, int16_t sid){
		return &m_transferStates[std::make_pair(bid, sid)];
	}*/

	DataBuffer* CollectivesApplication::GetSrcBuffer(){
		return &m_srcBuf;
	}

	DataBuffer* CollectivesApplication::GetDstBuffer(){
		return &m_dstBuf;
	}

	DataBuffer* CollectivesApplication::GetScratchBuffer(){
		return &m_scratchBuf;
	}

	void CollectivesApplication::AllocBuffer(size_t size, DataBuffer* buf){
		size_t bytes = size * DataType::GetSizeBytes(m_dataType);
		buf->dataBuffer = malloc(bytes);
		buf->len = size;
	}

	Time CollectivesApplication::GetLocalOpDelay(int8_t op){
		// TODO: add realistic delay
		return MilliSeconds(0);
	}

	void CollectivesApplication::NonTransferHandler(int16_t bid, int16_t sid, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t nElems, int8_t op){
		uint32_t bytes = nElems * DataType::GetSizeBytes(m_dataType);
		switch (op){
			case MSCCL_LOCAL_COPY:
				if (m_correctnessCheck)
					memcpy(GetBufferPtr(dstbuf, dstoff), GetBufferPtr(srcbuf, srcoff), bytes);
				break;
			default:
				NS_FATAL_ERROR("Not implemented.");
		}
		// TODO: add some fixed delay for different ops
		Simulator::Schedule(GetLocalOpDelay(op), &CollectivesApplication::StepCompletionCallback, this, bid, sid);
	}

	void CollectivesApplication::RunStep(int16_t bid, int16_t sid){
		// TODO: add realistic packet size modeling
		// Count rounds loop logic based on msccl scheduling? skipped for now
		mscclThreadBlock* tb = &(m_algo->mscclTBs[bid]);
		int8_t chanId = tb->channelId;
		MscclChannel* chan = &m_channels[chanId];
		mscclTransfer* tran = &tb->transfers[sid];
		uint16_t sendPeer = tb->sendpeer;
		uint16_t recvPeer = tb->recvpeer;
		uint32_t nElems = ((uint32_t) tran->count) * m_currChunkSize;
		uint16_t srcbuf = tran->srcbuffer;
		uint16_t dstbuf = tran->dstbuffer;
		int16_t srcoff = tran->srcoffset;
		int16_t dstoff = tran->dstoffset;
		switch (tran->type){
			case MSCCL_SEND:
				chan->Send(bid, sid, sendPeer, nElems, srcbuf, srcoff, dstbuf, dstoff, tran->mscclFlowId, tran->rate);
				break;
			case MSCCL_RECV:
				chan->Recv(bid, sid, recvPeer, nElems, dstbuf, dstoff);
				break;
			case MSCCL_RECV_COPY_SEND:
				chan->RecvCpSend(bid, sid, sendPeer, recvPeer, nElems);
				break;
			case MSCCL_RECV_REDUCE_SEND:
				chan->RecvRedSend(bid, sid, sendPeer, recvPeer, nElems);
				break;
			case MSCCL_RECV_REDUCE_COPY:
				chan->RecvRedCp(bid, sid, recvPeer, nElems, dstbuf, dstoff);
				break;
			case MSCCL_RECV_REDUCE_COPY_SEND:
				chan->RecvRedCpSend(bid, sid, sendPeer, recvPeer, nElems);
				break;
			case MSCCL_LOCAL_COPY:
				NonTransferHandler(bid, sid, srcbuf, srcoff, dstbuf, dstoff, nElems, MSCCL_LOCAL_COPY);
				break;
			case MSCCL_REDUCE:
				// TODO: need to change if count loop implemented
				m_TBStates[bid].global_step += tran->numReductions - 1;
				NonTransferHandler(bid, sid, srcbuf, srcoff, dstbuf, dstoff, nElems, MSCCL_REDUCE);
				break;
			default:
				return;
		}
	}

	void CollectivesApplication::OpenGate(int16_t gate){
		if (m_gateOpen[gate]) return; // idempotent; gates never close
		NS_LOG_DEBUG("GPU " << GetNode()->GetId() << " opening gate " << gate
			<< " (" << m_gateWaiters[gate].size() << " waiter(s))"
			<< " t=" << Simulator::Now().GetNanoSeconds());
		m_gateOpen[gate] = 1;
		// Waking a busy threadblock is safe to drop: TryScheduleNextStep is a no-op while a step
		// is in flight, but m_gateOpen is an absorbing latch re-read on every entry, and
		// StepCompletionCallback unconditionally re-calls TryScheduleNextStep when it clears
		// busy. That level-triggered property is why a gate must never be cleared mid-run.
		for (int16_t bid : m_gateWaiters[gate]){
			Simulator::ScheduleNow(&CollectivesApplication::TryScheduleNextStep, this, bid);
		}
		m_gateWaiters[gate].clear();
	}

	void CollectivesApplication::OpenGateForStep(int16_t bid, int16_t sid){
		int16_t gate = m_algo->mscclTBs[bid].transfers[sid].netGate;
		if (gate == MSCCL_GATE_NONE) return;
		OpenGate(gate);
	}

	void CollectivesApplication::TryScheduleNextStep(int16_t bid){
		mscclThreadBlock* tb = &m_algo->mscclTBs[bid];
		TBState* tbState = &m_TBStates[bid];
		uint32_t nodeId = GetNode()->GetId();
		NS_LOG_DEBUG("GPU " << nodeId << " TryScheduleNextStep TB=" << (int)bid
			<< " local_step=" << tbState->local_step
			<< " global_step=" << tbState->global_step
			<< " flag=" << tbState->flag
			<< " busy=" << tbState->busy
			<< " t=" << Simulator::Now().GetNanoSeconds());
		if (tbState->busy) return; // already scheduled next step
		int16_t sid = tbState->local_step; // sid is local step
		if (sid == tb->nsteps) return; // no more steps
		//TransferState* tState = GetTransferState(bid, sid);
		mscclTransfer* tran = &tb->transfers[sid];
		// Network gate check. This must come BEFORE the data-dependency block below, which
		// mutates state on its success path (`tbState->global_step += nDeps - 1`): a gate check
		// placed after it that returns would leave global_step bumped, and the retry would bump
		// it again, silently corrupting every downstream flag comparison on this threadblock.
		if (tran->netWait != MSCCL_GATE_NONE && !m_gateOpen[tran->netWait]){
			NS_LOG_DEBUG("GPU " << nodeId << " TB=" << (int)bid << " sid=" << sid
				<< " BLOCKED on gate " << tran->netWait
				<< " t=" << Simulator::Now().GetNanoSeconds());
			m_gateWaiters[tran->netWait].insert(bid);
			return;
		}
		// int16_t nDeps = tState->nPendingDeps;
		int16_t nDeps = tran->numDependences;
		// int16_t firstDepId = tState->firstPendingDep;
		int16_t firstDepId = tran->depencePointer;
		if (nDeps > 0) {
			for (int dep = firstDepId; dep < firstDepId + nDeps; ++dep){
				int16_t depbid = tb->dependentBid[dep];
				int16_t depsid = tb->dependentStep[dep]; // depsid is global step
				int64_t depflag = COMPUTE_FLAG(m_currWorkId, m_currIter, depsid);
				TBState* depTB = &m_TBStates[depbid];
				if (depflag > depTB->flag) {
					NS_LOG_DEBUG("GPU " << nodeId << " TB=" << (int)bid << " sid=" << sid
						<< " BLOCKED on depTB=" << (int)depbid
						<< " depsid=" << depsid
						<< " depflag=" << depflag
						<< " depTB->flag=" << depTB->flag
						<< " t=" << Simulator::Now().GetNanoSeconds());
					// tell depTB to try reschedule when its flag changes
					depTB->tryReschedule.insert(bid);
					return; // cannot schedule
				}
				NS_LOG_DEBUG("GPU " << nodeId << " TB=" << (int)bid << " sid=" << sid
					<< " dep satisfied: depTB=" << (int)depbid
					<< " depsid=" << depsid
					<< " depflag=" << depflag
					<< " depTB->flag=" << depTB->flag
					<< " t=" << Simulator::Now().GetNanoSeconds());
				// no need to re-check previous deps next time
				// commented out for correct global_step update
				// tState->firstPendingDep++;
				// tState->nPendingDeps--;
			}
			tbState->global_step += nDeps - 1; // tracks step, don't update flag until completion of step

		}
		NS_LOG_DEBUG("GPU " << nodeId << " TB=" << (int)bid << " sid=" << sid
			<< " DISPATCHING RunStep t=" << Simulator::Now().GetNanoSeconds());
		Simulator::ScheduleNow(&CollectivesApplication::RunStep, this, bid, sid);
		m_TBStates[bid].busy = true; // set flag to prevent multiple schedulings
	}

	void CollectivesApplication::StepCompletionCallback(int16_t bid, int16_t sid){
		// TransferState* tState = GetTransferState(bid, sid);
		// mscclTransfer* trans = &m_algo->mscclTBs[bid].transfers[sid];
		// update TBState
		TBState* tbState = &m_TBStates[bid];
		tbState->busy = false;
		tbState->local_step++;
		tbState->flag = (uint64_t) COMPUTE_FLAG(m_currWorkId, m_currIter, tbState->global_step); // flag update
		tbState->global_step++;
		Simulator::ScheduleNow(&CollectivesApplication::TryScheduleNextStep, this, bid);
		for (int16_t depTB : tbState->tryReschedule){
			Simulator::ScheduleNow(&CollectivesApplication::TryScheduleNextStep, this, depTB);
		}
		tbState->tryReschedule.clear();
	}

	void CollectivesApplication::InterpretAlgo(){
		if (!m_algo->isValid){
			NS_FATAL_ERROR("No valid algorithm found.");
		}
		Bootstrap();

		// Network gates (see mscclTransfer::netGate/netWait). Size the gate state from the
		// highest gate id the parser saw on this GPU -- maxNetGate is -1 when the schedule uses
		// no gates, which leaves both vectors empty and the gate check in TryScheduleNextStep
		// unreachable.
		m_gateOpen.assign(m_algo->maxNetGate + 1, 0);
		m_gateWaiters.assign(m_algo->maxNetGate + 1, std::unordered_set<int16_t>());
		// Gates are opened only on the RDMA path (MscclChannel::OnRdmaSendComplete), so a gate
		// on a socket-path op would never open and would hang the run. Peer classification is
		// already known here -- RdmaFabricHelper populates the peer ip table at topology-build
		// time -- so this fails at app start, before any simulation time passes. Every peer is
		// RDMA in the current topologies; this exists to make the constraint explicit.
		if (m_algo->maxNetGate != MSCCL_GATE_NONE){
			for (int16_t bid = 0; bid < m_algo->nBlocks; ++bid){
				mscclThreadBlock* tb = &m_algo->mscclTBs[bid];
				for (uint16_t sid = 0; sid < tb->nsteps; ++sid){
					mscclTransfer* tran = &tb->transfers[sid];
					if (tran->netGate == MSCCL_GATE_NONE && tran->netWait == MSCCL_GATE_NONE) continue;
					if (tb->sendpeer < 0 || !IsRdmaPeer(tb->sendpeer)){
						NS_FATAL_ERROR("Node " << GetNode()->GetId() << " TB " << (int)bid << " step " << sid
							<< " carries a network gate (netGate=" << tran->netGate << ", netWait=" << tran->netWait
							<< ") but its sendpeer (" << tb->sendpeer << ") is not an RDMA peer. Gates are only"
							<< " opened on the RDMA path, so this schedule would hang.");
					}
				}
			}
		}

		for (int16_t bid = 0; bid < m_algo->nBlocks; ++bid){
			// mscclThreadBlock* tb = &m_algo->mscclTBs[bid];
			m_TBStates.emplace(bid, TBState(bid)); // built TBStates
			/* for (int16_t local_step = 0; local_step < tb->nsteps; ++local_step){
				mscclTransfer* trans = &tb->transfers[local_step];
				int nDeps = trans->numDependences;
				TransferState* tState = GetTransferState(bid, local_step);
				tState->firstPendingDep = trans->depencePointer;
				tState->nPendingDeps = nDeps;
				if (nDeps > 0) {
					for (int dep = trans->depencePointer; dep < trans->depencePointer + nDeps; ++dep){
						int16_t depbid = tb->dependentBid[dep];
						int16_t depsid = tb->dependentStep[dep]; // depsid is global step
						tState->dependentTBs.insert(depbid);
					}
				}
				// m_transferStates.emplace(std::make_pair(bid, local_step), TransferState(trans->depencePointer, trans->numDependences));
			} // build transferStates */
			// try scheduling first step for every TB
			Simulator::ScheduleNow(&CollectivesApplication::TryScheduleNextStep, this, bid);
		}
	}



	void CollectivesApplication::Bootstrap(){
		for (int i = 0; i < m_algo->nChannels; ++i){
			mscclChannelInfo* chanInfo = &(m_algo->mscclChannels[i]);
			m_channels.emplace(i, MscclChannel(i, this));
			MscclChannel* chan = &m_channels[i];
			#ifdef FLOW_ID_TEST
			chan->SetFlowIdTable(m_flowIds);
			#endif
			//chan->SetupListener();
			// RDMA peers are bootstrapped by RdmaHw/RdmaDriver and complete via
			// OnRdmaSendComplete, so they have no PacketSocket device/address
			// registered and must skip socket setup here.
			for (int r = 0; r < chanInfo->nRecvPeers; ++r){
				int16_t peer = chanInfo->recvPeerInfo[r].peer;
				if (!IsRdmaPeer(peer)) chan->SetupRecvPeer(peer);
			}
			for (int s = 0; s < chanInfo->nSendPeers; ++s){
				int16_t peer = chanInfo->sendPeerInfo[s].peer;
				if (!IsRdmaPeer(peer)) chan->ConnectSendPeer(peer);
			}
		}
		// RDMA peers must be set up after every node's m_channels above has been
		// constructed (SetupRdmaSendPeer reaches into peerApp->GetChannel(m_id)), which is
		// only guaranteed once every node's own synchronous Bootstrap() has run -- see
		// SetupRdmaPeers for why deferring via ScheduleNow makes this safe across nodes.
		Simulator::ScheduleNow(&CollectivesApplication::SetupRdmaPeers, this);
	}

	void CollectivesApplication::SetupRdmaPeers(){
		for (int i = 0; i < m_algo->nChannels; ++i){
			mscclChannelInfo* chanInfo = &(m_algo->mscclChannels[i]);
			MscclChannel* chan = &m_channels[i];
			for (int s = 0; s < chanInfo->nSendPeers; ++s){
				int16_t peer = chanInfo->sendPeerInfo[s].peer;
				if (IsRdmaPeer(peer)) chan->SetupRdmaSendPeer(peer);
			}
		}
	}

	Time CollectivesApplication::GetTxTime(Ptr<NetDevice> dev, uint32_t bytes){
		DataRateValue drValue;
		if (dev->GetAttributeFailSafe("DataRate", drValue)){
			return drValue.Get().CalculateBytesTxTime(bytes);
		}
		// Some devices (e.g. SwitchedEthernetHostDevice) store DataRate on the
		// channel rather than on the device itself
		Ptr<Channel> ch = dev->GetChannel();
		if (ch && ch->GetAttributeFailSafe("DataRate", drValue)){
			return drValue.Get().CalculateBytesTxTime(bytes);
		}
		return Time(0);
	}

	void CollectivesApplication::QueueFragmentsForDevice(Ptr<NetDevice> dev, std::queue<PendingFragment> frags){
		std::queue<PendingFragment>& fragQueue = m_pendingFragments[dev];
		while (!frags.empty()){
			fragQueue.push(std::move(frags.front()));
			frags.pop();
		}
		// kick off pacing if this device isn't already draining its fragment queue
		if (!m_sendInFlight[dev]){
			m_sendInFlight[dev] = true;
			SendNextFragment(dev);
		}
	}

	void CollectivesApplication::SendNextFragment(Ptr<NetDevice> dev){
		std::queue<PendingFragment>& fragQueue = m_pendingFragments.at(dev);
		if (fragQueue.empty()){
			m_sendInFlight[dev] = false;
			return;
		}
		PendingFragment frag = fragQueue.front();
		fragQueue.pop();

		// capture size before sock->Send, which may let the device add its own header
		uint32_t wireSize = frag.packet->GetSize();

		int result = frag.sock->Send(frag.packet, 0);
		if (result < 0){
			NS_FATAL_ERROR("Node " << GetNode()->GetId()
				<< ": sock->Send() failed (returned " << result << ")"
				<< " wireSize=" << wireSize
				<< " txAvail=" << frag.sock->GetTxAvailable());
		}

		if (!fragQueue.empty()){
			// pad by the same L2 overhead margin used to size fragments in Send(), so our
			// pacing never runs ahead of the device's actual transmission time (which
			// includes link-layer headers we don't account for in wireSize). Otherwise
			// the small per-fragment drift accumulates across the simulation and
			// eventually overflows the device's TX queue.
			Simulator::Schedule(GetTxTime(dev, wireSize + MSCCL_L2_OVERHEAD_BYTES), &CollectivesApplication::SendNextFragment, this, dev);
		} else {
			m_sendInFlight[dev] = false;
		}
	}

	#ifdef FLOW_ID_TEST
	/*
	void CollectivesApplication::SetFlowIdTableForChannel(std::map<std::pair<int, int>, uint32_t>* table, int channel){
		m_channels[channel].SetFlowIdTable(table);
	}
	void CollectivesApplication::SetFlowIdTableForAllChannels(std::map<std::pair<int, int>, uint32_t>* table){
		for (auto cur = m_channels.begin(); cur != m_channels.end(); ++cur){
			cur->second.SetFlowIdTable(table);
		}
	}*/
	void CollectivesApplication::StoreFlowIdTable(std::map<std::pair<int, int>, uint32_t>* table){
		m_flowIds = table;
	}
	#endif

	void* CollectivesApplication::GetBufferPtrRawBytes(uint16_t buf, size_t offset){
		DataBuffer buffer;
		switch (buf){
			case MSCCL_INPUT_BUFFER:
				buffer = m_srcBuf;
				break;
			case MSCCL_OUTPUT_BUFFER:
				buffer = m_dstBuf;
				break;
			case MSCCL_SCRATCH_BUFFER:
				buffer = m_scratchBuf;
				break;
			default:
				NS_FATAL_ERROR("Unrecognized buffer type");
		}
		if (offset > buffer.len * DataType::GetSizeBytes(m_dataType))
			NS_FATAL_ERROR("Invalid offset: buf=" << buf << " offset=" << offset << " bytes, buffer holds "
				<< (buffer.len * DataType::GetSizeBytes(m_dataType)) << " bytes ("
				<< buffer.len << " elems, chunk=" << m_currChunkSize
				<< " elems). The buffer is smaller than the algorithm's declared chunk count.");
		return ((uint8_t*)buffer.dataBuffer) + offset;
	}

	void* CollectivesApplication::GetBufferPtr(uint16_t buf, int16_t offset){
		return GetBufferPtrRawBytes(buf, offset * m_currChunkSize * (DataType::GetSizeBytes(m_dataType)));
	}

	void CollectivesApplication::DumpBuffer(DataBuffer* buf, std::ostream& log_txt){
		void* data = buf->dataBuffer;
		size_t count = buf->len;
		log_txt << "On Node " << GetNode() << ":\n";
		log_txt << "Dumping " << std::dec << count << " elements\n";
    log_txt << std::hex << std::setfill('0');

    for (size_t i = 0; i < count; ++i) {

        log_txt << "Index " << std::dec << i << " | ";

        switch (m_dataType) {

        case DataType::INT16: {
            const int16_t* ptr = static_cast<const int16_t*>(data);
            int16_t val = ptr[i];
            log_txt << "INT16 | Addr: "
                    << static_cast<const void*>(&ptr[i])
                    << " | Val: " << std::dec << val
                    << " | Hex: 0x"
                    << std::setw(4) << std::hex
                    << static_cast<uint16_t>(val)
                    << "\n";
            break;
        }

        case DataType::INT32: {
            const int32_t* ptr = static_cast<const int32_t*>(data);
            int32_t val = ptr[i];
            log_txt << "INT32 | Addr: "
                    << static_cast<const void*>(&ptr[i])
                    << " | Val: " << std::dec << val
                    << " | Hex: 0x"
                    << std::setw(8) <<std::hex
                    << static_cast<uint32_t>(val)
                    << "\n";
            break;
        }

        case DataType::FLOAT32: {
            const float* ptr = static_cast<const float*>(data);
            float val = ptr[i];

            uint32_t bits;
            std::memcpy(&bits, &val, sizeof(bits));

            log_txt << "FLOAT32 | Addr: "
                    << static_cast<const void*>(&ptr[i])
                    << " | Val: " << std::dec << val
                    << " | Hex: 0x"
                    << std::hex << std::setw(8) << bits
                    << "\n";
            break;
        }

        case DataType::FLOAT64: {
            const double* ptr = static_cast<const double*>(data);
            double val = ptr[i];

            uint64_t bits;
            std::memcpy(&bits, &val, sizeof(bits));

            log_txt << "FLOAT64 | Addr: "
                    << static_cast<const void*>(&ptr[i])
                    << " | Val: " << std::dec << val
                    << " | Hex: 0x"
                    << std::hex << std::setw(16) << bits
                    << "\n";
            break;
        }
        }
    }

    log_txt << "---- End Dump ----\n";
	}


	void CollectivesApplication::StartApplication(){
		InterpretAlgo();
	}

	void CollectivesApplication::StopApplication(){
		// channels cleanup
		for (auto& pair : m_channels){
			pair.second.Close();
		}
		// tb states checks
		for (auto& pair : m_TBStates){
			mscclThreadBlock* tb = &m_algo->mscclTBs[pair.first];
			if (pair.second.busy || pair.second.local_step < tb->nsteps){
				// Name the gate too when the tb is parked on one: a gate that never opens drains
				// the event queue and returns a plausible-looking wrong answer otherwise.
				std::ostringstream gateInfo;
				if (pair.second.local_step < tb->nsteps){
					int16_t netWait = tb->transfers[pair.second.local_step].netWait;
					if (netWait != MSCCL_GATE_NONE){
						gateInfo << " Parked on gate " << netWait << " (open=" << (int)m_gateOpen[netWait] << ").";
					}
				}
				NS_FATAL_ERROR("BUG: TB " << pair.first << " on node " << GetNode()->GetId() << " not finished at application close. Has " << tb->nsteps << " steps, at step " << pair.second.local_step << "." << gateInfo.str());
			}
		}
	}

} // namespace ns3
