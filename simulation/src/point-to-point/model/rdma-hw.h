#ifndef RDMA_HW_H
#define RDMA_HW_H

#include <ns3/rdma.h>
#include <ns3/rdma-queue-pair.h>
#include <ns3/node.h>
#include <ns3/custom-header.h>
#include "qbb-net-device.h"
#include <unordered_map>
#include <functional>
#include <ostream>
#include <set>
#include "pint.h"

namespace ns3 {

struct RdmaInterfaceMgr{
	// The QbbDevice dev contains many qps in qpGrp
	Ptr<QbbNetDevice> dev;
	Ptr<RdmaQueuePairGroup> qpGrp;

	RdmaInterfaceMgr() : dev(NULL), qpGrp(NULL) {}
	RdmaInterfaceMgr(Ptr<QbbNetDevice> _dev){
		dev = _dev;
	}
};

class RdmaHw : public Object {
public:

	static TypeId GetTypeId (void);
	RdmaHw();

	Ptr<Node> m_node;
	DataRate m_minRate;		//< Min sending rate
	uint32_t m_mtu;
	uint32_t m_cc_mode;
	double m_nack_interval;
	uint32_t m_chunk;
	uint32_t m_ack_interval;
	bool m_backto0;
	bool m_var_win, m_fast_react;
	bool m_rateBound;
	// When true, per-message rate pacing (the MSCCL/XML "rate") accumulates credit like a
	// token-bucket shaper so a qp actually reaches its target rate, instead of merely being
	// upper-bounded. See RdmaHw::UpdateNextAvail and the rate-targeting design notes.
	bool m_rateTargeting;
	// Max banked catch-up "burst" for the accumulating pacer, in bytes (the ConnectX
	// max_burst_sz / packet_pacing_burst_bound analog). Converted to a time budget at the
	// qp's pace rate; bounds how far a starved qp may run ahead after an idle gap.
	uint32_t m_paceMaxCreditBytes;
	// Pipelining depth of a persistent qp: how many messages it may have in flight at once,
	// counting from the oldest unacknowledged one through the one snd_nxt is in. Copied onto
	// each qp at creation (RdmaQueuePair::m_maxMsgsInFlight). Mirrors NCCL's NCCL_STEPS ring
	// slots; 1 serializes messages, costing a full RTT of idle wire at every message boundary.
	uint32_t m_maxMsgsInFlight;
	uint32_t m_total_pause_times; 
	uint32_t m_paused_times;
	std::vector<RdmaInterfaceMgr> m_nic; // list of running nic controlled by this RdmaHw
	std::unordered_map<uint64_t, Ptr<RdmaQueuePair> > m_qpMap; // mapping from uint64_t to qp
	std::unordered_map<uint64_t, Ptr<RdmaRxQueuePair> > m_rxQpMap; // mapping from uint64_t to rx qp
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable_nxthop_nvswitch; // map from ip address (u32) to possible ECMP port (index of dev) connected to nvswitch
	// Rotation used by ResolveNic to hand this node's NICs out to new qps evenly when the
	// schedule does not dictate one. Node-global and advanced only when there is an actual
	// choice (>1 equal-cost NIC), so single-homed destinations do not perturb the alternation.
	uint32_t m_nicRoundRobin = 0;
	uint32_t m_gpus_per_server; // uesed for routing; if src and dst in the same server, then communicate by nvswitch.
	uint32_t nvls_enable;
	std::set<uint32_t> nvswitch_set;

	// qp complete callback
	typedef Callback<void, Ptr<RdmaQueuePair> > QpCompleteCallback;
	QpCompleteCallback m_qpCompleteCallback;
    typedef Callback<void, Ptr<RdmaQueuePair> > SendCompleteCallback;
    SendCompleteCallback m_sendCompleteCallback;

