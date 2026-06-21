#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/distributed-ml-module.h"

using namespace ns3;

int main(int argc, char *argv[]) {
    NodeContainer gpunodes;
    NodeContainer regswtches;
    NodeContainer nvswtches;
    
    for (uint32_t i = 0; i < 2; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 2; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper0.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("25GBps"));
    
    QbbHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper1.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("100GBps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), regswtches.Get(0));
    
    NetDeviceContainer devs1_1 = link_helper1.Install(regswtches.Get(0), regswtches.Get(1));
    
    NetDeviceContainer devs1_2 = link_helper1.Install(regswtches.Get(0), regswtches.Get(1));
    
    NetDeviceContainer devs0_3 = link_helper0.Install(gpunodes.Get(1), regswtches.Get(1));
    
    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    InternetStackHelper internetStack;
    internetStack.Install(gpunodes);
    
    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.1");
        NetDeviceContainer _tmp;
        _tmp.Add(devs0_0.Get(0));
        _ipv4.Assign(_tmp);
    }
    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.2");
        NetDeviceContainer _tmp;
        _tmp.Add(devs0_3.Get(0));
        _ipv4.Assign(_tmp);
    }
    
    // SwitchNode routing tables (BFS ECMP)
    {
        Ipv4Address _dst("10.0.0.1");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs0_0.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.2");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs1_1.Get(0)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.2");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs1_2.Get(0)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.1");
        DynamicCast<SwitchNode>(regswtches.Get(1))->AddTableEntry(_dst, devs1_1.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.1");
        DynamicCast<SwitchNode>(regswtches.Get(1))->AddTableEntry(_dst, devs1_2.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.2");
        DynamicCast<SwitchNode>(regswtches.Get(1))->AddTableEntry(_dst, devs0_3.Get(1)->GetIfIndex());
    }
    
    // RdmaHw + RdmaDriver setup per GPU
    {
        Ptr<RdmaHw> rdmaHw_gpu0 = CreateObject<RdmaHw>();
        rdmaHw_gpu0->SetAttribute("GPUsPerServer", UintegerValue(1));
        Ptr<RdmaDriver> rdmaDriver_gpu0 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu0->SetNode(gpunodes.Get(0));
        rdmaDriver_gpu0->SetRdmaHw(rdmaHw_gpu0);
        rdmaDriver_gpu0->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu0->AddTableEntry(_peerIp, devs0_0.Get(0)->GetIfIndex(), false);
        }
        
        DynamicCast<GPU>(gpunodes.Get(0))->SetMyIp(Ipv4Address("10.0.0.1"));
        gpunodes.Get(0)->AggregateObject(rdmaDriver_gpu0);
    }
    {
        Ptr<RdmaHw> rdmaHw_gpu1 = CreateObject<RdmaHw>();
        rdmaHw_gpu1->SetAttribute("GPUsPerServer", UintegerValue(1));
        Ptr<RdmaDriver> rdmaDriver_gpu1 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu1->SetNode(gpunodes.Get(1));
        rdmaDriver_gpu1->SetRdmaHw(rdmaHw_gpu1);
        rdmaDriver_gpu1->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu1->AddTableEntry(_peerIp, devs0_3.Get(0)->GetIfIndex(), false);
        }
        
        DynamicCast<GPU>(gpunodes.Get(1))->SetMyIp(Ipv4Address("10.0.0.2"));
        gpunodes.Get(1)->AggregateObject(rdmaDriver_gpu1);
    }
    
    // peer IP bookkeeping for the collectives app's RDMA-routed peers
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    
    // custom flow-id forwarding rules (bypasses ECMP for matching flows)
    DynamicCast<SwitchNode>(regswtches.Get(0))->SetAttribute("CustomFlowForwarding", BooleanValue(true));
    DynamicCast<SwitchNode>(regswtches.Get(0))->AddFlowForwardingRule(65536, devs0_0.Get(1)->GetIfIndex());
    DynamicCast<SwitchNode>(regswtches.Get(0))->AddFlowForwardingRule(1, devs1_1.Get(0)->GetIfIndex());
    
    
    /*
        n0 -> sw0: devs0_0
        sw0 -> sw1: devs1_2
        n1 -> sw1: devs0_3
    */
    
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}