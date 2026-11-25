// VARIANT 2 of the mini_* controlled set: 4 hosts x 2 GPUs sharing ONE PCIe-attached NIC.
// 8 GPUs, 4 root complexes (modelled as NVSwitchNodes -- see below), 2 leaves, 2 spines.
//
// The host interior is a plain PCIe box: both GPUs hang off a root complex over PCIe x16
// (200Gbps) and ALSO have a direct 56 GBps NVLink bridge to each other, but the bridge does
// not reach the network. There is exactly one NIC and it hangs off the root complex, so
// every fabric byte either GPU sends or receives crosses its PCIe link and then the shared
// 100Gbps port. That funnel is the whole point of this variant, and the per-NIC bandwidth
// trace (--nicBwInterval) is what measures it.
//
// The root complex is a self-routing element -- traffic passes through it but no forwarding
// entry is installed on it -- which is exactly what NVSwitchNode models here, so the DSL
// writes `type=pcie` and folds it to nvswitch. Consequence for --nicSel: the GPUs have no
// link to a programmable switch, so ParseSwitchJson's ingress-hop lookup finds nothing and
// reports 0 connections pinned. Expected, not a failure -- with one NIC per host there is no
// NIC choice to make, and all three --nicSel values are the same run.
//
// The fabric is bit-for-bit the one in mini_1gpu_1nic.cc (2 hosts per leaf, 2:1 taper), so
// the two differ only in the host interior. regswtches 0/1 = leaf0/leaf1, 2/3 =
// spine0/spine1, matching the switch JSON's switch_id_map; its "port" fields index the
// Install() order below, where a leaf's ports 0/1 face root complexes (the JSON names the
// GPU behind each, which ParseSwitchJson treats as documentation since "port" is
// authoritative). Regenerate from the DSL rather than editing the link list here.
//
// Inputs: scratch/xml_input/mini_2g1n_a2a.xml and scratch/json_input/mini_2g1n_a2a.json.

#include "mini_harness.h"

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("MINI_2GPU_1NIC");
    LogComponentEnable("AlgoTopo", LOG_LEVEL_WARN);

    mini::Options opt = mini::Options::For("mini_2g1n", 8);
    CommandLine cmd;
    opt.AddTo(cmd);
    cmd.Parse(argc, argv);
    opt.Validate();

    NodeContainer gpunodes;
    NodeContainer regswtches;
    NodeContainer nvswtches;

    // PFC backpressure (CheckAndSendPfc) runs unconditionally in SwitchNode, but only
    // has an effect once QcnEnabled lets a stalled NIC's queue resume; ECN marking is
    // separately gated per-switch by the EcnEnabled attribute. This must precede any
    // device creation, so it stays ahead of the generated link list below.
    Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(true));

    // ---- topology: generated from topology/dsl-frontend/examples/mini_2gpu_1nic.topo ----
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
    // ---- end generated topology ----

    opt.SetRdmaDefaults();

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);

    return mini::Run(opt, gpunodes, regswtches, nvswtches);
}