    // for monitor
	std::vector<uint64_t> tx_bytes; // <port_id, tx_bytes>
	std::unordered_map<uint64_t, uint32_t> qp_cnp; // key of qp ---> received cnp number
	std::vector<uint64_t> last_tx_bytes; // last sampling value <port_id, tx_bytes>
	std::unordered_map<uint64_t, uint32_t> last_qp_cnp; // last sampling value key of qp ---> received cnp number
	std::unordered_map<uint64_t, uint64_t> last_qp_rate; // last sampling value key of qp ---> sending rate
	void UpdateTxBytes(uint32_t port_id, uint64_t bytes);
	void PrintHostBW(FILE* bw_output, uint32_t bw_mon_interval);
	void PrintQPRate(FILE* rate_output);
	void PrintQPCnpNumber(FILE* cnp_output);

	// ---- pacing diagnostic ----------------------------------------------------------------
	// Answers the one question an ablation that flips the MSCCL XML "rate" on and off cannot
	// answer from completion time alone: was the cap ever actually applied? A cap that never
	// reaches the shaper and a cap that reaches it but never binds produce byte-identical runs,
	// and so does a cap the shaper structurally cannot express -- a message of a single MTU has
	// no inter-packet gap to stretch, so its requested rate is silently discarded.
	//
	// Static (job-global) rather than per-RdmaHw: every field here is only ever reported summed
	// over all nodes, and one copy avoids walking the node list to collect it.
	struct PaceStats {
		// Per packet, from UpdateNextAvail -- who set the gap that follows this packet. The
		// four buckets are disjoint and together cover every paced packet.
		uint64_t cappedPkts = 0,   cappedBytes = 0;    // the sending message's XML cap won
		uint64_t slackPkts = 0,    slackBytes = 0;     // cap present but the cc rate was already lower
		uint64_t noCapPkts = 0,    noCapBytes = 0;     // sending message carried no cap
		uint64_t tailPkts = 0,     tailBytes = 0;      // no sending message left: see the note in UpdateNextAvail
		double   cappedSeconds = 0.0;                  // sum bytes*8/cap, for the byte-weighted mean applied cap
		// Per message, from NotePacedMessage -- what the schedule asked for, before the
		// transport got a chance to apply or ignore it.
		uint64_t msgsWithRate = 0, msgsWithoutRate = 0;
		uint64_t msgsSinglePkt = 0;                    // of msgsWithRate: <= one MTU, so unshapeable
		uint64_t ratedBytes = 0;                       // bytes posted under a rate
		double   ratedSeconds = 0.0;                   // sum bytes*8/rate over those, for their mean
	};
	static PaceStats m_paceStats;
	// Records the rate the schedule asked for on one message as it is posted. Called by
	// MscclChannel::SendRdma next to PushMessage; rate 0 means the step carried no "rate".
	void NotePacedMessage(uint64_t bytes, DataRate rate);
	// The whole diagnostic as a short block on `os`, or one line if no rate was ever requested.
	static void PrintPaceStats(std::ostream& os);

	// nvls
	void enable_nvls();
	void disable_nvls();
	void add_nvswitch(uint32_t nvswitch_id);

