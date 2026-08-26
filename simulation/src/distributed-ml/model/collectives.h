#ifndef COLL_H
#define COLL_H

#include "ns3/core-module.h"
#include "ns3/applications-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/rdma-driver.h"
#include "ns3/node-list.h"
#include "gpu.h"
#include "msccl.h"
#include "utils.h"
#include "msccl-header.h"

#include <map>
#include <vector>
#include <utility>
#include <unordered_set>
#include <queue>
#include <tuple>
#include <fstream>
#include <iostream>

// #define FLOW_ID_TEST
// for temporary testing with inserting flow ID here
// to be disabled when such info properly encoded in xml



/*
TODO: model work index for sequences of ops
The `iter` component of the flag tuple is the MSCCL kernel's gridOffset loop -- see
TBState::iter and CollectivesApplication::m_nLoops.
Note: msccl uses (work_index, iter, step) flag for dependency tracking, where step is tracked UNIVERSALLY across all TBs-
if TB1 step0 and TB2 step0 both have same dependiences on TB0 step2, and TB0 steps have no dependencies on other TBs,
then TB1 step0 and TB2 step0 both have universal step id 3.
this is reflected by the flags here and global_step vars. Local step is also used for map keys; sid when used for indexing/key refers to local step
*/
namespace ns3 {
	class CollectivesApplication; // forward decl

	struct TBState {
		int16_t bid;
		int64_t global_step;
		int16_t local_step;
		// Which pipeline iteration (the kernel's gridOffset loop counter) this threadblock is
		// executing. Deliberately per-threadblock rather than per-app: the whole point of the
		// gridOffset loop is that there is no barrier between iterations, so a relay TB may
		// still be forwarding slice 0 while its producer has already moved on to slice 1.
		// Both global_step and local_step restart at 0 on every iteration, mirroring the
		// kernel's `int step = 0` inside the gridOffset loop -- dependence targets in the XML
		// are expressed in that per-iteration step numbering. `flag` stays monotone across the
		// rollover because COMPUTE_FLAG orders (iter, step) lexicographically.
		uint32_t iter;
		int64_t flag;
		// The wire counterpart of `flag`. `flag` publishes a step the moment it completes
		// locally -- for a send that is when the message was handed to the transport, not when
		// its bytes left. `netFlag` publishes it only once the RDMA message has physically
		// drained, and is what netdepid/netdeps are checked against. Monotone in (iter, step)
		// under COMPUTE_FLAG exactly like `flag`, so it needs no special handling across the
		// iteration rollover.
		int64_t netFlag;
		bool busy;
		std::unordered_set<int16_t> tryReschedule; // TBs that should try rescheduling when this TB reaches flag
		std::unordered_set<int16_t> netTryReschedule; // ... and when it reaches netFlag
		TBState(): bid(-1), global_step(0), local_step(0), iter(0), flag(-1), netFlag(-1), busy(false){}
		TBState(int16_t id): bid(id), global_step(0), local_step(0), iter(0),
      flag(-1), netFlag(-1), busy(false){}
	};

/*	struct TransferState{
		int16_t firstPendingDep;
		int16_t nPendingDeps;
		std::unordered_set<int8_t> dependentTBs; // needing retry scheduling
		TransferState(): firstPendingDep(-1), nPendingDeps(-1){}
		TransferState(int16_t firstDep, int16_t nDeps): firstPendingDep(firstDep), nPendingDeps(nDeps){}
	}; */

