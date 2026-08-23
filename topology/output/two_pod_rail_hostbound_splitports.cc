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
    
    for (uint32_t i = 0; i < 16; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 6; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    for (uint32_t i = 0; i < 8; ++i) { nvswtches.Add(CreateObject<NVSwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper0.SetChannelAttribute("Delay", StringValue("350ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("900GBps"));
    
    QbbHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper1.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("25GBps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), nvswtches.Get(0));
    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), nvswtches.Get(0));
    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), nvswtches.Get(1));
    NetDeviceContainer devs0_3 = link_helper0.Install(gpunodes.Get(3), nvswtches.Get(1));
    NetDeviceContainer devs0_4 = link_helper0.Install(gpunodes.Get(4), nvswtches.Get(2));
    NetDeviceContainer devs0_5 = link_helper0.Install(gpunodes.Get(5), nvswtches.Get(2));
    NetDeviceContainer devs0_6 = link_helper0.Install(gpunodes.Get(6), nvswtches.Get(3));
    NetDeviceContainer devs0_7 = link_helper0.Install(gpunodes.Get(7), nvswtches.Get(3));
    NetDeviceContainer devs0_8 = link_helper0.Install(gpunodes.Get(8), nvswtches.Get(4));
    NetDeviceContainer devs0_9 = link_helper0.Install(gpunodes.Get(9), nvswtches.Get(4));
    NetDeviceContainer devs0_10 = link_helper0.Install(gpunodes.Get(10), nvswtches.Get(5));
    NetDeviceContainer devs0_11 = link_helper0.Install(gpunodes.Get(11), nvswtches.Get(5));
    NetDeviceContainer devs0_12 = link_helper0.Install(gpunodes.Get(12), nvswtches.Get(6));
    NetDeviceContainer devs0_13 = link_helper0.Install(gpunodes.Get(13), nvswtches.Get(6));
    NetDeviceContainer devs0_14 = link_helper0.Install(gpunodes.Get(14), nvswtches.Get(7));
    NetDeviceContainer devs0_15 = link_helper0.Install(gpunodes.Get(15), nvswtches.Get(7));
    NetDeviceContainer devs1_16 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(0));
    NetDeviceContainer devs1_17 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(1));
    NetDeviceContainer devs1_18 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(2));
    NetDeviceContainer devs1_19 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(3));
    NetDeviceContainer devs1_20 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(4));
    NetDeviceContainer devs1_21 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(5));
    NetDeviceContainer devs1_22 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(6));
    NetDeviceContainer devs1_23 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(7));
    NetDeviceContainer devs1_24 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(8));
    NetDeviceContainer devs1_25 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(9));
    NetDeviceContainer devs1_26 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(10));
    NetDeviceContainer devs1_27 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(11));
    NetDeviceContainer devs1_28 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(12));
    NetDeviceContainer devs1_29 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(13));
    NetDeviceContainer devs1_30 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(14));
    NetDeviceContainer devs1_31 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(15));
    NetDeviceContainer devs1_32 = link_helper1.Install(regswtches.Get(0), regswtches.Get(4));
    NetDeviceContainer devs1_33 = link_helper1.Install(regswtches.Get(0), regswtches.Get(4));
    NetDeviceContainer devs1_34 = link_helper1.Install(regswtches.Get(0), regswtches.Get(5));
    NetDeviceContainer devs1_35 = link_helper1.Install(regswtches.Get(1), regswtches.Get(4));
    NetDeviceContainer devs1_36 = link_helper1.Install(regswtches.Get(1), regswtches.Get(4));
    NetDeviceContainer devs1_37 = link_helper1.Install(regswtches.Get(1), regswtches.Get(5));
    NetDeviceContainer devs1_38 = link_helper1.Install(regswtches.Get(2), regswtches.Get(4));
    NetDeviceContainer devs1_39 = link_helper1.Install(regswtches.Get(2), regswtches.Get(4));
    NetDeviceContainer devs1_40 = link_helper1.Install(regswtches.Get(2), regswtches.Get(5));
    NetDeviceContainer devs1_41 = link_helper1.Install(regswtches.Get(3), regswtches.Get(4));
    NetDeviceContainer devs1_42 = link_helper1.Install(regswtches.Get(3), regswtches.Get(4));
    NetDeviceContainer devs1_43 = link_helper1.Install(regswtches.Get(3), regswtches.Get(5));
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