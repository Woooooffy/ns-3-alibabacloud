#include <ns3/hash.h>
#include <ns3/uinteger.h>
#include <ns3/seq-ts-header.h>
#include <ns3/udp-header.h>
#include <ns3/ipv4-header.h>
#include <ns3/simulator.h>
#include "ns3/ppp-header.h"
#include "rdma-queue-pair.h"

namespace ns3 {

/**************************
 * RdmaQueuePair
 *************************/
TypeId RdmaQueuePair::GetTypeId (void)
{
static TypeId tid = TypeId ("ns3::RdmaQueuePair")
		.SetParent<Object> ()
		;
	return tid;
}

RdmaQueuePair::RdmaQueuePair(uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport){
	startTime = Simulator::Now();
	sip = _sip;
	dip = _dip;
	sport = _sport;
	dport = _dport;
	m_size = 0;
	m_init_size = 0;
	m_src = -1;
	m_dest = -1;
	m_tag = -1;
	m_mscclFlowId = MscclFlowIdHeader::NO_FLOW_ID;
	m_closed = false;
	m_autoClose = true;
	snd_nxt = snd_una = 0;
	m_pg = pg;
	m_ipid = 0;
	m_win = 0;
	m_baseRtt = 0;
	m_max_rate = 0;
	m_var_win = false;
	m_rate = 0;
	lastPktRate = 0;
	m_nextAvail = Time(0);
	mlx.m_alpha = 1;
	mlx.m_alpha_cnp_arrived = false;
	mlx.m_first_cnp = true;
	mlx.m_decrease_cnp_arrived = false;
	mlx.m_rpTimeStage = 0;
	hp.m_lastUpdateSeq = 0;
	for (uint32_t i = 0; i < sizeof(hp.keep) / sizeof(hp.keep[0]); i++)
		hp.keep[i] = 0;
	hp.m_incStage = 0;
	hp.m_lastGap = 0;
	hp.u = 1;
	for (uint32_t i = 0; i < IntHeader::maxHop; i++){
		hp.hopState[i].u = 1;
		hp.hopState[i].incStage = 0;
	}

	tmly.m_lastUpdateSeq = 0;
	tmly.m_incStage = 0;
	tmly.lastRtt = 0;
	tmly.rttDiff = 0;

	dctcp.m_lastUpdateSeq = 0;
	dctcp.m_caState = 0;
	dctcp.m_highSeq = 0;
	dctcp.m_alpha = 1;
	dctcp.m_ecnCnt = 0;
	dctcp.m_batchSizeOfAlpha = 0;

	hpccPint.m_lastUpdateSeq = 0;
	hpccPint.m_incStage = 0;
}

void RdmaQueuePair::SetSize(uint64_t size){
	m_size = size;
}

void RdmaQueuePair::SetSrc(uint32_t src){
	m_src = src;
}

void RdmaQueuePair::SetDest(uint32_t dest){
	m_dest = dest;
}

uint32_t RdmaQueuePair::GetSrc(){
	return m_src;
}

uint32_t RdmaQueuePair::GetDest(){
	return m_dest;
}

void RdmaQueuePair::SetTag(uint64_t tag){
	m_tag = tag;
}

uint64_t RdmaQueuePair::GetTag(){
	return m_tag;
}

void RdmaQueuePair::SetMscclFlowId(uint32_t mscclFlowId){
	m_mscclFlowId = mscclFlowId;
}

uint32_t RdmaQueuePair::GetMscclFlowId(){
	return m_mscclFlowId;
}

void RdmaQueuePair::SetInitialSize(uint64_t size){
	m_init_size = size;
}

uint64_t RdmaQueuePair::GetInitialSize(){
	return m_init_size;
}

void RdmaQueuePair::SetWin(uint32_t win){
	m_win = win;
	// std::cout << "set win: " << m_win << std::endl;
}

void RdmaQueuePair::SetBaseRtt(uint64_t baseRtt){
	m_baseRtt = baseRtt;
}

void RdmaQueuePair::SetVarWin(bool v){
	m_var_win = v;
}

void RdmaQueuePair::PushMessage(uint64_t size, uint8_t* srcDataPtr, uint32_t mscclFlowId, Callback<void> notifyAppFinish, Callback<void> notifyAppSent, DataRate rate){
	// Wake-from-idle: when a message lands on a qp whose message queue was empty, the qp has
	// been idle and its m_nextAvail is stale. Under rate targeting (accumulating pacer) start
	// the shaper clock fresh at Now so no credit banked before the idle gap is applied. Only
	// on the empty->backlogged edge -- appending behind an in-flight message must not reset
	// the pacer. No-op when m_pacerAccumulate is false (legacy reset-to-now pacing).
	if (m_pacerAccumulate && m_messages.empty())
		m_nextAvail = Simulator::Now();
	RdmaMessage msg;
	msg.m_size = size;
	msg.m_startSeq = m_messages.empty() ? snd_nxt : (m_messages.back().m_startSeq + m_messages.back().m_size);
	msg.m_srcDataPtr = srcDataPtr;
	msg.m_mscclFlowId = mscclFlowId;
	msg.m_rate = rate;
	msg.m_notifyAppFinish = notifyAppFinish;
	msg.m_notifyAppSent = notifyAppSent;
	m_messages.push_back(msg);
	// m_size/m_init_size are informational only (monitoring/print output); no longer drive
	// GetBytesLeft/IsFinished, which are computed relative to the message queue below.
	m_size += size;
	if (m_init_size == 0)
		m_init_size = size;
}

void RdmaQueuePair::FinishMessage(){
	NS_ASSERT_MSG(!m_messages.empty(), "RdmaQueuePair::FinishMessage(): message queue is empty");
	RdmaMessage msg = m_messages.front();
	m_messages.pop_front();
	if (!msg.m_notifyAppFinish.IsNull())
		msg.m_notifyAppFinish();
}

bool RdmaQueuePair::IsCurMessageFinished(){
	if (m_messages.empty())
		return true;
	return snd_una >= m_messages.front().m_startSeq + m_messages.front().m_size;
}

// Walks forward from the oldest unacknowledged message to the one snd_nxt sits in. Messages
// are laid out contiguously in sequence space (PushMessage chains each m_startSeq onto the
// previous message's end), so this is just a scan for the element whose range covers snd_nxt.
//
// Two things stop the scan. Running off the end means every queued byte has already been put
// on the wire and the qp is simply waiting for acks. Reaching m_maxMsgsInFlight means the
// pipelining depth is exhausted: index i counts messages from the front, so a sending message
// at index i implies i+1 messages in flight, and i must stay below the limit. Either way the
// qp reports nothing to send until an ack retires the front message -- and RdmaHw's ack
// handler calls TriggerTransmit afterwards, which is what re-polls the NIC once credit frees.
const RdmaQueuePair::RdmaMessage* RdmaQueuePair::GetSendingMessage(){
	uint64_t seq = snd_nxt;
	for (uint32_t i = 0; i < m_messages.size(); i++){
		if (i >= m_maxMsgsInFlight)
			return nullptr; // pipelining depth exhausted; wait for an ack to retire the front
		const RdmaMessage& msg = m_messages[i];
		if (seq < msg.m_startSeq + msg.m_size)
			return &msg;
	}
	return nullptr; // everything queued is already on the wire
}

uint8_t* RdmaQueuePair::GetCurSrcDataPtr(){
	const RdmaMessage* msg = GetSendingMessage();
	return msg == nullptr ? nullptr : msg->m_srcDataPtr;
}

uint64_t RdmaQueuePair::GetCurMsgStartSeq(){
	const RdmaMessage* msg = GetSendingMessage();
	return msg == nullptr ? snd_nxt : msg->m_startSeq;
}

uint32_t RdmaQueuePair::GetCurMscclFlowId(){
	const RdmaMessage* msg = GetSendingMessage();
	return msg == nullptr ? GetMscclFlowId() : msg->m_mscclFlowId;
}

DataRate RdmaQueuePair::GetCurRate(){
	const RdmaMessage* msg = GetSendingMessage();
	return msg == nullptr ? DataRate(0) : msg->m_rate;
}

// Bytes the qp may send right now, which is what makes it eligible for the NIC's arbiter
// (see QbbNetDevice::DequeueQindex). Deliberately capped at the end of the *sending* message
// rather than spanning the whole backlog: a packet must never straddle a message boundary,
// since RdmaHw::GetNxtPacket indexes each message's own source buffer relative to that
// message's m_startSeq, and consecutive messages on a persistent qp generally come from
// unrelated buffers. Returning 0 means "nothing sendable now" -- either fully sent, or
// blocked on pipelining depth -- not "the qp is done"; IsFinished() answers that.
uint64_t RdmaQueuePair::GetBytesLeft(){
	const RdmaMessage* msg = GetSendingMessage();
	if (msg == nullptr)
		return 0;
	uint64_t end = msg->m_startSeq + msg->m_size;
	return end >= snd_nxt ? end - snd_nxt : 0;
}

uint32_t RdmaQueuePair::GetHash(void){
	union{
		struct {
			uint32_t sip, dip;
			uint16_t sport, dport;
		};
		char c[12];
	} buf;
	buf.sip = sip.Get();
	buf.dip = dip.Get();
	buf.sport = sport;
	buf.dport = dport;
	return Hash32(buf.c, 12);
}

void RdmaQueuePair::Acknowledge(uint64_t ack){
	if (ack > snd_una){
		snd_una = ack;
	}
}

uint64_t RdmaQueuePair::GetOnTheFly(){
	return snd_nxt - snd_una;
}

bool RdmaQueuePair::IsWinBound(){
	uint64_t w = GetWin();
	return w != 0 && GetOnTheFly() >= w;
}

uint64_t RdmaQueuePair::GetWin(){
	if (m_win == 0)
		return 0;
	uint64_t w;
	if (m_var_win){
		w = m_win * m_rate.GetBitRate() / m_max_rate.GetBitRate();
		if (w == 0)
			w = 1; // must > 0
	}else{
		w = m_win;
	}
	return w;
}

uint64_t RdmaQueuePair::HpGetCurWin(){
	if (m_win == 0)
		return 0;
	uint64_t w;
	if (m_var_win){
		w = m_win * hp.m_curRate.GetBitRate() / m_max_rate.GetBitRate();
		if (w == 0)
			w = 1; // must > 0
	}else{
		w = m_win;
	}
	return w;
}

bool RdmaQueuePair::IsFinished(){
	return m_messages.empty();
}

/*********************
 * RdmaRxQueuePair
 ********************/
TypeId RdmaRxQueuePair::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::RdmaRxQueuePair")
		.SetParent<Object> ()
		;
	return tid;
}