	struct PendingTransfer{
		int16_t bid;
		int16_t sid; //local sid
		uint32_t receivedBytes;
		uint32_t pendingBytes;
		int8_t op;
		uint16_t srcBuf;
		int16_t srcOffset;
		uint16_t dstBuf;
		int16_t dstOffset;
		// lazily allocated when op==MSCCL_RECV_REDUCE_COPY and correctness check is enabled:
		// holds raw incoming bytes until pendingBytes is reached, then a single ReduceAdd is
		// applied and the buffer freed. Deferring the reduce to completion (rather than doing
		// it per-fragment) avoids splitting a data element across two fragments, since RDMA
		// packet boundaries aren't guaranteed aligned to the element size the way the socket
		// path's Send() fragmentation is.
		uint8_t* scratchBuf;
		// Byte offset of this lane's slice within the transfer's destination range. Zero for a
		// single-lane transfer; with NIC merging a transfer is cut into one contiguous slice per
		// lane (see MscclChannel::LaneSlice) and each lane's bytes land at dstOffset + this.
		uint32_t dstByteShift;
		// Byte offset of the pipeline slice this transfer belongs to, within each msccl chunk
		// (the kernel's `gridOffset`, in bytes). Composes with dstByteShift: bytes land at
		// dstOffset*chunkBytes + gridByteOff + dstByteShift + receivedBytes. Zero when
		// pipelining is off, which is why every buffer address below reduces to the old one.
		uint32_t gridByteOff;
		// Which sub-transfer of the maxAllowedCount loop this is (see
		// CollectivesApplication::m_maxAllowedCount). A step moving `count` chunks is emitted as
		// several messages, and each needs its own lane countdown, so this is part of the key
		// into m_recvLanesLeft. 0 when the step is not split.
		uint16_t part;
		PendingTransfer(): bid(-1), sid(-1), receivedBytes(0), pendingBytes(0), op(-1), srcBuf(3), srcOffset(-1), dstBuf(3), dstOffset(-1), scratchBuf(nullptr), dstByteShift(0), gridByteOff(0), part(0){}
		PendingTransfer(int16_t bId, int16_t sId, uint32_t bytes, int8_t Op, uint16_t srcbuf, uint16_t srcoff, uint16_t dstbuf, int16_t dstoff): bid(bId), sid(sId),
										receivedBytes(0), pendingBytes(bytes), op(Op), srcBuf(srcbuf), srcOffset(srcoff), dstBuf(dstbuf), dstOffset(dstoff), scratchBuf(nullptr), dstByteShift(0), gridByteOff(0), part(0){}
	};

	// bytes that arrived on a peer's connection before a matching Recv()/RecvRedCp() was
	// posted by this node's own step interpreter -- the symmetric counterpart to
	// PendingTransfer for the "data-first" ordering case. Modeled as one continuous,
	// undifferentiated byte stream per peer (like a raw socket stream), not a queue of
	// discretely-sized transfers: nothing about transfer boundaries crosses the wire, so
	// the receiver has no way to know them in advance and doesn't need to -- a future
	// Recv()/RecvRedCp() call simply claims exactly the byte count it locally expects off
	// the front, in FIFO order. If a rank's algorithm XML miscounts elements relative to
	// its peer, the stream desyncs from that point on (silently, exactly as it would on
	// real hardware) rather than being caught by any cross-check here.
	struct UnclaimedBytes{
		uint32_t gotBytes; // bytes arrived from this peer but not yet claimed by a Recv()/RecvRedCp() call
		uint8_t* stagingBuf; // nullptr if correctness check disabled; grows (realloc) as bytes arrive
		UnclaimedBytes(): gotBytes(0), stagingBuf(nullptr){}
	};

	// How a connection picks the NIC it is bound to. Deliberately an explicit three-way choice
	// rather than a bool: NIC selection used to be a side effect of whether the switch JSON was
	// parsed, which fused it with custom flow-id forwarding and made the two impossible to
	// ablate separately.
	// Unscoped with a fixed underlying type, matching DataType::Type: ns-3's EnumValue and
	// MakeEnumChecker both take int, so a scoped enum would not convert.
	enum NicSelection : uint8_t {
		// Follow the NIC the schedule dictates (switch JSON -> GPU::PushPeerNic). One qp per
		// connection, pinned to the plane whose switches hold that connection's flow rules.
		// Requires a switch JSON parsed with pinNics enabled.
		NIC_SCHEDULED,
		// NCCL_IB_MERGE_NICS: fuse the NICs reaching a peer into one logical device, open one qp
		// per NIC, and split every message across them (>1 qp per connection).
		NIC_MERGED,
		// One qp per connection, NICs handed out by RdmaHw's node-global round-robin rotation.
		// Flow-level round robin -- the unmerged baseline.
		NIC_ROUND_ROBIN
	};

