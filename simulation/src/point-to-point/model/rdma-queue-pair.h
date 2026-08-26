#ifndef RDMA_QUEUE_PAIR_H
#define RDMA_QUEUE_PAIR_H

#include <ns3/object.h>
#include <ns3/packet.h>
#include <ns3/ipv4-address.h>
#include <ns3/data-rate.h>
#include <ns3/event-id.h>
#include <ns3/custom-header.h>
#include <ns3/int-header.h>
#include <ns3/msccl-flow-id-header.h>
#include <deque>
#include <queue>
#include <vector>
#include <functional>

namespace ns3 {

class RdmaQueuePair : public Object {
public:
	Time startTime;
	Ipv4Address sip, dip;
	uint16_t sport, dport;
	uint64_t m_size, m_init_size, m_tag; // m_size/m_init_size are informational (monitoring/print only) since the message queue below drives GetBytesLeft/IsFinished
	uint32_t m_mscclFlowId; // app-level msccl flow id, carried on the wire via MscclFlowIdHeader; MscclFlowIdHeader::NO_FLOW_ID if unset
	// Whether this connection's packets carry a MscclFlowIdHeader at all. Decided once, per
	// connection, by MscclChannel::SetupRdmaSendPeer: true only when the feature is enabled AND
	// this connection's schedule steps actually name flow ids. In every schedule written so far
	// that predicate is exactly "this connection crosses the fabric" -- flow ids and
	// intra-NVSwitch connections are disjoint, and no connection mixes labelled and unlabelled
	// steps -- so intra-node RDMA traffic carries no custom header, as intended.
	//
	// Per-connection rather than per-step because the receiver has to agree, and it can only
	// resolve a packet down to its rx qp, never to a step. The sender sets the matching bit on
	// the peer's rx qp directly at setup, so the two cannot desync.
	bool m_emitFlowIdHdr = false;
	// sentinel: no NIC bound yet (m_pinnedNicIdx), and no NIC asked for (m_requestedNicIdx).
	static constexpr uint32_t NIC_UNPINNED = 0xffffffff;
	// The NIC the schedule asks this connection to use, or NIC_UNPINNED when nothing dictates
	// one. This is only a request: RdmaHw::ResolveNic honors it if that NIC actually reaches
	// the destination, and otherwise warns and falls back, so it is kept separate from the
	// binding below rather than written straight into it.
	uint32_t m_requestedNicIdx = NIC_UNPINNED;
	// The NIC (ifIndex, which is also the index into RdmaHw::m_nic) this qp sends on. Resolved
	// once -- by RdmaHw::GetNicIdxOfQp on its first call, from m_requestedNicIdx when the
	// schedule dictates one and from the node's round-robin rotation otherwise -- and never
	// changed afterwards,
	// mirroring hardware: a verbs queue pair is bound to a device at creation and cannot
	// migrate. RdmaHw also places the qp in that NIC's queue-pair group and seeds its rate from
	// that device, so a binding that drifted mid-connection would corrupt pacing outright.
	// On a multi-homed fabric this choice picks the network plane, and therefore decides
	// whether the switches along the path hold any flow rules for this connection at all.
	uint32_t m_pinnedNicIdx = NIC_UNPINNED;
	uint32_t m_src, m_dest;
	uint64_t snd_nxt, snd_una; // next seq to send, the highest unacked seq
	uint16_t m_pg;
	uint16_t m_ipid;
	uint32_t m_win; // bound of on-the-fly packets
	uint64_t m_baseRtt; // base RTT of this qp
	DataRate m_max_rate; // max rate
	bool m_var_win; // variable window size
	Time m_nextAvail;	//< Soonest time of next send
	// When true, PushMessage resets m_nextAvail to Now on an idle->backlogged transition so the
	// accumulating rate-targeting pacer (RdmaHw::UpdateNextAvail) does not carry stale credit
	// across an idle gap. Set from RdmaHw::m_rateTargeting at qp creation; false = legacy pacing.
	bool m_pacerAccumulate = false;
	uint32_t wp; // current window of packets
	uint32_t lastPktSize;
	// true only once explicitly torn down via RdmaHw::CloseQueuePair/QpComplete -- the sole
	// condition under which qbb-net-device's pacing sweep evicts this QP from its NIC's
	// group. A QP with an empty message queue but m_closed==false (e.g. an MSCCL persistent
	// connection idling between Send() calls) is left resident, since GetBytesLeft()==0
	// already keeps it out of the pacing selection without needing eviction.
	bool m_closed;
	// when true (the default, preserving today's behavior for every existing non-MSCCL
	// caller such as RdmaClient), RdmaHw automatically tears the whole qp down once its
	// message queue drains completely. MSCCL sets this false on its persistent per-peer
	// qps so draining between Send() calls is just normal idle, not teardown -- those qps
	// are only closed explicitly, via RdmaHw::CloseQueuePair, at channel teardown.
	bool m_autoClose;

