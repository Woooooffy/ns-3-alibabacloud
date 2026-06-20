#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/csma-module.h"
#include "ns3/ethernet-switch-module.h"
#include "ns3/distributed-training-module.h"
#include "ns3/qbb-helper.h"
#include "ns3/qbb-net-device.h"
#include "ns3/rdma-hw.h"
#include "ns3/rdma-driver.h"

using namespace ns3;

int main(int argc, char *argv[]) {
    NodeContainer gpunodes;
    NodeContainer swtches;
    NodeContainer regswtches;
    P4Helper sw_helper;
    sw_helper.SetDeviceAttribute("EnableCustomImpl", BooleanValue(true));
    
    gpunodes.Create<GPU>(3);
    swtches.Create(0);
    regswtches.Create<SwitchNode>(1);
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper0.SetChannelAttribute("Delay", StringValue("1us"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("200Gbps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), regswtches.Get(0));
    
    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), regswtches.Get(0));
    
    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), regswtches.Get(0));
    
    
    // ---- RegSwitch (SwitchNode / QBB / RDMA) setup ----
    
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
        // CC_MODE {Specifying different CC. 1: DCQCN, 3: HPCC, 7: TIMELY, 8: DCTCP, 10: HPCC-PINT}
        rdmaHw_gpu0->SetAttribute("CcMode", UintegerValue(12));
        Ptr<RdmaDriver> rdmaDriver_gpu0 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu0->SetNode(gpunodes.Get(0));
        rdmaDriver_gpu0->SetRdmaHw(rdmaHw_gpu0);
        rdmaDriver_gpu0->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu0->AddTableEntry(_peerIp, DynamicCast<QbbNetDevice>(devs0_0.Get(0))->GetIfIndex());
        }
        {
            Ipv4Address _peerIp("10.0.0.3");
            rdmaHw_gpu0->AddTableEntry(_peerIp, DynamicCast<QbbNetDevice>(devs0_0.Get(0))->GetIfIndex());
        }
        
        rdmaHw_gpu0->SetDataRecvCallback(MakeCallback(&GPU::OnRdmaDataRecv, DynamicCast<GPU>(gpunodes.Get(0))));
        
        DynamicCast<GPU>(gpunodes.Get(0))->SetMyIp(Ipv4Address("10.0.0.1"));
        DynamicCast<GPU>(gpunodes.Get(0))->SetRdmaDriver(rdmaDriver_gpu0);
    }
    {
        Ptr<RdmaHw> rdmaHw_gpu1 = CreateObject<RdmaHw>();
        // CC_MODE {Specifying different CC. 1: DCQCN, 3: HPCC, 7: TIMELY, 8: DCTCP, 10: HPCC-PINT}
        rdmaHw_gpu1->SetAttribute("CcMode", UintegerValue(12));
        Ptr<RdmaDriver> rdmaDriver_gpu1 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu1->SetNode(gpunodes.Get(1));
        rdmaDriver_gpu1->SetRdmaHw(rdmaHw_gpu1);
        rdmaDriver_gpu1->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.3");
            rdmaHw_gpu1->AddTableEntry(_peerIp, DynamicCast<QbbNetDevice>(devs0_1.Get(0))->GetIfIndex());
        }
        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu1->AddTableEntry(_peerIp, DynamicCast<QbbNetDevice>(devs0_1.Get(0))->GetIfIndex());
        }
        
        rdmaHw_gpu1->SetDataRecvCallback(MakeCallback(&GPU::OnRdmaDataRecv, DynamicCast<GPU>(gpunodes.Get(1))));
        
        DynamicCast<GPU>(gpunodes.Get(1))->SetMyIp(Ipv4Address("10.0.0.2"));
        DynamicCast<GPU>(gpunodes.Get(1))->SetRdmaDriver(rdmaDriver_gpu1);
    }
    {
        Ptr<RdmaHw> rdmaHw_gpu2 = CreateObject<RdmaHw>();
        // CC_MODE {Specifying different CC. 1: DCQCN, 3: HPCC, 7: TIMELY, 8: DCTCP, 10: HPCC-PINT}
        rdmaHw_gpu2->SetAttribute("CcMode", UintegerValue(12));
        Ptr<RdmaDriver> rdmaDriver_gpu2 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu2->SetNode(gpunodes.Get(2));
        rdmaDriver_gpu2->SetRdmaHw(rdmaHw_gpu2);
        rdmaDriver_gpu2->Init();
        
        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu2->AddTableEntry(_peerIp, DynamicCast<QbbNetDevice>(devs0_2.Get(0))->GetIfIndex());
        }
        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu2->AddTableEntry(_peerIp, DynamicCast<QbbNetDevice>(devs0_2.Get(0))->GetIfIndex());
        }
        
        rdmaHw_gpu2->SetDataRecvCallback(MakeCallback(&GPU::OnRdmaDataRecv, DynamicCast<GPU>(gpunodes.Get(2))));
        
        DynamicCast<GPU>(gpunodes.Get(2))->SetMyIp(Ipv4Address("10.0.0.3"));
        DynamicCast<GPU>(gpunodes.Get(2))->SetRdmaDriver(rdmaDriver_gpu2);
    }
    
    // PushSendPeerDevice and PushPeerIpAddr for switch-fabric pairs
    DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(2, devs0_1.Get(0));
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(2, Ipv4Address("10.0.0.3"));
    DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(0, devs0_1.Get(0));
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(1, devs0_2.Get(0));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(0, devs0_2.Get(0));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(1, devs0_0.Get(0));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(2, devs0_0.Get(0));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(2, Ipv4Address("10.0.0.3"));
    
    
    /*
    */
    
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}