	// Ceiling on lanes per RDMA connection (see CollectivesApplication::GetRdmaLaneCount).
	// Mirrors NCCL_IB_MAX_DEVS_PER_NIC: a merged virtual device fuses at most this many
	// physical ports, so a connection over one never needs more parallel qps than this.
	#define MSCCL_MAX_RDMA_LANES 4

	// a single on-wire fragment of a Send(), queued for pacing onto the NIC.
	// the packet is fully constructed at Send() time so source data is captured
	// before any concurrent receive operations can overwrite the source buffer.
	struct PendingFragment{
		Ptr<Packet> packet; // fully constructed, header already attached
		Ptr<Socket> sock;   // channel-specific socket to send through
		PendingFragment(Ptr<Packet> pkt, Ptr<Socket> s) : packet(pkt), sock(s) {}
	};

	// helper class for channel modeling
	class MscclChannel {
		public:
			MscclChannel();
			MscclChannel(int id, Ptr<CollectivesApplication> app);
			~MscclChannel();
			void ConnectSendPeer(int peerId);
			void SetupRecvPeer(int peerId);
			//void SetupListener();
			//void OnNewConnection(Ptr<Socket> newSock, const Address& from);
			//bool CanAcceptConnection(Ptr<Socket> sock, const Address& from);
			void SendCallback(Ptr<Socket> sock, uint32_t bytes);
			void RecvCallback(Ptr<Socket> sock);

			inline void PushPendingSend(Ptr<Socket> sendpeer, PendingTransfer send);
			void Send(int16_t bid, int16_t sid, int16_t sendPeer, uint32_t nElems, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t mscclFlowId = MSCCL_FLOW_ID_NONE, double rateGBps = 0.0, uint32_t gridByteOff = 0, uint32_t iter = 0, uint16_t part = 0);
			void Recv(int16_t bid, int16_t sid, int16_t recvPeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff, uint32_t gridByteOff = 0, uint16_t part = 0);
			void RecvCpSend(int16_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems);
			void RecvRedSend(int16_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems);
			void RecvRedCp(int16_t bid, int16_t sid, int16_t recvpeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff, uint32_t gridByteOff = 0, uint16_t part = 0);
			void RecvRedCpSend(int16_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems);
			#ifdef FLOW_ID_TEST
			uint32_t GetFlowId(int src, int dst);
			void SetFlowIdTable(std::map<std::pair<int, int>, uint32_t>* table);
			#endif

			// RDMA-fabric transport (gpu<->switch/nvswitch peers), as opposed to the
			// p2p PacketSocket path above (gpu<->gpu direct peers)
			void SendRdma(int16_t bid, int16_t sid, int16_t sendpeer, uint32_t nElems, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t mscclFlowId, double rateGBps, uint32_t gridByteOff, uint32_t iter, uint16_t part);
			// eagerly establishes this channel's persistent RDMA connection to `peer` --
			// called once per (channel,peer) from CollectivesApplication::SetupRdmaPeers,
			// deferred one tick past Bootstrap() so every node's m_channels already exists
			// (see CollectivesApplication::Bootstrap()). Creates the peer's persistent qp and
			// force-creates the peer's RdmaRxQueuePair for this connection, hanging a callback
			// directly off it (RdmaRxQueuePair::m_perPktFn) -- that callback forwards straight
			// into OnBytesArrivedFromPeer, ignoring RdmaHw's wire-level sequence number entirely
			// (see OnBytesArrivedFromPeer's comment).
			void SetupRdmaSendPeer(int16_t peer);
			// Brackets the maxAllowedCount sub-transfers of one send step, so the step-level
			// effects that fire when its bytes are off the wire -- the network gate and the
			// netFlag -- happen once, after every sub-transfer of every lane has drained, rather
			// than once per sub-transfer. Begin plants a sentinel on the countdown so it cannot
			// reach zero while parts are still being issued; End removes it, and whichever of
			// End or the last completion gets there last fires the step's effects. A step with
			// no sub-transfers at all (count 0) is therefore completed by End directly.
			void BeginSendStep(int16_t bid, int16_t sid, uint32_t iter);
			void EndSendStep(int16_t bid, int16_t sid, uint32_t iter);
			// Drops one outstanding lane (or the issuing sentinel) from a send step, firing the
			// step's gate and netFlag when the last one goes.
			void NoteSendLaneComplete(int16_t bid, int16_t sid, uint32_t iter);
			// bound as the RdmaDriver::AddQueuePair completion callback
			// `iter` is the pipeline iteration this message was posted under, bound at post time
			// because a send step completes immediately and the threadblock may have advanced
			// well past it by the time the bytes actually drain.
			void OnRdmaSendComplete(int16_t bid, int16_t sid, uint32_t iter, int16_t sendpeer, uint32_t nElems);

