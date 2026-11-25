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
    
    QbbHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper1.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("100Gbps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), nvswtches.Get(0));
    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), nvswtches.Get(0));
    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), nvswtches.Get(1));
    NetDeviceContainer devs0_3 = link_helper0.Install(gpunodes.Get(3), nvswtches.Get(1));
    NetDeviceContainer devs0_4 = link_helper0.Install(gpunodes.Get(4), nvswtches.Get(2));
    NetDeviceContainer devs0_5 = link_helper0.Install(gpunodes.Get(5), nvswtches.Get(2));
    NetDeviceContainer devs0_6 = link_helper0.Install(gpunodes.Get(6), nvswtches.Get(3));
    NetDeviceContainer devs0_7 = link_helper0.Install(gpunodes.Get(7), nvswtches.Get(3));
    NetDeviceContainer devs1_8 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(0));
    NetDeviceContainer devs1_9 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(1));
    NetDeviceContainer devs1_10 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(2));
    NetDeviceContainer devs1_11 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(3));
    NetDeviceContainer devs1_12 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(4));
    NetDeviceContainer devs1_13 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(5));
    NetDeviceContainer devs1_14 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(6));
    NetDeviceContainer devs1_15 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(7));
    NetDeviceContainer devs1_16 = link_helper1.Install(regswtches.Get(0), regswtches.Get(2));
    NetDeviceContainer devs1_17 = link_helper1.Install(regswtches.Get(0), regswtches.Get(3));
    NetDeviceContainer devs1_18 = link_helper1.Install(regswtches.Get(1), regswtches.Get(2));
    NetDeviceContainer devs1_19 = link_helper1.Install(regswtches.Get(1), regswtches.Get(3));
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