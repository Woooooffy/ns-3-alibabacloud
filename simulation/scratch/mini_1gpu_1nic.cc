// VARIANT 1 of the mini_* controlled set: 4 hosts x 1 GPU, 1 NIC each, behind a 2:1
// tapered leaf-spine fabric. 4 GPUs, 2 leaves, 2 spines, no host tier at all.
//
// The fabric is identical in all three mini_* scratches -- 2 hosts per leaf, both leaves
// meshed to both spines, twice as much leaf downlink as uplink -- so the set isolates the
// host interior and nothing else. The taper is what makes multipath load-bearing: at 2:1 a
// single spine bounds the collective at 4V/E against the 3V/E egress bound, so a schedule
// that uses both spines strictly beats one that does not, and --flowId (per-flow switch
// forwarding from the TE-CCL JSON, versus ECMP hashing) is the knob that decides whether
// the solve's split actually happens. The derivation is written out in full in
// topology/dsl-frontend/examples/mini_1gpu_1nic.topo.
//
// Node order below IS the switch JSON's switch_id_map order: regswtches 0/1 = leaf0/leaf1,
// 2/3 = spine0/spine1, and the JSON's "port" fields are ns-3 device indices into exactly
// the Install() order written here. Regenerate the topology body from the DSL
// (`python3 topology/main.py topology/dsl-frontend/examples/mini_1gpu_1nic.topo`) rather
// than editing the link list by hand; the harness lives in mini_harness.h.
//
// Inputs: scratch/xml_input/mini_1g1n_a2a.xml and scratch/json_input/mini_1g1n_a2a.json.
//
// --sched=milp swaps in the variant solve (xml_input/mini_1g1n_a2a_milp[_no_rate].xml plus
// json_input/mini_1g1n_a2a_milp.json), which fixes strictly one chunk per (src, dst) GPU pair
// and so takes exactly one path per pair. With every dynamism knob off it is the "baseline
// baseline": a direct, ECMP-forwarded, unpaced schedule that makes no multipath decision --
//     ./ns3 run "scratch/mini_1gpu_1nic --sched=milp --rate=0 --netDeps=0 --flowId=0 --nicSel=merged"
// The taper means this is expected to be SLOWER than the default solve; that gap is the point.

#include "mini_harness.h"

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("MINI_1GPU_1NIC");
    LogComponentEnable("AlgoTopo", LOG_LEVEL_WARN);

    mini::Options opt = mini::Options::For("mini_1g1n", 4);
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

    // ---- topology: generated from topology/dsl-frontend/examples/mini_1gpu_1nic.topo ----
    for (uint32_t i = 0; i < 4; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 4; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper0.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("100Gbps"));
    
    QbbHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper1.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("50Gbps"));
    
    NetDeviceContainer devs0_0 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(0));
    NetDeviceContainer devs0_1 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(1));
    NetDeviceContainer devs0_2 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(2));
    NetDeviceContainer devs0_3 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(3));
    NetDeviceContainer devs1_4 = link_helper1.Install(regswtches.Get(0), regswtches.Get(2));
    NetDeviceContainer devs1_5 = link_helper1.Install(regswtches.Get(0), regswtches.Get(3));
    NetDeviceContainer devs1_6 = link_helper1.Install(regswtches.Get(1), regswtches.Get(2));
    NetDeviceContainer devs1_7 = link_helper1.Install(regswtches.Get(1), regswtches.Get(3));
    // ---- end generated topology ----

    opt.SetRdmaDefaults();

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);

    return mini::Run(opt, gpunodes, regswtches, nvswtches);
}
