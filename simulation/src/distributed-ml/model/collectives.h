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
#include <fstream>
#include <iostream>

// #define FLOW_ID_TEST
// for temporary testing with inserting flow ID here
// to be disabled when such info properly encoded in xml



/*
TODO: model work index & iter for sequences of ops & reuse of algorithm over larger grid
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
		int64_t flag;
		bool busy;
		std::unordered_set<int16_t> tryReschedule; // TBs that should try rescheduling when this TB reaches flag
		TBState(): bid(-1), global_step(0), local_step(0), flag(-1), busy(false){}
		TBState(int16_t id): bid(id), global_step(0), local_step(0),
      flag(-1), busy(false){}
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
		PendingTransfer(): bid(-1), sid(-1), receivedBytes(0), pendingBytes(0), op(-1), srcBuf(3), srcOffset(-1), dstBuf(3), dstOffset(-1), scratchBuf(nullptr){}
		PendingTransfer(int16_t bId, int16_t sId, uint32_t bytes, int8_t Op, uint16_t srcbuf, uint16_t srcoff, uint16_t dstbuf, int16_t dstoff): bid(bId), sid(sId),
										receivedBytes(0), pendingBytes(bytes), op(Op), srcBuf(srcbuf), srcOffset(srcoff), dstBuf(dstbuf), dstOffset(dstoff), scratchBuf(nullptr){}
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
			void Send(int16_t bid, int16_t sid, int16_t sendPeer, uint32_t nElems, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t mscclFlowId = MSCCL_FLOW_ID_NONE, double rateGBps = 0.0);
			void Recv(int16_t bid, int16_t sid, int16_t recvPeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff);
			void RecvCpSend(int16_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems);
			void RecvRedSend(int16_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems);
			void RecvRedCp(int16_t bid, int16_t sid, int16_t recvpeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff);
			void RecvRedCpSend(int16_t bid, int16_t sid, int16_t sendpeer, int16_t recvpeer, uint32_t nElems);
			#ifdef FLOW_ID_TEST
			uint32_t GetFlowId(int src, int dst);
			void SetFlowIdTable(std::map<std::pair<int, int>, uint32_t>* table);
			#endif

			// RDMA-fabric transport (gpu<->switch/nvswitch peers), as opposed to the
			// p2p PacketSocket path above (gpu<->gpu direct peers)
			void SendRdma(int16_t bid, int16_t sid, int16_t sendpeer, uint32_t nElems, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t mscclFlowId, double rateGBps);
			// eagerly establishes this channel's persistent RDMA connection to `peer` --
			// called once per (channel,peer) from CollectivesApplication::SetupRdmaPeers,
			// deferred one tick past Bootstrap() so every node's m_channels already exists
			// (see CollectivesApplication::Bootstrap()). Creates the peer's persistent qp and
			// force-creates the peer's RdmaRxQueuePair for this connection, hanging a callback
			// directly off it (RdmaRxQueuePair::m_perPktFn) -- that callback forwards straight
			// into OnBytesArrivedFromPeer, ignoring RdmaHw's wire-level sequence number entirely
			// (see OnBytesArrivedFromPeer's comment).
			void SetupRdmaSendPeer(int16_t peer);
			// bound as the RdmaDriver::AddQueuePair completion callback
			void OnRdmaSendComplete(int16_t bid, int16_t sid, int16_t sendpeer, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t nElems);

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
			void OnBytesArrivedFromPeer(int16_t peer, const uint8_t* payload, uint32_t fragSize);
			// shared body of Recv()/RecvRedCp(): claims bytes already sitting in
			// m_unclaimedBytes for `recvPeer` if any (a full or partial claim, in FIFO byte
			// order), else registers a new pending recv (m_pendingRecvQueue) to be matched by
			// a future arrival.
			void ClaimOrRegisterPendingRecv(int16_t bid, int16_t sid, int16_t recvPeer, uint32_t nElems, uint16_t dstbuf, int16_t dstoff, int8_t op);

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
			std::map<int16_t, std::queue<PendingTransfer>> m_pendingRecvQueue;
			// bytes that arrived before a matching Recv()/RecvRedCp() was posted; per peer, one
			// continuous unclaimed byte stream (symmetric early-arrival counterpart to
			// m_pendingRecvQueue) -- see UnclaimedBytes for why this isn't chunked by transfer.
			std::map<int16_t, UnclaimedBytes> m_unclaimedBytes;
			std::map<Ptr<Socket>, std::queue<PendingTransfer>> m_pendingSends;
			// persistent per-peer RDMA connection, established once in SetupRdmaSendPeer
			std::map<int16_t, Ptr<RdmaQueuePair>> m_rdmaQpByPeer;
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
			void OpenGateForStep(int16_t bid, int16_t sid);
			DataBuffer* GetSrcBuffer();
			DataBuffer* GetDstBuffer();
			DataBuffer* GetScratchBuffer();
			void AllocBuffer(size_t size, DataBuffer* buf);
			void* GetBufferPtrRawBytes(uint16_t buf, size_t byte_offset);
			void* GetBufferPtr(uint16_t buf, int16_t offset);
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
			void NonTransferHandler(int16_t bid, int16_t sid, uint16_t srcbuf, int16_t srcoff, uint16_t dstbuf, int16_t dstoff, uint32_t nElems, int8_t op); // add some fixed delay
			void RunStep(int16_t bid, int16_t sid);
			void TryScheduleNextStep(int16_t bid);
			// Marks `gate` open (idempotently) and re-runs every threadblock parked on it.
			void OpenGate(int16_t gate);
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
			uint32_t m_currIter = 0;
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
			uint16_t m_rdmaSportCounter = 0; // node-global; see AllocateRdmaSport
			// Network gate state (see mscclTransfer::netGate/netWait). Both vectors are sized at
			// InterpretAlgo from mscclAlgorithm::maxNetGate, so there is no MSCCL_MAX_GATES to tune.
			//
			// Gates are one-shot and never reset. This is sound only because the simulator runs a
			// single work item / single iteration: m_currWorkId and m_currIter are initialized to 0
			// and never advanced, and nLoops is effectively 1. If multi-loop pipelining is ever
			// modeled, both vectors must be cleared per iteration -- otherwise iteration 2 finds
			// every gate already open and runs unpaced.
			std::vector<uint8_t> m_gateOpen;                       // gate id -> open?
			std::vector<std::unordered_set<int16_t>> m_gateWaiters; // gate id -> tbs parked on it
			#ifdef FLOW_ID_TEST
			std::map<std::pair<int, int>, uint32_t>* m_flowIds;
			#endif
	};
} // namespace ns3
#endif
