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
    
    for (uint32_t i = 0; i < 3; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 1; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(1500));
    link_helper0.SetChannelAttribute("Delay", StringValue("1us"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("130Gbps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), regswtches.Get(0));
    
    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), regswtches.Get(0));
    
    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), regswtches.Get(0));
    
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
        _tmp.Add(devs0_1.Get(0));
        _ipv4.Assign(_tmp);
    }
    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.3");
        NetDeviceContainer _tmp;
        _tmp.Add(devs0_2.Get(0));
        _ipv4.Assign(_tmp);
    }
    
    // SwitchNode routing tables (BFS ECMP)
    {
        Ipv4Address _dst("10.0.0.1");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs0_0.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.2");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs0_1.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.3");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs0_2.Get(1)->GetIfIndex());
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
        {
            Ipv4Address _peerIp("10.0.0.3");
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
            rdmaHw_gpu1->AddTableEntry(_peerIp, devs0_1.Get(0)->GetIfIndex(), false);
        }
        {
            Ipv4Address _peerIp("10.0.0.3");
            rdmaHw_gpu1->AddTableEntry(_peerIp, devs0_1.Get(0)->GetIfIndex(), false);
        }
        
        DynamicCast<GPU>(gpunodes.Get(1))->SetMyIp(Ipv4Address("10.0.0.2"));
        gpunodes.Get(1)->AggregateObject(rdmaDriver_gpu1);
    }
    {
        Ptr<RdmaHw> rdmaHw_gpu2 = CreateObject<RdmaHw>();
        rdmaHw_gpu2->SetAttribute("GPUsPerServer", UintegerValue(1));
        Ptr<RdmaDriver> rdmaDriver_gpu2 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu2->SetNode(gpunodes.Get(2));
        rdmaDriver_gpu2->SetRdmaHw(rdmaHw_gpu2);
        rdmaDriver_gpu2->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu2->AddTableEntry(_peerIp, devs0_2.Get(0)->GetIfIndex(), false);
        }
        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu2->AddTableEntry(_peerIp, devs0_2.Get(0)->GetIfIndex(), false);
        }
        
        DynamicCast<GPU>(gpunodes.Get(2))->SetMyIp(Ipv4Address("10.0.0.3"));
        gpunodes.Get(2)->AggregateObject(rdmaDriver_gpu2);
    }
    
    // peer IP bookkeeping for the collectives app's RDMA-routed peers
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(2, Ipv4Address("10.0.0.3"));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(2, Ipv4Address("10.0.0.3"));
    
    
    /*
        n0 -> sw: devs0_0
        n1 -> sw: devs0_1
        n2 -> sw: devs0_2
    */
    
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}