	// A persistent QP (reused across multiple logical transfers to the same peer) enqueues
	// one RdmaMessage per transfer; snd_nxt/snd_una progress continuously across message
	// boundaries so congestion-control state carries over between transfers, while
	// completion is tracked per-message rather than for the whole QP.
	class RdmaMessage {
	public:
		uint64_t m_size;
		uint64_t m_startSeq;
		uint8_t* m_srcDataPtr; // nullptr if correctness check disabled for this message
		uint32_t m_mscclFlowId; // per-step flow id (XML "mscclflowid"); a persistent qp's messages can each carry a different one
		// per-message host-side pacing cap (XML "rate", GB/s -> DataRate); 0 (default)
		// means no cap and pacing falls back to the qp's congestion-controlled rate.
		// Like m_mscclFlowId, each message on a persistent qp can carry a different one.
		DataRate m_rate;
		Callback<void> m_notifyAppFinish;
		Callback<void> m_notifyAppSent;
	};
	// A deque rather than a queue because the qp sends out of a different element than it
	// retires: the FRONT message is the oldest unacknowledged one (retired by FinishMessage
	// when its bytes are acked), while snd_nxt may already have run on into a LATER message.
	// Those two are the same element only when m_maxMsgsInFlight == 1.
	std::deque<RdmaMessage> m_messages;
	// How many messages this qp may have in flight at once, counting from the oldest
	// unacknowledged one through the one snd_nxt is currently in. This is the transport's
	// pipelining depth, and the analogue of NCCL's NCCL_STEPS ring-buffer slots: real hardware
	// streams the next step's bytes out while earlier steps are still awaiting completion.
	// 1 restores strict one-message-at-a-time behaviour, in which the qp goes idle for a full
	// RTT at every message boundary waiting for the ack that retires the front message. Set
	// from RdmaHw's MaxMsgsInFlight attribute at qp creation.
	uint32_t m_maxMsgsInFlight = 1;
	void PushMessage(uint64_t size, uint8_t* srcDataPtr, uint32_t mscclFlowId, Callback<void> notifyAppFinish, Callback<void> notifyAppSent, DataRate rate = DataRate(0));
	void FinishMessage(); // pops the front message, fires its m_notifyAppFinish
	bool IsCurMessageFinished(); // snd_una >= front.m_startSeq + front.m_size
	// The message snd_nxt currently falls in -- the one whose bytes the next packet carries --
	// or nullptr when there is nothing sendable, either because every queued message has been
	// fully sent or because doing so would exceed m_maxMsgsInFlight. Everything on the send
	// path is derived from this rather than from front(), since each message on a persistent
	// qp can carry its own source buffer, flow id and pacing cap.
	const RdmaMessage* GetSendingMessage();
	uint8_t* GetCurSrcDataPtr(); // sending message's srcDataPtr, or nullptr if none/not sendable
	uint64_t GetCurMsgStartSeq(); // sending message's m_startSeq, or snd_nxt if none sendable
	uint32_t GetCurMscclFlowId(); // sending message's mscclFlowId, or this qp's own (fallback) if none
	DataRate GetCurRate(); // sending message's host-side pacing cap, or DataRate(0) (no cap) if none
	/******************************
	 * runtime states
	 *****************************/
	uint32_t nvls_enable;
	DataRate m_rate;	//< Current rate
	struct {
		DataRate m_targetRate;	//< Target rate
		EventId m_eventUpdateAlpha;
		double m_alpha;
		bool m_alpha_cnp_arrived; // indicate if CNP arrived in the last slot
		bool m_first_cnp; // indicate if the current CNP is the first CNP
		EventId m_eventDecreaseRate;
		bool m_decrease_cnp_arrived; // indicate if CNP arrived in the last slot
		uint32_t m_rpTimeStage;
		EventId m_rpTimer;
	} mlx;
	struct {
		uint64_t m_lastUpdateSeq;
		DataRate m_curRate;
		IntHop hop[IntHeader::maxHop];
		uint32_t keep[IntHeader::maxHop];
		uint32_t m_incStage;
		double m_lastGap;
		double u;
		struct {
			double u;
			DataRate Rc;
			uint32_t incStage;
		}hopState[IntHeader::maxHop];
	} hp;
	struct{
		uint64_t m_lastUpdateSeq;
		DataRate m_curRate;
		uint32_t m_incStage;
		uint64_t lastRtt;
		double rttDiff;
	} tmly;
	struct{
		uint64_t m_lastUpdateSeq;
		uint32_t m_caState;
		uint64_t m_highSeq; // when to exit cwr
		double m_alpha;
		uint32_t m_ecnCnt;
		uint32_t m_batchSizeOfAlpha;
	} dctcp;
	struct{
		uint64_t m_lastUpdateSeq;
		DataRate m_curRate;
		uint32_t m_incStage;
	}hpccPint;