			void Close();
		private:
			// shared per-fragment matching logic for both transports (called directly from
			// RecvCallback for sockets, and from the perPktFn lambda registered in
			// SetupRdmaSendPeer for RDMA): matches incoming bytes against `peer`'s
			// locally-posted Recv()/RecvRedCp() order (m_pendingRecvQueue) using a purely
			// local write-offset accumulator (PendingTransfer::receivedBytes) -- no fragment
			// offset or transfer-size info from the sender is used or needed. Stages bytes in
			// m_unclaimedBytes if no matching Recv()/RecvRedCp() has been posted yet, and
			// completes the step once a transfer's full locally-expected byte count has
			// arrived.
			// `lane` identifies which of the peer's parallel connections these bytes arrived on.
			// With NIC merging a peer opens one qp per NIC, each carrying a fixed contiguous
			// slice of every transfer, so each lane is its own independent in-order byte stream
			// and gets its own pending-recv queue and unclaimed-byte staging area. Lane indices
			// are assigned in the same order on both ends (SetupRdmaSendPeer creates the sender's
			// qp and registers the receiver's callback for lane i in the same iteration), which
			// is what makes "lane i carries slice i" agree across the connection.
			void OnBytesArrivedFromPeer(int16_t peer, uint8_t lane, const uint8_t* payload, uint32_t fragSize);
			// Byte range of `lane` when `totalBytes` is cut into `nLanes` contiguous slices.
			// Cuts on element boundaries so no slice splits a data element, and is a pure
			// function of (totalBytes, nLanes, lane) so both ends compute the same split from
			// their own copy of the algorithm without exchanging anything.
			void LaneSlice(uint32_t totalBytes, uint8_t nLanes, uint8_t lane, uint32_t& offset, uint32_t& size) const;
			// Counts down a step's lanes; drives StepCompletionCallback (recv) / the network
			// gate (send) only once the last lane of that step has completed.
			void NoteRecvLaneComplete(int16_t bid, int16_t sid, uint16_t part);
			// shared body of Recv()/RecvRedCp(): claims bytes already sitting in
			// m_unclaimedBytes for `recvPeer` if any (a full or partial claim, in FIFO byte
			// order), else registers a new pending recv (m_pendingRecvQueue) to be matched by
			// a future arrival.
			void ClaimOrRegisterPendingRecv(int16_t bid, int16_t sid, int16_t recvPeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff, int8_t op, uint32_t gridByteOff, uint16_t part);

