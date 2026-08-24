#ifndef GPU_H
#define GPU_H

#include "ns3/network-module.h"
#include "ns3/core-module.h"
#include "ns3/type-id.h"
#include "ns3/ipv4-address.h"
#include "msccl.h"
#include <ostream>
#include <map>
#include <utility>
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

		// RDMA-fabric (switch/nvswitch) peers, as opposed to direct p2p peers above
		void SetMyIp(Ipv4Address addr);
		Ipv4Address GetMyIp() const;
		void PushPeerIpAddr(int16_t peer, Ipv4Address addr);
		Ipv4Address GetPeerIpAddr(int16_t peer) const;
		bool HasPeerIpAddr(int16_t peer) const;

		// per-peer RDMA pacing: bandwidth-delay-product window (bytes) and base
		// RTT (ns) over the path to that peer, used to bound in-flight bytes on
		// RdmaQueuePairs (mirrors astra-sim's pairBdp/pairRtt)
		void PushPeerWin(int16_t peer, uint32_t winBytes);
		uint32_t GetPeerWin(int16_t peer) const;
		void PushPeerBaseRtt(int16_t peer, uint64_t rttNs);
		uint64_t GetPeerBaseRtt(int16_t peer) const;

		// The NIC (ifIndex) the parsed schedule routes this connection out of, set by
		// AlgoTopology::ParseSwitchJson. Keyed by (peer, channel) because that is the identity
		// of one persistent RDMA connection, and RdmaHw binds a qp to a single NIC for its
		// lifetime. Absent (GetPeerNic returns false) means "unscheduled" -- RdmaHw then falls
		// back to hashing across the equal-cost NICs, which is the behavior for every topology
		// without a switch JSON.
		void PushPeerNic(int16_t peer, int chan, uint32_t ifIndex);
		bool GetPeerNic(int16_t peer, int chan, uint32_t& ifIndex) const;

		private:
		int m_maxNChannels;
		struct mscclAlgorithm m_algo;
		std::map<int16_t, std::vector<Ptr<NetDevice>>> m_recvDevicePeer;
		std::map<int16_t, std::vector<Ptr<NetDevice>>> m_sendDevicePeer;
		std::map<int16_t, std::vector<Address>> m_sendPeerAddr;
		Ipv4Address m_myIp;
		std::map<int16_t, Ipv4Address> m_peerIpAddr;
		std::map<int16_t, uint32_t> m_peerWin;
		std::map<int16_t, uint64_t> m_peerBaseRtt;
		std::map<std::pair<int16_t, int>, uint32_t> m_peerNic;
	};
}
#endif 
