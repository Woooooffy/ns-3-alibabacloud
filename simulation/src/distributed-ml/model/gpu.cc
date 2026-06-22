#include "gpu.h"

namespace ns3
{
	NS_LOG_COMPONENT_DEFINE("GPU");

	TypeId GPU::GetTypeId (void){
		static TypeId tid = TypeId ("ns3::GPU")
			.SetParent<Node> ()
			.SetGroupName ("DistributedTraining")
			.AddConstructor<GPU> ();	
		return tid;
	}
	GPU::GPU(): Node(){};
	GPU::GPU(int maxNChannels): m_maxNChannels(maxNChannels){}
	GPU::~GPU(){}
	void GPU::SetMaxNChannels(int maxNChannels){
		m_maxNChannels = maxNChannels;
	}
	int GPU::GetMaxNChannels(){
		return m_maxNChannels;
	}
	struct mscclAlgorithm* GPU::GetAlgo(){
		return &m_algo;
	}
	std::ostream& GPU::DumpAlgo(std::ostream& oss){
		return oss << m_algo;
	}
	Ptr<NetDevice> GPU::GetSendDevicePeer(int16_t peer, int ind){
		auto& devices = m_sendDevicePeer[peer];
		// TODO consider other assignment policies
		return devices.at(ind % devices.size());
	}
	Ptr<NetDevice> GPU::GetRecvDevicePeer(int16_t peer, int ind){
		auto& devices = m_recvDevicePeer[peer];
		return devices.at(ind % devices.size());
	}
	Address GPU::GetPeerAddr(int16_t peer, int ind){
		auto& addresses = m_sendPeerAddr[peer];
		return addresses.at(ind % addresses.size());
	}
	void GPU::PushRecvPeerDevice(int16_t peer, Ptr<NetDevice> dev){
		m_recvDevicePeer[peer].push_back(dev);
	}
	void GPU::PushSendPeerDevice(int16_t peer, Ptr<NetDevice> dev){
		m_sendDevicePeer[peer].push_back(dev);
	}
	void GPU::PushPeerAddr(int16_t peer, Address addr){
		m_sendPeerAddr[peer].push_back(addr);
	}

	void GPU::SetMyIp(Ipv4Address addr){
		m_myIp = addr;
	}
	Ipv4Address GPU::GetMyIp() const{
		return m_myIp;
	}
	void GPU::PushPeerIpAddr(int16_t peer, Ipv4Address addr){
		m_peerIpAddr[peer] = addr;
	}
	Ipv4Address GPU::GetPeerIpAddr(int16_t peer) const{
		return m_peerIpAddr.at(peer);
	}
	bool GPU::HasPeerIpAddr(int16_t peer) const{
		return m_peerIpAddr.count(peer) > 0;
	}

	void GPU::PushPeerWin(int16_t peer, uint32_t winBytes){
		m_peerWin[peer] = winBytes;
	}
	uint32_t GPU::GetPeerWin(int16_t peer) const{
		auto it = m_peerWin.find(peer);
		return it != m_peerWin.end() ? it->second : 0;
	}
	void GPU::PushPeerBaseRtt(int16_t peer, uint64_t rttNs){
		m_peerBaseRtt[peer] = rttNs;
	}
	uint64_t GPU::GetPeerBaseRtt(int16_t peer) const{
		auto it = m_peerBaseRtt.find(peer);
		return it != m_peerBaseRtt.end() ? it->second : 0;
	}

}
