#include "collectives.h"

#include <iomanip>

#define MSCCL_MAX_ITER 65536
// L2 overhead (e.g. PPP/eth header) added by the NetDevice on top of our packet,
// reserved when sizing fragments and used to keep fragment pacing from
// outrunning the device's actual transmission time
#define MSCCL_L2_OVERHEAD_BYTES 14

// RDMA-fabric transport constants: sport is allocated per (channel, send call) to keep
// RdmaHw's (dip,sport,pg) queue-pair key unique across concurrent in-flight transfers to
// the same peer; dport/pg are fixed since uniqueness only needs to hold on sport.
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
			// TODO: properly consider cross recv op boundary packets
			// tmp buffer and offset for cross recv op boundary packets
			uint8_t* tmp = (uint8_t*) malloc(recvSize);
			packet->CopyData(tmp, recvSize);
			size_t tmp_offset = 0;
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
			// copy/reduce fragment into destination at the byte offset encoded in the header
			std::pair<uint16_t, uint16_t> dstInfo(hdr.GetDstBuf(), hdr.GetDstOff());
			if (m_app->GetCorrectnessCheck()){
				uint8_t* dst = (uint8_t*) m_app->GetBufferPtr(dstInfo.first, dstInfo.second);
				bool isRrc = m_pendingRecvByBufferRegion.count(dstInfo) > 0 &&
				             m_pendingRecvByBufferRegion[dstInfo].op == MSCCL_RECV_REDUCE_COPY;
				if (isRrc){
					ReduceAdd(dst + hdr.GetFragByteOffset(), tmp + tmp_offset, recvSize, m_dataType);
				} else {
					memcpy(dst + hdr.GetFragByteOffset(), tmp + tmp_offset, recvSize);
				}
			}

			// accumulate received bytes; complete only when the full logical transfer is done
			m_recvBytesAccum[dstInfo] += recvSize;
			NS_LOG_DEBUG("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": recv frag from " << hdr.GetSrcGpu() << " dstInfo=(" << dstInfo.first << "," << dstInfo.second << ") fragOff=" << hdr.GetFragByteOffset() << " fragBytes=" << recvSize << " accum=" << m_recvBytesAccum[dstInfo] << "/" << hdr.GetBytes());
			if (m_recvBytesAccum[dstInfo] < hdr.GetBytes()){
				free(tmp);
				continue;
			}
			if (m_recvBytesAccum[dstInfo] > hdr.GetBytes()){
				NS_FATAL_ERROR("Node " << m_app->GetNode()->GetId() << ": accumulator overshot for dstInfo=(" << dstInfo.first << "," << dstInfo.second << "): got " << m_recvBytesAccum[dstInfo] << " expected " << hdr.GetBytes() << ". Possible key collision between concurrent transfers.");
			}
			NS_LOG_INFO("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": transfer complete from " << hdr.GetSrcGpu() << " dstInfo=(" << dstInfo.first << "," << dstInfo.second << ") totalBytes=" << hdr.GetBytes());
			m_recvBytesAccum.erase(dstInfo);

			NotifyTransferArrived(dstInfo.first, dstInfo.second);
			/* std::queue<PendingTransfer>& recvQueue = m_pendingRecvs.at(peerId);
			while (recvSize > 0 && !recvQueue.empty()){
				auto& cur = recvQueue.front();
				uint32_t take = std::min(recvSize, cur.pendingBytes);
				cur.pendingBytes -= take;
				void* dst = ((uint8_t*) m_app->GetBufferPtr(cur.dstBuf, cur.dstOffset)) + cur.receivedBytes;
				memcpy(dst, tmp + tmp_offset, take);
				cur.receivedBytes += take;
				tmp_offset += take;
				recvSize -= take;
				if (cur.pendingBytes == 0){
					// logical packet fully received
					recvQueue.pop();
					switch (cur.op){
						case MSCCL_RECV:
							Simulator::ScheduleNow(&CollectivesApplication::StepCompletionCallback, m_app, cur.bid, cur.sid);
							break;
						// case MSCCL_RECV_COPY_SEND:
						// case MSCCL_RECV_REDUCE_SEND:
						default:
							NS_FATAL_ERROR("not implemented");
					}
				}
			} */
			free(tmp);
		}
	}

	bool MscclChannel::IsPendingReduceCopy(uint16_t dstbuf, uint16_t dstoff){
		std::pair<uint16_t, uint16_t> dstInfo(dstbuf, dstoff);
		return m_pendingRecvByBufferRegion.count(dstInfo) > 0 &&
		       m_pendingRecvByBufferRegion[dstInfo].op == MSCCL_RECV_REDUCE_COPY;
	}

	// shared completion-trigger logic for the socket RecvCallback and the cross-node
	// RDMA OnRdmaSendComplete: either complete a matching pending recv, or mark the
	// buffer region ready for when Recv()/RecvRedCp() is later called.
	void MscclChannel::NotifyTransferArrived(uint16_t dstbuf, uint16_t dstoff){
		std::pair<uint16_t, uint16_t> dstInfo(dstbuf, dstoff);
		if (m_pendingRecvByBufferRegion.count(dstInfo) > 0){
			auto& cur = m_pendingRecvByBufferRegion[dstInfo];
			switch (cur.op){
				case MSCCL_RECV:
				case MSCCL_RECV_REDUCE_COPY:
					Simulator::ScheduleNow(&CollectivesApplication::StepCompletionCallback, m_app, cur.bid, cur.sid);
					break;
				default:
					NS_FATAL_ERROR("not implemented");
			}
			m_pendingRecvByBufferRegion.erase(dstInfo);
			return;
		}
		if (m_recvReadyByBufferRegion.count(dstInfo) > 0 && m_recvReadyByBufferRegion[dstInfo] == true){
			NS_FATAL_ERROR("Received multiple packets for same dst region. Check scheduling bugs or duplicate packet in network");
		}
		m_recvReadyByBufferRegion[dstInfo] = true;
	}

	void MscclChannel::SetPendingRecv(uint16_t dstbuf, int16_t dstoff, PendingTransfer recv){
		if (dstoff < 0) NS_FATAL_ERROR("Invalid dst offset");
		m_pendingRecvByBufferRegion[std::make_pair(dstbuf, static_cast<uint16_t>(dstoff))] = recv;
	}

	void MscclChannel::PushPendingSend(Ptr<Socket> sock, PendingTransfer send){
		m_pendingSends[sock].push(send);
	}

	void MscclChannel::Send(int8_t bid, int16_t sid, int16_t sendpeer, uint32_t nElems, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t mscclFlowId){
		if (sendpeer < 0){
			NS_FATAL_ERROR("Send peer is negative in Send");
		}
		if (dstoff < 0){
			NS_FATAL_ERROR("Invalid dst offset in Send");
		}
		if (m_app->IsRdmaPeer(sendpeer)){
			SendRdma(bid, sid, sendpeer, nElems, srcbuf, srcoff, dstbuf, dstoff, mscclFlowId);
			return;
		}
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

	void MscclChannel::SendRdma(int8_t bid, int16_t sid, int16_t sendpeer, uint32_t nElems, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t mscclFlowId){
		uint32_t totalBytes = nElems * DataType::GetSizeBytes(m_dataType);
		// distinct sport per send call to keep RdmaHw's (dip,sport,pg) qp key unique
		// across concurrent in-flight transfers to the same peer on this channel
		uint16_t sport = static_cast<uint16_t>(MSCCL_RDMA_SPORT_BASE + (m_rdmaSportCounter++));

		// ns-3's Callback<void> can't wrap a capturing lambda here (FunctorCallbackImpl
		// requires operator!= on the functor, which closures don't have), so bind the
		// context onto a member-function Callback one argument at a time instead.
		Callback<void> finishCb = MakeCallback(&MscclChannel::OnRdmaSendComplete, this)
			.Bind(bid).Bind(sid).Bind(sendpeer).Bind(srcbuf).Bind(srcoff).Bind(dstbuf).Bind(dstoff).Bind(nElems);

		// RdmaHw owns pacing/congestion-control/PFC end-to-end; no manual fragmentation needed.
		// win/baseRtt bound in-flight bytes to the path's bandwidth-delay product
		// (mirrors astra-sim's pairBdp/pairRtt) -- without this RdmaQueuePair::IsWinBound()
		// never trips and the flow streams unbounded at full line rate.
		NS_LOG_INFO("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": RDMA send to "
			<< sendpeer << " totalBytes=" << totalBytes << " win=" << m_app->GetPeerWin(sendpeer)
			<< " baseRtt=" << m_app->GetPeerBaseRtt(sendpeer) << " at t=" << Simulator::Now().GetNanoSeconds());
		// mscclFlowId comes from the XML algorithm's per-step "mscclflowid"
		// attribute (parsed in algo_topology.cc); MSCCL_FLOW_ID_NONE if the step didn't
		// assign one, in which case switches fall back to plain ECMP for this qp
		m_app->GetRdmaDriver()->AddQueuePair(
			m_app->GetNode()->GetId(), static_cast<uint32_t>(sendpeer), /* tag */ 0, totalBytes, MSCCL_RDMA_PG,
			m_app->GetMyIp(), m_app->GetPeerIp(sendpeer), sport, MSCCL_RDMA_DPORT,
			m_app->GetPeerWin(sendpeer), m_app->GetPeerBaseRtt(sendpeer), mscclFlowId, finishCb, Callback<void>());
	}

	void MscclChannel::OnRdmaSendComplete(int8_t bid, int16_t sid, int16_t sendpeer, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t nElems){
		Ptr<Node> peerNode = NodeList::GetNode(static_cast<uint32_t>(sendpeer));
		Ptr<CollectivesApplication> peerApp = DynamicCast<CollectivesApplication>(peerNode->GetApplication(0));
		MscclChannel* peerChan = peerApp->GetChannel(m_id);
		uint16_t dstoffU = static_cast<uint16_t>(dstoff);

		if (m_app->GetCorrectnessCheck()){
			uint32_t bytes = nElems * DataType::GetSizeBytes(m_dataType);
			uint8_t* src = (uint8_t*) m_app->GetBufferPtr(srcbuf, srcoff);
			uint8_t* dst = (uint8_t*) peerApp->GetBufferPtr(dstbuf, dstoff);
			if (peerChan->IsPendingReduceCopy(dstbuf, dstoffU)){
				ReduceAdd(dst, src, bytes, m_dataType);
			} else {
				memcpy(dst, src, bytes);
			}
		}
		NS_LOG_INFO("Node " << m_app->GetNode()->GetId() << " chan " << (int)m_id << ": RDMA send to " << sendpeer << " complete, dstInfo=(" << dstbuf << "," << dstoffU << ") at t=" << Simulator::Now().GetNanoSeconds());
		peerChan->NotifyTransferArrived(dstbuf, dstoffU);
		Simulator::ScheduleNow(&CollectivesApplication::StepCompletionCallback, m_app, bid, sid);
	}

	void MscclChannel::Recv(int8_t bid, int16_t sid, int16_t recvpeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff){
		if (dstoff < 0) NS_FATAL_ERROR("Invalid offset");
		PendingTransfer recv(bid, sid, nElems * DataType::GetSizeBytes(m_dataType), MSCCL_RECV, 0, -1, dstbuf, dstoff);
		std::pair<uint16_t, uint16_t> dstinfo(dstbuf, static_cast<uint16_t> (dstoff));
		// if we have already received
		if (m_recvReadyByBufferRegion.count(dstinfo) > 0 && m_recvReadyByBufferRegion[dstinfo] == true){
			Simulator::ScheduleNow(&CollectivesApplication::StepCompletionCallback, m_app, bid, sid);
			m_recvReadyByBufferRegion[dstinfo] = false;
			return;
		}
		// else note this pending recv
		SetPendingRecv(dstbuf, dstoff, recv);
		// StepCompletion triggered during recv callback
	}

	void MscclChannel::RecvCpSend(int8_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems){
	NS_FATAL_ERROR("RecvCpSend not yet implemented");
	}

	void MscclChannel::RecvRedSend(int8_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems){
	NS_FATAL_ERROR("RecvRedSend not yet implemented");
	}

	void MscclChannel::RecvRedCp(int8_t bid, int16_t sid, int16_t recvpeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff){
		if (dstoff < 0) NS_FATAL_ERROR("Invalid offset");
		PendingTransfer recv(bid, sid, nElems * DataType::GetSizeBytes(m_dataType), MSCCL_RECV_REDUCE_COPY, 0, -1, dstbuf, dstoff);
		std::pair<uint16_t, uint16_t> dstinfo(dstbuf, static_cast<uint16_t>(dstoff));
		if (m_recvReadyByBufferRegion.count(dstinfo) > 0 && m_recvReadyByBufferRegion[dstinfo] == true){
			Simulator::ScheduleNow(&CollectivesApplication::StepCompletionCallback, m_app, bid, sid);
			m_recvReadyByBufferRegion[dstinfo] = false;
			return;
		}
		SetPendingRecv(dstbuf, dstoff, recv);
	}

	void MscclChannel::RecvRedCpSend(int8_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems){
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
		/*for (auto& recvQueue : m_pendingRecvs){
			if (!recvQueue.second.empty()){
				NS_FATAL_ERROR("BUG: has pending recv on node " << m_app->GetNode()->GetId() << " channel " << m_id << " at application close.");
			}
		}*/
		// socket close
		if (m_listenSocket) m_listenSocket->Close();
		for (auto& pair: m_sendPeerSockets){
			pair.second->Close();
		}
		for (auto& pair: m_recvSocketPeers){
			pair.first->Close();
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

	DataType::Type CollectivesApplication::GetDataType(){
		return m_dataType;
	}

	TypeId CollectivesApplication::GetSocketTypeId(){
		return m_socket_tid;
	}

/*	inline TransferState* CollectivesApplication::GetTransferState(int8_t bid, int16_t sid){
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

	void CollectivesApplication::NonTransferHandler(int8_t bid, int16_t sid, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t nElems, int8_t op){
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

	void CollectivesApplication::RunStep(int8_t bid, int16_t sid){
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
				chan->Send(bid, sid, sendPeer, nElems, srcbuf, srcoff, dstbuf, dstoff, tran->mscclFlowId);
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

	void CollectivesApplication::TryScheduleNextStep(int8_t bid){
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

	void CollectivesApplication::StepCompletionCallback(int8_t bid, int16_t sid){
		// TransferState* tState = GetTransferState(bid, sid);
		// mscclTransfer* trans = &m_algo->mscclTBs[bid].transfers[sid];
		// update TBState
		TBState* tbState = &m_TBStates[bid];
		tbState->busy = false;
		tbState->local_step++;
		tbState->flag = (uint64_t) COMPUTE_FLAG(m_currWorkId, m_currIter, tbState->global_step); // flag update
		tbState->global_step++;
		Simulator::ScheduleNow(&CollectivesApplication::TryScheduleNextStep, this, bid);
		for (int8_t depTB : tbState->tryReschedule){
			Simulator::ScheduleNow(&CollectivesApplication::TryScheduleNextStep, this, depTB);
		}
		tbState->tryReschedule.clear();
	}

	void CollectivesApplication::InterpretAlgo(){
		if (!m_algo->isValid){
			NS_FATAL_ERROR("No valid algorithm found.");
		}
		Bootstrap();
		for (int8_t bid = 0; bid < m_algo->nBlocks; ++bid){
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
						int8_t depbid = tb->dependentBid[dep];
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
		if (offset < 0 || offset > buffer.len * DataType::GetSizeBytes(m_dataType)) NS_FATAL_ERROR("Invalid offset");
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
				NS_FATAL_ERROR("BUG: TB " << pair.first << " on node " << GetNode()->GetId() << " not finished at application close. Has " << tb->nsteps << " steps, at step " << pair.second.local_step);
			}
		}
	}

} // namespace ns3
