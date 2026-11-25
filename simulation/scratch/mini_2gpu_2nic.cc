// VARIANT 3 of the mini_* controlled set: 4 hosts x 2 GPUs, ONE NIC PER GPU. 8 GPUs, 4 host
// switches, 2 leaves, 2 spines -- the richest of the three.
//
// Each GPU owns its own 100Gbps fabric port, so a GPU can reach the network without crossing
// the host interior at all; the intra-host switch becomes an optional relay rather than the
// mandatory funnel of mini_2gpu_1nic.cc. Host egress is therefore 200Gbps against variant
// 2's 100, and the fabric is doubled in width to match: 4 downlinks per leaf against 2 x
// 100Gbps of uplink, still exactly 2:1, so the taper argument and the 4/3 separation between
// the one-spine and two-spine bounds carry over unchanged.
//
// Both of a host's NICs land on the SAME leaf, so --nicSel has little to decide here: every
// setting reaches the same switch. The knob that matters on this fabric is --flowId, which
// is what makes a leaf spread its traffic over both spines the way the solve intended
// instead of letting ECMP hash it.
//
// regswtches 0/1 = leaf0/leaf1, 2/3 = spine0/spine1, matching the switch JSON's
// switch_id_map; the JSON's "port" fields are ns-3 device indices into the Install() order
// below (leaf0 ports 0-3 = gpus 0-3, ports 4/5 = the two spines). Regenerate the topology
// body from the DSL rather than editing the link list by hand; the harness is in
// mini_harness.h.
//
// Inputs: scratch/xml_input/mini_2g2n_a2a.xml and scratch/json_input/mini_2g2n_a2a.json.

#include "mini_harness.h"

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("MINI_2GPU_2NIC");
    LogComponentEnable("AlgoTopo", LOG_LEVEL_WARN);

    mini::Options opt = mini::Options::For("mini_2g2n", 8);
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

    // ---- topology: generated from topology/dsl-frontend/examples/mini_2gpu_2nic.topo ----
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
    // ---- end generated topology ----

    opt.SetRdmaDefaults();

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);

    return mini::Run(opt, gpunodes, regswtches, nvswtches);
}
