#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/distributed-training-module.h"

using namespace ns3;

int main(int argc, char *argv[]) {
    NodeContainer gpunodes;
    NodeContainer regswtches;
    NodeContainer nvswtches;
    
    for (uint32_t i = 0; i < 4; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 2; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    for (uint32_t i = 0; i < 1; ++i) { nvswtches.Add(CreateObject<NVSwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper0.SetChannelAttribute("Delay", StringValue("100ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("400GBps"));
    
    QbbHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper1.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("25GBps"));
    
    QbbHelper link_helper2;
    link_helper2.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper2.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper2.SetDeviceAttribute("DataRate", StringValue("100GBps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), nvswtches.Get(0));
    
    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), nvswtches.Get(0));
    
    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), nvswtches.Get(0));
    
    NetDeviceContainer devs0_3 = link_helper0.Install(gpunodes.Get(3), nvswtches.Get(0));
    
    NetDeviceContainer devs1_4 = link_helper1.Install(gpunodes.Get(0), regswtches.Get(0));
    
    NetDeviceContainer devs1_5 = link_helper1.Install(gpunodes.Get(1), regswtches.Get(0));
    
    NetDeviceContainer devs1_6 = link_helper1.Install(gpunodes.Get(2), regswtches.Get(1));
    
    NetDeviceContainer devs1_7 = link_helper1.Install(gpunodes.Get(3), regswtches.Get(1));
    
    NetDeviceContainer devs2_8 = link_helper2.Install(regswtches.Get(0), regswtches.Get(1));
    
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
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.1");
        NetDeviceContainer _tmp;
        _tmp.Add(devs1_4.Get(0));
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
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.2");
        NetDeviceContainer _tmp;
        _tmp.Add(devs1_5.Get(0));
        _ipv4.Assign(_tmp);
    }
    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.3");
        NetDeviceContainer _tmp;
        _tmp.Add(devs0_2.Get(0));
        _ipv4.Assign(_tmp);
    }
    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.3");
        NetDeviceContainer _tmp;
        _tmp.Add(devs1_6.Get(0));
        _ipv4.Assign(_tmp);
    }
    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.4");
        NetDeviceContainer _tmp;
        _tmp.Add(devs0_3.Get(0));
        _ipv4.Assign(_tmp);
    }
    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.4");
        NetDeviceContainer _tmp;
        _tmp.Add(devs1_7.Get(0));
        _ipv4.Assign(_tmp);
    }
    
    // SwitchNode routing tables (BFS ECMP)
    {
        Ipv4Address _dst("10.0.0.1");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs1_4.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.2");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs1_5.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.3");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs2_8.Get(0)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.4");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs2_8.Get(0)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.1");
        DynamicCast<SwitchNode>(regswtches.Get(1))->AddTableEntry(_dst, devs2_8.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.2");
        DynamicCast<SwitchNode>(regswtches.Get(1))->AddTableEntry(_dst, devs2_8.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.3");
        DynamicCast<SwitchNode>(regswtches.Get(1))->AddTableEntry(_dst, devs1_6.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.4");
        DynamicCast<SwitchNode>(regswtches.Get(1))->AddTableEntry(_dst, devs1_7.Get(1)->GetIfIndex());
    }
    
    // NVSwitchNode routing tables (BFS ECMP)
    {
        Ipv4Address _dst("10.0.0.1");
        DynamicCast<NVSwitchNode>(nvswtches.Get(0))->AddTableEntry(_dst, devs0_0.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.2");
        DynamicCast<NVSwitchNode>(nvswtches.Get(0))->AddTableEntry(_dst, devs0_1.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.3");
        DynamicCast<NVSwitchNode>(nvswtches.Get(0))->AddTableEntry(_dst, devs0_2.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.4");
        DynamicCast<NVSwitchNode>(nvswtches.Get(0))->AddTableEntry(_dst, devs0_3.Get(1)->GetIfIndex());
    }
    
    // RdmaHw + RdmaDriver setup per GPU
    {
        Ptr<RdmaHw> rdmaHw_gpu0 = CreateObject<RdmaHw>();
        rdmaHw_gpu0->SetAttribute("GPUsPerServer", UintegerValue(4));
        Ptr<RdmaDriver> rdmaDriver_gpu0 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu0->SetNode(gpunodes.Get(0));
        rdmaDriver_gpu0->SetRdmaHw(rdmaHw_gpu0);
        rdmaDriver_gpu0->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu0->AddTableEntry(_peerIp, devs0_0.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu0->AddTableEntry(_peerIp, devs1_4.Get(0)->GetIfIndex(), false);
        }
        {
            Ipv4Address _peerIp("10.0.0.3");
            rdmaHw_gpu0->AddTableEntry(_peerIp, devs0_0.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.4");
            rdmaHw_gpu0->AddTableEntry(_peerIp, devs0_0.Get(0)->GetIfIndex(), true);
        }
        
        DynamicCast<GPU>(gpunodes.Get(0))->SetMyIp(Ipv4Address("10.0.0.1"));
        gpunodes.Get(0)->AggregateObject(rdmaDriver_gpu0);
    }
    {
        Ptr<RdmaHw> rdmaHw_gpu1 = CreateObject<RdmaHw>();
        rdmaHw_gpu1->SetAttribute("GPUsPerServer", UintegerValue(4));
        Ptr<RdmaDriver> rdmaDriver_gpu1 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu1->SetNode(gpunodes.Get(1));
        rdmaDriver_gpu1->SetRdmaHw(rdmaHw_gpu1);
        rdmaDriver_gpu1->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu1->AddTableEntry(_peerIp, devs0_1.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu1->AddTableEntry(_peerIp, devs1_5.Get(0)->GetIfIndex(), false);
        }
        {
            Ipv4Address _peerIp("10.0.0.3");
            rdmaHw_gpu1->AddTableEntry(_peerIp, devs0_1.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.4");
            rdmaHw_gpu1->AddTableEntry(_peerIp, devs0_1.Get(0)->GetIfIndex(), true);
        }
        
        DynamicCast<GPU>(gpunodes.Get(1))->SetMyIp(Ipv4Address("10.0.0.2"));
        gpunodes.Get(1)->AggregateObject(rdmaDriver_gpu1);
    }
    {
        Ptr<RdmaHw> rdmaHw_gpu2 = CreateObject<RdmaHw>();
        rdmaHw_gpu2->SetAttribute("GPUsPerServer", UintegerValue(4));
        Ptr<RdmaDriver> rdmaDriver_gpu2 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu2->SetNode(gpunodes.Get(2));
        rdmaDriver_gpu2->SetRdmaHw(rdmaHw_gpu2);
        rdmaDriver_gpu2->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu2->AddTableEntry(_peerIp, devs0_2.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu2->AddTableEntry(_peerIp, devs0_2.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.4");
            rdmaHw_gpu2->AddTableEntry(_peerIp, devs0_2.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.4");
            rdmaHw_gpu2->AddTableEntry(_peerIp, devs1_6.Get(0)->GetIfIndex(), false);
        }
        
        DynamicCast<GPU>(gpunodes.Get(2))->SetMyIp(Ipv4Address("10.0.0.3"));
        gpunodes.Get(2)->AggregateObject(rdmaDriver_gpu2);
    }
    {
        Ptr<RdmaHw> rdmaHw_gpu3 = CreateObject<RdmaHw>();
        rdmaHw_gpu3->SetAttribute("GPUsPerServer", UintegerValue(4));
        Ptr<RdmaDriver> rdmaDriver_gpu3 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu3->SetNode(gpunodes.Get(3));
        rdmaDriver_gpu3->SetRdmaHw(rdmaHw_gpu3);
        rdmaDriver_gpu3->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu3->AddTableEntry(_peerIp, devs0_3.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu3->AddTableEntry(_peerIp, devs0_3.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.3");
            rdmaHw_gpu3->AddTableEntry(_peerIp, devs0_3.Get(0)->GetIfIndex(), true);
        }
        {
            Ipv4Address _peerIp("10.0.0.3");
            rdmaHw_gpu3->AddTableEntry(_peerIp, devs1_7.Get(0)->GetIfIndex(), false);
        }
        
        DynamicCast<GPU>(gpunodes.Get(3))->SetMyIp(Ipv4Address("10.0.0.4"));
        gpunodes.Get(3)->AggregateObject(rdmaDriver_gpu3);
    }
    
    // peer IP bookkeeping for the collectives app's RDMA-routed peers
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(2, Ipv4Address("10.0.0.3"));
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(3, Ipv4Address("10.0.0.4"));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerIpAddr(3, Ipv4Address("10.0.0.4"));
    DynamicCast<GPU>(gpunodes.Get(3))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(3))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    DynamicCast<GPU>(gpunodes.Get(3))->PushPeerIpAddr(2, Ipv4Address("10.0.0.3"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(2, Ipv4Address("10.0.0.3"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(3, Ipv4Address("10.0.0.4"));
    
    
    /*
        n0 -> nvsw: devs0_0
        n1 -> nvsw: devs0_1
        n2 -> nvsw: devs0_2
        n3 -> nvsw: devs0_3
        n0 -> sw0: devs1_4
        n1 -> sw0: devs1_5
        n2 -> sw1: devs1_6
        n3 -> sw1: devs1_7
        sw0 -> sw1: devs2_8
    */
    
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}