	void SetNode(Ptr<Node> node);
	void Setup(QpCompleteCallback cb,SendCompleteCallback send_cb); // setup shared data and callbacks with the QbbNetDevice
	static uint64_t GetQpKey(uint32_t dip, uint16_t sport, uint16_t pg); // get the lookup key for m_qpMap
	Ptr<RdmaQueuePair> GetQp(uint32_t dip, uint16_t sport, uint16_t pg); // get the qp
	// get the NIC index of the qp; resolves and caches the binding on first call, so a qp
	// always answers with the same NIC for its whole life (as a verbs qp does)
	uint32_t GetNicIdxOfQp(Ptr<RdmaQueuePair> qp);
	// binds a new qp to one of `candidates`: the schedule's NIC if it dictated one and it is
	// reachable, otherwise the next NIC in this node's round-robin rotation
	uint32_t ResolveNic(Ptr<RdmaQueuePair> qp, const std::vector<int>& candidates);
	// The equal-cost next-hop NICs toward `dip`, preferring the NVSwitch when both an NVSwitch
	// and a fabric route exist (two GPUs behind one NVSwitch are equidistant either way, and
	// intra-node traffic must stay off the fabric). Single source of truth for that choice --
	// GetNicIdxOfQp, GetNicIdxOfRxQp and GetNicsToward all route through it so the data path,
	// the ack path and the app's lane setup can never disagree about which NICs are eligible.
	const std::vector<int>& NicCandidates(uint32_t dip);
	// Every NIC that reaches `dip` at equal cost, in ifIndex order, as BFS computed them.
	// Lets the app stripe one connection's qps over the NICs (see
	// CollectivesApplication::GetRdmaLaneCount) instead of picking just one. Returns false
	// if the destination is unknown; `out` is cleared first.
	bool GetNicsToward(Ipv4Address dip, std::vector<int>& out);
	// creates a new qp and, if size != 0, pushes it as the qp's first message (size == 0 is
	// used to eagerly establish a persistent MSCCL connection at bootstrap with no data
	// queued yet -- see MscclChannel::SetupRdmaSendPeer). Returns the qp so callers that
	// intend to reuse it (push further messages directly via qp->PushMessage(...), bypassing
	// this method) can hold onto it; one-shot callers (e.g. RdmaClient) can simply ignore it,
	// since autoClose defaults to true and behaves exactly as before.
	Ptr<RdmaQueuePair> AddQueuePair(uint32_t src, uint32_t dest, uint64_t tag, uint64_t size, uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport, uint32_t win, uint64_t baseRtt, uint32_t mscclFlowId, Callback<void> notifyAppFinish, Callback<void> notifyAppSent, uint8_t* srcDataPtr = nullptr, bool autoClose = true, uint32_t pinnedNic = RdmaQueuePair::NIC_UNPINNED);
	// explicit whole-qp teardown for reused/persistent qps (autoClose == false); thin
	// wrapper over QpComplete that exists purely for call-site clarity.
	void CloseQueuePair(Ptr<RdmaQueuePair> qp);
	// wakes up qp's NIC so it re-polls for data: needed after pushing a message directly
	// onto an existing/persistent qp (bypassing AddQueuePair, which does this once at qp
	// creation via NewQp) -- otherwise a qp that had drained to idle never gets re-selected
	// by RdmaEgressQueue::GetNextQindex, and the new message just sits in the queue forever.
	void TriggerTransmit(Ptr<RdmaQueuePair> qp);
	void DeleteQueuePair(Ptr<RdmaQueuePair> qp);

	Ptr<RdmaRxQueuePair> GetRxQp(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg, bool create); // get a rxQp
	uint32_t GetNicIdxOfRxQp(Ptr<RdmaRxQueuePair> q); // get the NIC index of the rxQp
	void DeleteRxQp(uint32_t dip, uint16_t pg, uint16_t dport);

	int ReceiveUdp(Ptr<Packet> p, CustomHeader &ch);
	int ReceiveCnp(Ptr<Packet> p, CustomHeader &ch);
	int ReceiveAck(Ptr<Packet> p, CustomHeader &ch); // handle both ACK and NACK
	int Receive(Ptr<Packet> p, CustomHeader &ch); // callback function that the QbbNetDevice should use when receive packets. Only NIC can call this function. And do not call this upon PFC

	void PCIePause(uint32_t nic_idx, uint32_t qIndex);
	void PCIeResume(uint32_t nic_idx, uint32_t qIndex);
	void EnablePause();
	bool enable_pcie_pause; 

	void CheckandSendQCN(Ptr<RdmaRxQueuePair> q);
	int ReceiverCheckSeq(uint64_t seq, Ptr<RdmaRxQueuePair> q, uint32_t size);
	void AddHeader (Ptr<Packet> p, uint16_t protocolNumber);
	static uint16_t EtherToPpp (uint16_t protocol);

