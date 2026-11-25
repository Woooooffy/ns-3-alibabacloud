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
    
    for (uint32_t i = 0; i < 8; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 4; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    for (uint32_t i = 0; i < 4; ++i) { nvswtches.Add(CreateObject<NVSwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper0.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("200Gbps"));
    
    PointToPointHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper1.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("56GBps"));
    
    QbbHelper link_helper2;
    link_helper2.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper2.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper2.SetDeviceAttribute("DataRate", StringValue("100Gbps"));
    
    QbbHelper link_helper3;
    link_helper3.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper3.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper3.SetDeviceAttribute("DataRate", StringValue("50Gbps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), nvswtches.Get(0));
    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), nvswtches.Get(0));
    NetDeviceContainer devs1_2 = link_helper1.Install(gpunodes.Get(0), gpunodes.Get(1));
    DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(1, devs1_2.Get(0));
    DynamicCast<GPU>(gpunodes.Get(1))->PushRecvPeerDevice(0, devs1_2.Get(1));
    DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(0, devs1_2.Get(1));
    DynamicCast<GPU>(gpunodes.Get(0))->PushRecvPeerDevice(1, devs1_2.Get(0));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerAddr(1, (devs1_2.Get(1))->GetAddress());
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerAddr(0, (devs1_2.Get(0))->GetAddress());
    
    NetDeviceContainer devs0_3 = link_helper0.Install(gpunodes.Get(2), nvswtches.Get(1));
    NetDeviceContainer devs0_4 = link_helper0.Install(gpunodes.Get(3), nvswtches.Get(1));
    NetDeviceContainer devs1_5 = link_helper1.Install(gpunodes.Get(2), gpunodes.Get(3));
    DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(3, devs1_5.Get(0));
    DynamicCast<GPU>(gpunodes.Get(3))->PushRecvPeerDevice(2, devs1_5.Get(1));
    DynamicCast<GPU>(gpunodes.Get(3))->PushSendPeerDevice(2, devs1_5.Get(1));
    DynamicCast<GPU>(gpunodes.Get(2))->PushRecvPeerDevice(3, devs1_5.Get(0));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerAddr(3, (devs1_5.Get(1))->GetAddress());
    DynamicCast<GPU>(gpunodes.Get(3))->PushPeerAddr(2, (devs1_5.Get(0))->GetAddress());
    
    NetDeviceContainer devs0_6 = link_helper0.Install(gpunodes.Get(4), nvswtches.Get(2));
    NetDeviceContainer devs0_7 = link_helper0.Install(gpunodes.Get(5), nvswtches.Get(2));
    NetDeviceContainer devs1_8 = link_helper1.Install(gpunodes.Get(4), gpunodes.Get(5));
    DynamicCast<GPU>(gpunodes.Get(4))->PushSendPeerDevice(5, devs1_8.Get(0));
    DynamicCast<GPU>(gpunodes.Get(5))->PushRecvPeerDevice(4, devs1_8.Get(1));
    DynamicCast<GPU>(gpunodes.Get(5))->PushSendPeerDevice(4, devs1_8.Get(1));
    DynamicCast<GPU>(gpunodes.Get(4))->PushRecvPeerDevice(5, devs1_8.Get(0));
    DynamicCast<GPU>(gpunodes.Get(4))->PushPeerAddr(5, (devs1_8.Get(1))->GetAddress());
    DynamicCast<GPU>(gpunodes.Get(5))->PushPeerAddr(4, (devs1_8.Get(0))->GetAddress());
    
    NetDeviceContainer devs0_9 = link_helper0.Install(gpunodes.Get(6), nvswtches.Get(3));
    NetDeviceContainer devs0_10 = link_helper0.Install(gpunodes.Get(7), nvswtches.Get(3));
    NetDeviceContainer devs1_11 = link_helper1.Install(gpunodes.Get(6), gpunodes.Get(7));
    DynamicCast<GPU>(gpunodes.Get(6))->PushSendPeerDevice(7, devs1_11.Get(0));
    DynamicCast<GPU>(gpunodes.Get(7))->PushRecvPeerDevice(6, devs1_11.Get(1));
    DynamicCast<GPU>(gpunodes.Get(7))->PushSendPeerDevice(6, devs1_11.Get(1));
    DynamicCast<GPU>(gpunodes.Get(6))->PushRecvPeerDevice(7, devs1_11.Get(0));
    DynamicCast<GPU>(gpunodes.Get(6))->PushPeerAddr(7, (devs1_11.Get(1))->GetAddress());
    DynamicCast<GPU>(gpunodes.Get(7))->PushPeerAddr(6, (devs1_11.Get(0))->GetAddress());
    
    NetDeviceContainer devs2_12 = link_helper2.Install(regswtches.Get(0), nvswtches.Get(0));
    NetDeviceContainer devs2_13 = link_helper2.Install(regswtches.Get(0), nvswtches.Get(1));
    NetDeviceContainer devs2_14 = link_helper2.Install(regswtches.Get(1), nvswtches.Get(2));
    NetDeviceContainer devs2_15 = link_helper2.Install(regswtches.Get(1), nvswtches.Get(3));
    NetDeviceContainer devs3_16 = link_helper3.Install(regswtches.Get(0), regswtches.Get(2));
    NetDeviceContainer devs3_17 = link_helper3.Install(regswtches.Get(0), regswtches.Get(3));
    NetDeviceContainer devs3_18 = link_helper3.Install(regswtches.Get(1), regswtches.Get(2));
    NetDeviceContainer devs3_19 = link_helper3.Install(regswtches.Get(1), regswtches.Get(3));
    Config::SetDefault("ns3::RdmaHw::CcMode", UintegerValue(12));
    Config::SetDefault("ns3::RdmaHw::L2AckInterval", UintegerValue(0));
    Config::SetDefault("ns3::RdmaHw::L2ChunkSize", UintegerValue(4000));
    Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(4096));
    
    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);
    
    
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}