			int8_t m_id;
			DataType::Type m_dataType;
			TypeId m_socketType;
			Ptr<CollectivesApplication> m_app;
			Ptr<Socket> m_listenSocket;
			std::map<int16_t, Ptr<Socket>> m_sendPeerSockets;
			std::map<Ptr<Socket>, int16_t> m_recvSocketPeers;
			// posted via Recv()/RecvRedCp(), dst known, awaiting bytes; per peer, FIFO in the
			// order this node's own step interpreter posted them -- matches real hardware's
			// per-connection step ordering (destination is never derived from wire content)
			std::map<std::pair<int16_t, uint8_t>, std::queue<PendingTransfer>> m_pendingRecvQueue;
			// bytes that arrived before a matching Recv()/RecvRedCp() was posted; per peer, one
			// continuous unclaimed byte stream (symmetric early-arrival counterpart to
			// m_pendingRecvQueue) -- see UnclaimedBytes for why this isn't chunked by transfer.
			std::map<std::pair<int16_t, uint8_t>, UnclaimedBytes> m_unclaimedBytes;
			std::map<Ptr<Socket>, std::queue<PendingTransfer>> m_pendingSends;
			// persistent per-peer RDMA connection, established once in SetupRdmaSendPeer. One qp
			// per lane, in lane order: index i is pinned to the i'th NIC that reaches the peer,
			// mirroring how a merged multi-port device stripes its qps over its ports.
			std::map<int16_t, std::vector<Ptr<RdmaQueuePair>>> m_rdmaQpByPeer;
			// lanes still outstanding for a (bid, sid) step, recv side and send side.
			// m_recvLanesLeft is safe to key on (bid, sid) alone under pipelining: a threadblock
			// stays busy until its recv completes, so it can never have the same step
			// outstanding for two iterations at once.
			//
			// (m_recvLanesLeft's third key element is the sub-transfer index, since a split step
			// posts one recv per part and each needs its own countdown.)
			//
			// m_sendLanesLeft is not, hence the iteration in its key. A send step completes the
			// moment the message is posted, so a threadblock can wrap around and re-post the same
			// (bid, sid) for the next iteration while the previous iteration's lanes are still
			// draining. Sharing one counter between them makes the second seed overwrite the
			// first, and the surplus completions then find no entry and each fire a step
			// completion of their own -- over-advancing netFlag past what has actually drained,
			// and releasing netdep waiters early.
			std::map<std::tuple<int16_t, int16_t, uint16_t>, uint8_t> m_recvLanesLeft;
			std::map<std::tuple<int16_t, int16_t, uint32_t>, uint8_t> m_sendLanesLeft;
			#ifdef FLOW_ID_TEST
			std::map<std::pair<int, int>, uint32_t>* m_flowIds;
			uint32_t m_flowId_counter = 0;
			#endif
	};


	// application
	class CollectivesApplication : public Application {

