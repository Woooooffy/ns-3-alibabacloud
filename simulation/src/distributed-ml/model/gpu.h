#ifndef GPU_H
#define GPU_H

#include "ns3/network-module.h"
#include "ns3/core-module.h"
#include "ns3/type-id.h"
#include "msccl.h"
#include <ostream>
#include <map>
#include <vector>

namespace ns3
{
	class GPU : public Node {

		public:
		static TypeId GetTypeId (void);
		GPU();
		GPU(int maxNChannels);
		~GPU() override;		
		struct mscclAlgorithm* GetAlgo();
		void SetMaxNChannels(int maxNChannels);
		int GetMaxNChannels();
		Ptr<NetDevice> GetSendDevicePeer(int16_t peer, int ind);
		Ptr<NetDevice> GetRecvDevicePeer(int16_t peer, int ind);
		Address GetPeerAddr(int16_t peer, int ind);
		void PushRecvPeerDevice(int16_t peer, Ptr<NetDevice> dev);
		void PushSendPeerDevice(int16_t peer, Ptr<NetDevice> dev);
		void PushPeerAddr(int16_t peer, Address addr);
		std::ostream& DumpAlgo(std::ostream& oss);

		private:
		int m_maxNChannels;
		struct mscclAlgorithm m_algo;
		std::map<int16_t, std::vector<Ptr<NetDevice>>> m_recvDevicePeer;
		std::map<int16_t, std::vector<Ptr<NetDevice>>> m_sendDevicePeer;
		std::map<int16_t, std::vector<Address>> m_sendPeerAddr;
	};
}
#endif 
