#ifndef CUSTOM_SWITCH_H
#define CUSTOM_SWITCH_H

#include "ns3/node.h"
#include "ns3/net-device.h"
#include "ns3/packet.h"
#include "ns3/queue.h"
#include "ns3/object-factory.h"
#include <vector>
#include <unordered_set>
#include <string>

namespace ns3 {
class P4SwitchNetDevice;

class CustomSwitchImpl : public Object {
	public:
		CustomSwitchImpl();
		~CustomSwitchImpl();
		static TypeId GetTypeId(void);
		void AddPort();
		void AddForwardingRule(uint32_t flowId, uint32_t outport);
		void AddAddrForwarding(Address addr, uint32_t port);
		void DisableLearning();
		void SetSwitchNetDevice(Ptr<P4SwitchNetDevice> dev); 
		void SetSharedTraceMap(std::map<uint32_t, std::vector<int>>* map);
		void SetQueueType(std::string type);
		void SetQueueMaxSize(uint32_t maxPackets);
		void ReceivePacket(Ptr<Packet> packetIn,
                      int inPort, uint16_t protocol,
                      const Address& destination, NetDevice::PacketType type);
		void TryPortEgress(int port);
		void PortEgressComplete(int port);
		private:
			Ptr<P4SwitchNetDevice> m_switchNetDevice;
			bool m_learning = true;
			std::map<uint32_t, uint32_t> m_forwarding_table;
			// write to a tracing table shared between all switches
			// for routes that are pre-programmed (not learned)
			std::map<Address, uint32_t> m_addr_forwarding_table; // fallback for learning or default
			std::map<uint32_t, std::vector<int>>* m_shared_trace = nullptr;
			std::unordered_set<uint32_t> m_seen_packets;
			enum PortTxState{
				READY,
				BUSY,
			};
			std::vector<PortTxState> m_portTxStates;
			ObjectFactory m_queueFactory;
			std::vector<Ptr<Queue<Packet>>> m_portQueues;
}; // CustomSwitchImpl
}//namespace

#endif