		public:
			static TypeId GetTypeId();
			CollectivesApplication();
			~CollectivesApplication();
			void SetAlgo(mscclAlgorithm* algo);
			void SetCurrChunkSize(uint32_t chunksize);
			Address GetPeerAddr(int16_t peerId, int id);
			Ptr<NetDevice> GetSendDevicePeer(int16_t peerId, int id);
			Ptr<NetDevice> GetRecvDevicePeer(int16_t peerId, int id);
			int GetPort();
			DataType::Type GetDataType();
		  TypeId GetSocketTypeId();
			void StepCompletionCallback(int16_t bid, int16_t sid);
			// Opens the network gate declared by transfer (bid, sid), if it declares one. Called
			// from MscclChannel::OnRdmaSendComplete, i.e. when that step's RDMA message actually
			// completes on the qp -- the CQE a proxy thread could really reap. No-op for a step
			// with netGate == MSCCL_GATE_NONE.
			void OpenGateForStep(int16_t bid, int16_t sid, uint32_t iter);
			// Called when threadblock `bid`'s oldest outstanding network transfer has physically
			// drained. Advances that threadblock's netFlag to the value recorded for that step at
			// dispatch time and wakes anything parked on a netdepid/netdeps referring to it.
			void NoteNetworkStepComplete(int16_t bid);
			DataBuffer* GetSrcBuffer();
			DataBuffer* GetDstBuffer();
			DataBuffer* GetScratchBuffer();
			void AllocBuffer(size_t size, DataBuffer* buf);
			void* GetBufferPtrRawBytes(uint16_t buf, size_t byte_offset);
			// `offset` is in whole msccl chunks; `gridByteOff` is the byte offset of the current
			// pipeline slice within that chunk (the kernel's gridOffset). Address is
			// offset*chunkBytes + gridByteOff.
			void* GetBufferPtr(uint16_t buf, int16_t offset, uint32_t gridByteOff = 0);
			void DumpBuffer(DataBuffer* buf, std::ostream& log);
			void SetCorrectnessCheck(bool enable);
			bool GetCorrectnessCheck() const;
			// queue fragments for transmission on the given (possibly shared) device,
			// pacing them onto the wire one at a time at the device's data rate
			void QueueFragmentsForDevice(Ptr<NetDevice> dev, std::queue<PendingFragment> frags);
			// RDMA-fabric helpers
			bool IsRdmaPeer(int16_t peer);
			Ptr<RdmaDriver> GetRdmaDriver();
			Ipv4Address GetMyIp();
			Ipv4Address GetPeerIp(int16_t peer);
			uint32_t GetPeerWin(int16_t peer);
			uint64_t GetPeerBaseRtt(int16_t peer);
			MscclChannel* GetChannel(int8_t chanId); // lets a sender reach into the peer's matching channel directly
			// hands out a node-global, monotonically increasing counter for RDMA sport
			// allocation (see MscclChannel::SetupRdmaSendPeer). Must be shared across every
			// MscclChannel on this node, not scoped per channel -- the sport is the only field
			// that makes RdmaHw's (senderIp,senderSport,pg) rx-qp key unique, and two channels
			// both connecting to the same peer would otherwise allocate the same sport and
			// collide on that key.
			uint16_t AllocateRdmaSport();
			// How many parallel RDMA connections ("lanes") this node opens to `peer`, i.e. how
			// many of its NICs a transfer to that peer is striped over. 1 unless NIC merging is
			// on. Modeled on NCCL's merged virtual device: NCCL_IB_MERGE_NICS fuses the ports of
			// one physical NIC into a single logical device, creates one qp per port, and splits
			// every message proportionally across them, rather than giving each connection a
			// single port. Cached, and computed as the min of the two ends' NIC counts so both
			// ends derive the same number without exchanging it.
			uint8_t GetRdmaLaneCount(int16_t peer);
			// True if the parsed switch JSON pinned this node's connection to `peer` (any
			// channel) to a specific NIC. GetRdmaLaneCount used to consult this to stop a pinned
			// connection from also being merged; NicSelection makes those mutually exclusive by
			// construction, so this is now only a diagnostic ("did the JSON actually pin me?").
			bool HasScheduledNicToward(int16_t peer);
			NicSelection GetNicSelection() const { return m_nicSelection; }
			// Whether the RDMA connection (peer, chan) should put a MscclFlowIdHeader on the
			// wire: true only when network flow ids are enabled AND this connection's schedule
			// steps actually name any. Cached per (peer, chan).
			//
			// In every schedule written so far this is exactly "this connection crosses the
			// fabric": flow-id-bearing and intra-NVSwitch connections are disjoint, and no
			// connection mixes labelled with unlabelled steps -- so intra-node RDMA never carries
			// the header, without needing a separate rule for the NVSwitch path. A schedule that
			// did mix them would round up to "emit on all of this connection's steps", which stays
			// correct: unlabelled steps send MSCCL_FLOW_ID_NONE and switches fall through to ECMP.
			bool ConnectionCarriesFlowIds(int16_t peer, int8_t chan);
			bool GetHonorNetDeps() const { return m_honorNetDeps; }
			#ifdef FLOW_ID_TEST
			// void SetFlowIdTableForChannel(std::map<std::pair<int, int>, uint32_t>*, int channel);
			// void SetFlowIdTableForAllChannels(std::map<std::pair<int, int>, uint32_t>* table);
			void StoreFlowIdTable(std::map<std::pair<int, int>, uint32_t>* table);
			#endif

