#include "switch.h"
#include "msccl-header.h"
namespace ns3{
	NS_OBJECT_ENSURE_REGISTERED (SmartSwitch);

	NS_LOG_COMPONENT_DEFINE("SmartSwitch");

	TypeId SmartSwitch::GetTypeId (void){
		static TypeId tid = TypeId ("ns3::SmartSwitch")
			.SetParent<BridgeNetDevice>()
			.SetGroupName("Bridge")
			.AddConstructor<SmartSwitch>();
		return tid;
	}

	void SmartSwitch::DisableLearning(){
		m_enableLearning = false;
	}

	void SmartSwitch::AddForwardingRule(uint32_t flowId, Ptr<NetDevice> port){
		m_forwarding_table[flowId] = port;
	}

	void SmartSwitch::SetSharedTraceMap(std::map<uint32_t, std::vector<int>>* map){
		m_shared_trace = map;
	}

	void SmartSwitch::ForwardUnicast(Ptr<NetDevice> device, Ptr<const Packet> packet, uint16_t protocol, Mac48Address src, Mac48Address dst){
		if (m_seen_packets.contains(packet->GetUid())){
			NS_LOG_WARN("Received duplicate packet " << packet->GetUid());
			std::cout << "Received duplicate packet " << packet->GetUid() << std::endl;
			auto& vec = (*m_shared_trace)[packet->GetUid()];
			std::cout << "Path: [";
			for (size_t i = 0; i < vec.size(); ++i){
				std::cout << vec[i];
				if (i + 1 < vec.size()) {
					std::cout << ", ";
				}
			}
			std::cout << "]\n";
		}
		m_seen_packets.insert(packet->GetUid());
		if (m_enableLearning) Learn(src, device);
		if (protocol == COLLECTIVES_PROTOCOL){	
			// Smart switch behavior for collectives
			MscclHeader hdr;
			if (packet->GetSize() < hdr.GetSerializedSize()) NS_FATAL_ERROR("Received packet with incomplete header.");
			packet->PeekHeader(hdr);
			if (auto search = m_forwarding_table.find(hdr.GetFlowId()); search != m_forwarding_table.end()){
				Ptr<NetDevice> port = search->second;	
				if (port != device){
					(*m_shared_trace)[packet->GetUid()].push_back(GetNode()->GetId());	
					port->SendFrom(packet->Copy(), src, dst, protocol);
					return;
				}
			}
		}
		// All other cases default to bridge behavior
		BridgeNetDevice::ForwardUnicast(device, packet, protocol, src, dst);
	}

}