	/***********
	 * methods
	 **********/
	static TypeId GetTypeId (void);
	RdmaQueuePair(uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport);
	void SetSize(uint64_t size);
	void SetWin(uint32_t win);
	void SetBaseRtt(uint64_t baseRtt);
	void SetVarWin(bool v);

	uint64_t GetBytesLeft();
	uint64_t GetInitialSize();
	uint32_t GetSrc();
	uint32_t GetDest();
	uint64_t GetTag();
	void SetTag(uint64_t tag);void SetSrc(uint32_t src);void SetDest(uint32_t dest);void SetInitialSize(uint64_t size);
	uint32_t GetMscclFlowId();
	void SetMscclFlowId(uint32_t mscclFlowId);
	uint32_t GetHash(void);
	void Acknowledge(uint64_t ack);
	uint64_t GetOnTheFly();
	bool IsWinBound();
	uint64_t GetWin(); // window size calculated from m_rate
	bool IsFinished();
	uint64_t HpGetCurWin(); // window size calculated from hp.m_curRate, used by HPCC
};

class RdmaRxQueuePair : public Object { // Rx side queue pair
public:
	struct ECNAccount{
		uint16_t qIndex;
		uint8_t ecnbits;
		uint16_t qfb;
		uint16_t total;

		ECNAccount() { memset(this, 0, sizeof(ECNAccount));}
	};
	ECNAccount m_ecn_source;
	uint32_t sip, dip;
	uint16_t sport, dport;
	uint16_t m_ipid;
	uint64_t ReceiverNextExpectedSeq;
	Time m_nackTimer;
	int32_t m_milestone_rx;
	uint32_t m_lastNACK;
	EventId QcnTimerEvent; // if destroy this rxQp, remember to cancel this timer
	// per-flow rx-side byte-arrival notification, set once (eagerly, at connection setup --
	// see MscclChannel::SetupRdmaSendPeer) for as long as this rx qp lives. Called for every
	// in-order packet on this flow: (payload, payloadSize, seqOffset). Living directly on the
	// rx qp means callers get uniqueness for free from RdmaHw's own (senderIp,senderSport,pg)
	// rx-qp key instead of needing a second, independently-derived key.
	std::function<void(const uint8_t*, uint32_t, uint64_t)> m_perPktFn;
	// Receiver's copy of RdmaQueuePair::m_emitFlowIdHdr for this connection, set by the *sender*
	// at connection setup (MscclChannel::SetupRdmaSendPeer force-creates this rx qp and writes
	// it right alongside m_perPktFn above). RdmaHw::ReceiveUdp needs it to size the payload,
	// since the header cannot be detected from the wire. Defaults false, matching the tx side.
	bool m_expectFlowIdHdr = false;

	static TypeId GetTypeId (void);
	RdmaRxQueuePair();
	uint32_t GetHash(void);
};

class RdmaQueuePairGroup : public Object {
public:
	std::vector<Ptr<RdmaQueuePair> > m_qps;
	//std::vector<Ptr<RdmaRxQueuePair> > m_rxQps;

	static TypeId GetTypeId (void);
	RdmaQueuePairGroup(void);
	uint32_t GetN(void);
	Ptr<RdmaQueuePair> Get(uint32_t idx);
	Ptr<RdmaQueuePair> operator[](uint32_t idx);
	void AddQp(Ptr<RdmaQueuePair> qp);
	//void AddRxQp(Ptr<RdmaRxQueuePair> rxQp);
	void Clear(void);
};

}

#endif /* RDMA_QUEUE_PAIR_H */