		protected:
  		void StartApplication() override;
  		void StopApplication() override;
			// inline TransferState* GetTransferState(int16_t bid, int16_t sid);
			Time GetLocalOpDelay(int8_t op);
			void NonTransferHandler(int16_t bid, int16_t sid, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t nElems, int8_t op, uint32_t gridByteOff); // add some fixed delay
			// Number of sub-transfers step (bid,sid) is split into by the count loop, and the
			// chunk offset / element count of sub-transfer `part`. See m_maxAllowedCount.
			uint16_t StepPartCount(const mscclTransfer* tran) const;
			void StepPart(const mscclTransfer* tran, uint32_t iter, uint16_t part, uint16_t& chunkOff, uint32_t& nElems) const;
			void RunStep(int16_t bid, int16_t sid);
			void TryScheduleNextStep(int16_t bid);
			// Marks `gate` open (idempotently) and re-runs every threadblock parked on it.
			void OpenGate(int16_t gate, uint32_t iter);
			// Derives m_sliceElems / m_nLoops from m_protoChunkBytes and m_currChunkSize, and
			// rejects schedules whose features the pipelining model does not cover yet.
			void DerivePipelining();
			void InterpretAlgo();
			void Bootstrap();
			// establishes every channel's persistent RDMA send-peer connections. Deferred one
			// tick past Bootstrap() (via Simulator::ScheduleNow) so it only runs once every
			// node's own Bootstrap() has already constructed its m_channels map -- reaching
			// into a peer's channel here is otherwise a race across nodes. See
			// MscclChannel::SetupRdmaSendPeer for the per-(channel,peer) setup itself.
			void SetupRdmaPeers();
			void SendNextFragment(Ptr<NetDevice> dev);
			static Time GetTxTime(Ptr<NetDevice> dev, uint32_t bytes);
		private:
			DataType::Type m_dataType;
			mscclAlgorithm* m_algo;
			TypeId m_socket_tid;
			std::map<int16_t, TBState> m_TBStates; // maps tbId to last completed step
			// std::map<std::pair<int8_t, int16_t>, TransferState> m_transferStates; // transfer (tbId, sId) -> pending dependences info
			uint32_t m_currChunkSize;
			uint32_t m_currWorkId = 0;
			// Pipelining (the MSCCL kernel's gridOffset loop). A schedule's chunk is not shipped
			// as one message: the kernel replays the *entire* threadblock schedule once per
			// `chunkSize`-sized slice of it, with no barrier between replays, so a relay
			// threadblock starts forwarding slice 0 while its producer is still pushing slice 1.
			//
			// m_protoChunkBytes is the NCCL transport's per-iteration granularity, i.e.
			// (buffSize/NCCL_STEPS)*chunkSteps -- roughly 1-2 MiB for the SIMPLE protocol at
			// NCCL's default 4 MiB buffer. 0 disables pipelining entirely (m_nLoops == 1), which
			// reproduces the pre-pipelining behaviour byte for byte and is the default so that
			// existing scenarios keep their results until they opt in.
			//
			// m_sliceElems / m_nLoops are derived from it against m_currChunkSize in
			// InterpretAlgo. Every rank derives them from the same three globally-agreed inputs,
			// so they agree without any handshake -- the same property MscclChannel::LaneSlice
			// already relies on.
			//
			// Not modeled yet: send-side flow control. On real hardware a producer threadblock
			// cannot run arbitrarily far ahead of its consumer, because the NCCL transport's
			// ring buffer only holds NCCL_STEPS outstanding steps and the prims send blocks for
			// a credit once it is full. Here a send step completes the instant the message is
			// handed to the RDMA engine (see MscclChannel::SendRdma), so a producer can post
			// every iteration back to back. That makes injection no more aggressive than the
			// unpipelined case -- which posts the whole chunk at once regardless -- but it does
			// mean the sender is not throttled by the receiver the way it would be in practice.
			uint32_t m_protoChunkBytes = 0;
			uint32_t m_sliceElems = 0; // elements of a chunk covered by one iteration
			uint32_t m_nLoops = 1;     // number of gridOffset iterations
			// The kernel's maxAllowedCount: how many of a step's `count` chunks one sub-transfer
			// may carry. A step moving more than this is emitted as several messages, matching
			// the kernel's `for (c = 0; c < count; c += maxAllowedCount)` loop. Derived in
			// DerivePipelining as chunkEffectiveSize/sizePerMscclChunk, i.e. how many whole
			// chunks fit the transport's staging buffer, so it collapses to 1 in exactly the
			// regime where m_nLoops > 1. 0 means "no cap" and is what ProtoChunkBytes == 0
			// selects, leaving a step a single message exactly as before.
			uint32_t m_maxAllowedCount = 0;
			// Sub-transfers of a step still outstanding, for steps the count loop split. Seeded
			// by RunStep before it issues any of them and counted down by StepCompletionCallback,
			// which only advances the threadblock once the last one lands -- the kernel likewise
			// publishes a step's flag after the whole count loop, not per iteration of it. Absent
			// for an unsplit step, which is the common case.
			std::map<std::pair<int16_t, int16_t>, uint16_t> m_stepPartsLeft;
			// Elements this iteration covers (the kernel's `nelem`): m_sliceElems, except on the
			// final iteration where the chunk may not divide evenly.
			uint32_t SliceElemsForIter(uint32_t iter) const;
			// Byte offset of `iter`'s slice within a chunk (the kernel's `gridOffset`, in bytes).
			uint32_t GridByteOffsetForIter(uint32_t iter) const;
			bool m_correctnessCheck = false;
			int m_port = 5000;
			std::map<int, MscclChannel> m_channels;
			// per-device fragment pacing state; shared across MscclChannels whose
			// sockets resolve to the same underlying NetDevice
			std::map<Ptr<NetDevice>, std::queue<PendingFragment>> m_pendingFragments;
			std::map<Ptr<NetDevice>, bool> m_sendInFlight;
			// std::map<int16_t, Address> m_peerAddr;
			// std::map<int16_t, Ptr<NetDevice>> m_deviceFromPeer;
			// std::map<std::pair<uint8_t, int16_t>, PendingTransfer> m_pendingRecvs;
			// std::map<Ptr<Socket>, std::queue<PendingTransfer>> m_pendingSends;
			DataBuffer m_srcBuf;
			DataBuffer m_dstBuf;
			DataBuffer m_scratchBuf;
			// Per-threadblock FIFO of the netFlag value each in-flight network transfer will
			// publish when it drains. Pushed at dispatch (RunStep), where the step's (iter,
			// global_step) is still current, and popped on completion -- which can be much later,
			// by which time the threadblock has moved on and that pair is no longer recoverable.
			// A FIFO rather than a map because a threadblock's messages complete in issue order:
			// they share one qp (or, with NIC merging, a fixed set of qps that each drain their
			// slice of every message in order).
			std::map<int16_t, std::queue<int64_t>> m_pendingNetFlag;
			uint16_t m_rdmaSportCounter = 0; // node-global; see AllocateRdmaSport
			// NicSelection attribute; see GetRdmaLaneCount and MscclChannel::SetupRdmaSendPeer
			NicSelection m_nicSelection = NIC_SCHEDULED;
			std::map<int16_t, uint8_t> m_rdmaLaneCount; // memoized GetRdmaLaneCount
			bool m_networkFlowIds = true;    // NetworkFlowIds attribute; see ConnectionCarriesFlowIds
			std::map<std::pair<int16_t, int8_t>, bool> m_connFlowIds; // memoized ConnectionCarriesFlowIds
			bool m_honorNetDeps = true;      // HonorNetDeps attribute; see TryScheduleNextStep
			// Network gate state (see mscclTransfer::netGate/netWait), indexed [iter][gate]. Both
			// vectors are sized at InterpretAlgo from m_nLoops and mscclAlgorithm::maxNetGate, so
			// there is no MSCCL_MAX_GATES to tune.
			//
			// A gate is one-shot *within an iteration* and never closes; each iteration gets its
			// own independent set. That is the natural lift of what a gate means -- slice k of a
			// waiting op is held until slice k of the opening op is off the wire -- and it
			// matches how netdepid/netdeps and data dependences already resolve against the
			// waiter's own iteration. Flat, un-keyed gates would leave every iteration after the
			// first finding its gates already open, running completely unpaced.
			//
			// The level-triggered property OpenGate relies on survives the lift: a parked
			// threadblock is by definition not busy, and a threadblock's iteration only advances
			// in StepCompletionCallback, so it cannot change iteration while parked. A gate must
			// still never be cleared mid-run.
			std::vector<std::vector<uint8_t>> m_gateOpen;                       // [iter][gate] -> open?
			std::vector<std::vector<std::unordered_set<int16_t>>> m_gateWaiters; // [iter][gate] -> parked tbs
			#ifdef FLOW_ID_TEST
			std::map<std::pair<int, int>, uint32_t>* m_flowIds;
			#endif
	};
} // namespace ns3
#endif