	void RecoverQueue(Ptr<RdmaQueuePair> qp);
	// per-message completion: pops the qp's front message and fires its callback, without
	// tearing down the qp itself. Called whenever IsCurMessageFinished() (queue non-empty
	// and its front message's bytes are all acked), in place of the old
	// "whole qp finished -> QpComplete" check.
	void QpCompleteMessage(Ptr<RdmaQueuePair> qp);
	// whole-qp teardown: only called explicitly now (e.g. RdmaClient once its single
	// message finishes, or MscclChannel::Close() for a persistent MSCCL connection at
	// simulation end) -- never automatically inferred from a temporarily-empty message
	// queue, since that's the normal idle state for a persistent, reused qp.
	void QpComplete(Ptr<RdmaQueuePair> qp);
	void SetLinkDown(Ptr<QbbNetDevice> dev);

    int SendPacketComplete(Ptr<Packet> p, CustomHeader &ch);
    void SendComplete(Ptr<RdmaQueuePair> qp);

    // call this function after the NIC is setup
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx, bool is_nvswitch);
	void ClearTable();
	void RedistributeQp();

	Ptr<Packet> GetNxtPacket(Ptr<RdmaQueuePair> qp); // get next packet to send, inc snd_nxt
	void PktSent(Ptr<RdmaQueuePair> qp, Ptr<Packet> pkt, Time interframeGap);
	void UpdateNextAvail(Ptr<RdmaQueuePair> qp, Time interframeGap, uint32_t pkt_size);
	void ChangeRate(Ptr<RdmaQueuePair> qp, DataRate new_rate);
	/******************************
	 * Mellanox's version of DCQCN
	 *****************************/
	double m_g; //feedback weight
	double m_rateOnFirstCNP; // the fraction of line rate to set on first CNP
	bool m_EcnClampTgtRate;
	double m_rpgTimeReset;
	double m_rateDecreaseInterval;
	uint32_t m_rpgThreshold;
	double m_alpha_resume_interval;
	DataRate m_rai;		//< Rate of additive increase
	DataRate m_rhai;		//< Rate of hyper-additive increase

	// the Mellanox's version of alpha update:
	// every fixed time slot, update alpha.
	void UpdateAlphaMlx(Ptr<RdmaQueuePair> q);
	void ScheduleUpdateAlphaMlx(Ptr<RdmaQueuePair> q);

	// Mellanox's version of CNP receive
	void cnp_received_mlx(Ptr<RdmaQueuePair> q);

	// Mellanox's version of rate decrease
	// It checks every m_rateDecreaseInterval if CNP arrived (m_decrease_cnp_arrived).
	// If so, decrease rate, and reset all rate increase related things
	void CheckRateDecreaseMlx(Ptr<RdmaQueuePair> q);
	void ScheduleDecreaseRateMlx(Ptr<RdmaQueuePair> q, uint32_t delta);

	// Mellanox's version of rate increase
	void RateIncEventTimerMlx(Ptr<RdmaQueuePair> q);
	void RateIncEventMlx(Ptr<RdmaQueuePair> q);
	void FastRecoveryMlx(Ptr<RdmaQueuePair> q);
	void ActiveIncreaseMlx(Ptr<RdmaQueuePair> q);
	void HyperIncreaseMlx(Ptr<RdmaQueuePair> q);

	/***********************
	 * High Precision CC
	 ***********************/
	double m_targetUtil;
	double m_utilHigh;
	uint32_t m_miThresh;
	bool m_multipleRate;
	bool m_sampleFeedback; // only react to feedback every RTT, or qlen > 0
	void HandleAckHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);
	void UpdateRateHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react);
	void UpdateRateHpTest(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react);
	void FastReactHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);

	/**********************
	 * TIMELY
	 *********************/
	double m_tmly_alpha, m_tmly_beta;
	uint64_t m_tmly_TLow, m_tmly_THigh, m_tmly_minRtt;
	void HandleAckTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);
	void UpdateRateTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool us);
	void FastReactTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);

	/**********************
	 * DCTCP
	 *********************/
	DataRate m_dctcp_rai;
	void HandleAckDctcp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);

	/*********************
	 * HPCC-PINT
	 ********************/
	uint32_t pint_smpl_thresh;
	void SetPintSmplThresh(double p);
	void HandleAckHpPint(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);
	void UpdateRateHpPint(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react);
};

} /* namespace ns3 */

#endif /* RDMA_HW_H */
