#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/distributed-ml-module.h"

#include <vector>

using namespace ns3;

int main(int argc, char *argv[]) {
    NodeContainer gpunodes;
    NodeContainer regswtches;
    NodeContainer nvswtches;
    
    // PFC backpressure (CheckAndSendPfc) runs unconditionally in SwitchNode, but only
    // has an effect once QcnEnabled lets a stalled NIC's queue resume; ECN marking is
    // separately gated per-switch by the EcnEnabled attribute set below.
    Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(true));
    
    for (uint32_t i = 0; i < 4; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 4; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper0.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("400Gbps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(0));
    
    NetDeviceContainer devs0_1 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(1));
    
    NetDeviceContainer devs0_2 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(2));
    
    NetDeviceContainer devs0_3 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(3));
    
    NetDeviceContainer devs0_4 = link_helper0.Install(regswtches.Get(0), regswtches.Get(2));
    
    NetDeviceContainer devs0_5 = link_helper0.Install(regswtches.Get(0), regswtches.Get(3));
    
    NetDeviceContainer devs0_6 = link_helper0.Install(regswtches.Get(1), regswtches.Get(2));
    
    NetDeviceContainer devs0_7 = link_helper0.Install(regswtches.Get(1), regswtches.Get(3));
    
    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    InternetStackHelper internetStack;
    internetStack.Install(gpunodes);
    
    {
        struct _IpAssign { Ptr<NetDevice> dev; const char* hostMask; };
        std::vector<_IpAssign> _ipAssigns = {
            {devs0_0.Get(1), "0.0.0.1"},
            {devs0_1.Get(1), "0.0.0.2"},
            {devs0_2.Get(1), "0.0.0.3"},
            {devs0_3.Get(1), "0.0.0.4"},
        };
        for (auto& a : _ipAssigns) {
            Ipv4AddressHelper _ipv4;
            _ipv4.SetBase("10.0.0.0", "255.0.0.0", a.hostMask);
            NetDeviceContainer _tmp;
            _tmp.Add(a.dev);
            _ipv4.Assign(_tmp);
        }
    }
    
    // SwitchNode routing tables (BFS ECMP)
    {
        struct _Route { Ptr<SwitchNode> node; Ipv4Address dst; Ptr<NetDevice> dev; };
        std::vector<_Route> _routes = {
            {DynamicCast<SwitchNode>(regswtches.Get(0)), Ipv4Address("10.0.0.1"), devs0_0.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(0)), Ipv4Address("10.0.0.2"), devs0_1.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(0)), Ipv4Address("10.0.0.3"), devs0_4.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(0)), Ipv4Address("10.0.0.3"), devs0_5.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(0)), Ipv4Address("10.0.0.4"), devs0_4.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(0)), Ipv4Address("10.0.0.4"), devs0_5.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(1)), Ipv4Address("10.0.0.1"), devs0_6.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(1)), Ipv4Address("10.0.0.1"), devs0_7.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(1)), Ipv4Address("10.0.0.2"), devs0_6.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(1)), Ipv4Address("10.0.0.2"), devs0_7.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(1)), Ipv4Address("10.0.0.3"), devs0_2.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(1)), Ipv4Address("10.0.0.4"), devs0_3.Get(0)},
            {DynamicCast<SwitchNode>(regswtches.Get(2)), Ipv4Address("10.0.0.1"), devs0_4.Get(1)},
            {DynamicCast<SwitchNode>(regswtches.Get(2)), Ipv4Address("10.0.0.2"), devs0_4.Get(1)},
            {DynamicCast<SwitchNode>(regswtches.Get(2)), Ipv4Address("10.0.0.3"), devs0_6.Get(1)},
            {DynamicCast<SwitchNode>(regswtches.Get(2)), Ipv4Address("10.0.0.4"), devs0_6.Get(1)},
            {DynamicCast<SwitchNode>(regswtches.Get(3)), Ipv4Address("10.0.0.1"), devs0_5.Get(1)},
            {DynamicCast<SwitchNode>(regswtches.Get(3)), Ipv4Address("10.0.0.2"), devs0_5.Get(1)},
            {DynamicCast<SwitchNode>(regswtches.Get(3)), Ipv4Address("10.0.0.3"), devs0_7.Get(1)},
            {DynamicCast<SwitchNode>(regswtches.Get(3)), Ipv4Address("10.0.0.4"), devs0_7.Get(1)},
        };
        for (auto& r : _routes) {
            r.node->AddTableEntry(r.dst, r.dev->GetIfIndex());
        }
    }
    
    // RdmaHw + RdmaDriver setup per GPU
    {
        struct _RdmaRoute { Ipv4Address dst; uint32_t ifIndex; bool isNvswitch; };
        struct _GpuRdmaSetup { Ptr<Node> gpu; Ipv4Address myIp; std::vector<_RdmaRoute> routes; };
        std::vector<_GpuRdmaSetup> _gpuSetups = {
            {gpunodes.Get(0), Ipv4Address("10.0.0.1"), {{Ipv4Address("10.0.0.2"), devs0_0.Get(1)->GetIfIndex(), false}, {Ipv4Address("10.0.0.3"), devs0_0.Get(1)->GetIfIndex(), false}, {Ipv4Address("10.0.0.4"), devs0_0.Get(1)->GetIfIndex(), false}}},
            {gpunodes.Get(1), Ipv4Address("10.0.0.2"), {{Ipv4Address("10.0.0.1"), devs0_1.Get(1)->GetIfIndex(), false}, {Ipv4Address("10.0.0.3"), devs0_1.Get(1)->GetIfIndex(), false}, {Ipv4Address("10.0.0.4"), devs0_1.Get(1)->GetIfIndex(), false}}},
            {gpunodes.Get(2), Ipv4Address("10.0.0.3"), {{Ipv4Address("10.0.0.1"), devs0_2.Get(1)->GetIfIndex(), false}, {Ipv4Address("10.0.0.2"), devs0_2.Get(1)->GetIfIndex(), false}, {Ipv4Address("10.0.0.4"), devs0_2.Get(1)->GetIfIndex(), false}}},
            {gpunodes.Get(3), Ipv4Address("10.0.0.4"), {{Ipv4Address("10.0.0.1"), devs0_3.Get(1)->GetIfIndex(), false}, {Ipv4Address("10.0.0.2"), devs0_3.Get(1)->GetIfIndex(), false}, {Ipv4Address("10.0.0.3"), devs0_3.Get(1)->GetIfIndex(), false}}},
        };
        for (auto& g : _gpuSetups) {
            Ptr<RdmaHw> rdmaHw = CreateObject<RdmaHw>();
            rdmaHw->SetAttribute("GPUsPerServer", UintegerValue(1));
            Ptr<RdmaDriver> rdmaDriver = CreateObject<RdmaDriver>();
            rdmaDriver->SetNode(g.gpu);
            rdmaDriver->SetRdmaHw(rdmaHw);
            rdmaDriver->Init();
            for (auto& r : g.routes) {
                rdmaHw->AddTableEntry(r.dst, r.ifIndex, r.isNvswitch);
            }
            DynamicCast<GPU>(g.gpu)->SetMyIp(g.myIp);
            g.gpu->AggregateObject(rdmaDriver);
        }
    }
    
    // peer IP bookkeeping for the collectives app's RDMA-routed peers
    {
        struct _PeerIp { Ptr<Node> gpu; int16_t peerIdx; Ipv4Address peerIp; };
        std::vector<_PeerIp> _peerIps = {
            {gpunodes.Get(1), 0, Ipv4Address("10.0.0.1")},
            {gpunodes.Get(1), 2, Ipv4Address("10.0.0.3")},
            {gpunodes.Get(1), 3, Ipv4Address("10.0.0.4")},
            {gpunodes.Get(2), 0, Ipv4Address("10.0.0.1")},
            {gpunodes.Get(2), 1, Ipv4Address("10.0.0.2")},
            {gpunodes.Get(2), 3, Ipv4Address("10.0.0.4")},
            {gpunodes.Get(3), 0, Ipv4Address("10.0.0.1")},
            {gpunodes.Get(3), 1, Ipv4Address("10.0.0.2")},
            {gpunodes.Get(3), 2, Ipv4Address("10.0.0.3")},
            {gpunodes.Get(0), 1, Ipv4Address("10.0.0.2")},
            {gpunodes.Get(0), 2, Ipv4Address("10.0.0.3")},
            {gpunodes.Get(0), 3, Ipv4Address("10.0.0.4")},
        };
        for (auto& p : _peerIps) {
            DynamicCast<GPU>(p.gpu)->PushPeerIpAddr(p.peerIdx, p.peerIp);
        }
    }
    
    // peer RDMA pacing: bandwidth-delay-product window + base RTT per peer
    {
        struct _PeerPacing { Ptr<Node> gpu; int16_t peerIdx; uint32_t winBytes; uint64_t baseRttNs; };
        std::vector<_PeerPacing> _peerPacing = {
            {gpunodes.Get(1), 0, 158000, 3160},
            {gpunodes.Get(1), 2, 316000, 6320},
            {gpunodes.Get(1), 3, 316000, 6320},
            {gpunodes.Get(2), 0, 316000, 6320},
            {gpunodes.Get(2), 1, 316000, 6320},
            {gpunodes.Get(2), 3, 158000, 3160},
            {gpunodes.Get(3), 0, 316000, 6320},
            {gpunodes.Get(3), 1, 316000, 6320},
            {gpunodes.Get(3), 2, 158000, 3160},
            {gpunodes.Get(0), 1, 158000, 3160},
            {gpunodes.Get(0), 2, 316000, 6320},
            {gpunodes.Get(0), 3, 316000, 6320},
        };
        for (auto& p : _peerPacing) {
            DynamicCast<GPU>(p.gpu)->PushPeerWin(p.peerIdx, p.winBytes);
            DynamicCast<GPU>(p.gpu)->PushPeerBaseRtt(p.peerIdx, p.baseRttNs);
        }
    }
    
    // switch/nvswitch MMU: PFC headroom + ECN thresholds per port (otherwise
    // SwitchMmu's headroom[]/kmin[]/kmax[]/pmax[]/pfc_a_shift[] are uninitialized,
    // which disables realistic PFC backpressure under incast)
    {
        struct _MmuNode { Ptr<Node> node; Ptr<SwitchMmu> mmu; uint32_t nPorts; bool isSwitch; };
        std::vector<_MmuNode> _mmuNodes = {
            {regswtches.Get(0), DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu, 4, true},
            {regswtches.Get(1), DynamicCast<SwitchNode>(regswtches.Get(1))->m_mmu, 4, true},
            {regswtches.Get(2), DynamicCast<SwitchNode>(regswtches.Get(2))->m_mmu, 2, true},
            {regswtches.Get(3), DynamicCast<SwitchNode>(regswtches.Get(3))->m_mmu, 2, true},
        };
        for (auto& n : _mmuNodes) {
            if (n.isSwitch) n.node->SetAttribute("EcnEnabled", BooleanValue(true));
            n.mmu->ConfigNPort(n.nPorts);
            n.mmu->node_id = n.node->GetId();
        }
    }
    
    {
        struct _MmuPort { Ptr<SwitchMmu> mmu; uint32_t port; uint32_t headroomBytes; uint32_t kminKB; uint32_t kmaxKB; double pmax; uint32_t shift; };
        std::vector<_MmuPort> _mmuPorts = {
            {DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu, devs0_0.Get(0)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu, devs0_1.Get(0)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu, devs0_4.Get(0)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu, devs0_5.Get(0)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(1))->m_mmu, devs0_2.Get(0)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(1))->m_mmu, devs0_3.Get(0)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(1))->m_mmu, devs0_6.Get(0)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(1))->m_mmu, devs0_7.Get(0)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(2))->m_mmu, devs0_4.Get(1)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(2))->m_mmu, devs0_6.Get(1)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(3))->m_mmu, devs0_5.Get(1)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(3))->m_mmu, devs0_7.Get(1)->GetIfIndex(), 105000, 200, 400, 0.2, 3},
        };
        for (auto& p : _mmuPorts) {
            p.mmu->ConfigEcn(p.port, p.kminKB, p.kmaxKB, p.pmax);
            p.mmu->ConfigHdrm(p.port, p.headroomBytes);
            p.mmu->pfc_a_shift[p.port] = p.shift;
        }
    }
    
    
    /*
        p_esw1_sw -> p_esw1_gpu1: devs0_0
        p_esw1_sw -> p_esw1_gpu2: devs0_1
        p_esw2_sw -> p_esw2_gpu1: devs0_2
        p_esw2_sw -> p_esw2_gpu2: devs0_3
        p_esw1_sw -> p_asw1: devs0_4
        p_esw1_sw -> p_asw2: devs0_5
        p_esw2_sw -> p_asw1: devs0_6
        p_esw2_sw -> p_asw2: devs0_7
    */
    
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}