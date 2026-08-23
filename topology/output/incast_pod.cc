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
    for (uint32_t i = 0; i < 3; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper0.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("400Gbps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(0));
    NetDeviceContainer devs0_1 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(1));
    NetDeviceContainer devs0_2 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(2));
    NetDeviceContainer devs0_3 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(3));
    NetDeviceContainer devs0_4 = link_helper0.Install(regswtches.Get(0), regswtches.Get(2));
    NetDeviceContainer devs0_5 = link_helper0.Install(regswtches.Get(1), regswtches.Get(2));
    Config::SetDefault("ns3::RdmaHw::CcMode", UintegerValue(12));
    Config::SetDefault("ns3::RdmaHw::L2AckInterval", UintegerValue(0));
    Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(4096));
    
    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);
    
    
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}