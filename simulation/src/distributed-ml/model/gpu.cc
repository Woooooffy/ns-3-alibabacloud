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
	
}
