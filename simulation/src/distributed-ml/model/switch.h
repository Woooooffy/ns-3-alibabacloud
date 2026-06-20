#ifndef SMART_SWITCH_H
#define SMART_SWITCH_H
#include "ns3/bridge-module.h"
#include "ns3/node.h"
#include "utils.h"
#include <vector>
#include <unordered_set>

namespace ns3{
	class SmartSwitch: public BridgeNetDevice{
		public:
	  	static TypeId GetTypeId (void);
			void AddForwardingRule(uint32_t flowId, Ptr<NetDevice> port);
			void DisableLearning();
			void SetSharedTraceMap(std::map<uint32_t, std::vector<int>>* map);

		protected:
  		virtual void ForwardUnicast(Ptr<NetDevice> device, 
				Ptr<const Packet> packet, uint16_t protocol, 
				Mac48Address src, Mac48Address dst) override;

		private:
			std::map<uint32_t, Ptr<NetDevice>> m_forwarding_table;
			// write to a tracing table shared between all switches
			// for routes that are pre-programmed (not learned)
			std::map<uint32_t, std::vector<int>>* m_shared_trace;
			std::unordered_set<uint32_t> m_seen_packets;

	}; // SmartSwitch


}// namespace ns3

#endif