RdmaRxQueuePair::RdmaRxQueuePair(){
	sip = dip = sport = dport = 0;
	m_ipid = 0;
	ReceiverNextExpectedSeq = 0;
	m_nackTimer = Time(0);
	m_milestone_rx = 0;
	m_lastNACK = 0;
}

uint32_t RdmaRxQueuePair::GetHash(void){
	union{
		struct {
			uint32_t sip, dip;
			uint16_t sport, dport;
		};
		char c[12];
	} buf;
	buf.sip = sip;
	buf.dip = dip;
	buf.sport = sport;
	buf.dport = dport;
	return Hash32(buf.c, 12);
}

/*********************
 * RdmaQueuePairGroup
 ********************/
TypeId RdmaQueuePairGroup::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::RdmaQueuePairGroup")
		.SetParent<Object> ()
		;
	return tid;
}

RdmaQueuePairGroup::RdmaQueuePairGroup(void){
}

uint32_t RdmaQueuePairGroup::GetN(void){
	return m_qps.size();
}

Ptr<RdmaQueuePair> RdmaQueuePairGroup::Get(uint32_t idx){
	return m_qps[idx];
}

Ptr<RdmaQueuePair> RdmaQueuePairGroup::operator[](uint32_t idx){
	return m_qps[idx];
}

void RdmaQueuePairGroup::AddQp(Ptr<RdmaQueuePair> qp){
	m_qps.push_back(qp);
}

#if 0
void RdmaQueuePairGroup::AddRxQp(Ptr<RdmaRxQueuePair> rxQp){
	m_rxQps.push_back(rxQp);
}
#endif

void RdmaQueuePairGroup::Clear(void){
	m_qps.clear();
}

}
