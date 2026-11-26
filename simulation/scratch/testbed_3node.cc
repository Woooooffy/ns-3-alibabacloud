// The 3-node hardware testbed (ja2-ja4) as a star: one V100 per host, one ConnectX-6 HDR NIC
// per host, all three hosts on one InfiniBand switch. 3 GPUs, 3 host nodes, 1 switch.
//
//     gpu_ja2 --staging-- host_ja2 --IB--+
//     gpu_ja3 --staging-- host_ja3 --IB--+-- ib_switch
//     gpu_ja4 --staging-- host_ja4 --IB--+
//
// GPUDirect RDMA is off on the testbed, so every network transfer is staged through pinned host
// memory, and staging (about 65 Gbps), not the NIC (about 195 Gbps), limits a GPU. Staging is
// its own link, GPU to host, in front of the NIC link, host to switch. The host is an
// NVSwitchNode: a self-routing forwarder with no switch program, which is what a host bounce
// buffer is. This is the two-hop model (testbed_3node.topo.tmpl), the fabric every planner's
// testbed schedule runs on. Planners that see only GPUs solved the flat model
// (testbed_3node_flat.topo.tmpl), but their schedules name only GPUs, so they run here as well.
//
// The topology body is generated, not hand-written. The testbed is measured anew each day, and
// each day's filled .topo lives in the experiments repo (topologies/testbed_past/). To move
// this scratch to another day:
//     experiments/scripts/sim/gen_scratch.py \
//         experiments/topologies/testbed_past/testbed_3node_<day>.topo  scratch/testbed_3node.cc
// The generated block below names the file it came from. Schedules are only valid against
// the day they were solved for, so check that it matches the solve's day.
//
// Ranks: GPU k is slot k of that day's .ranks file (gpu_ja2, gpu_ja3, gpu_ja4 = ranks 0, 1, 2).
// That is the GPU numbering of every planner's schedule.
//
// Inputs: scratch/xml_input/testbed_3node_<a2a|ag>[_<sched>][_no_rate].xml, named the way
// mini_harness.h derives it. --xml=<file> runs any XML in xml_input directly. No schedule here
// has a switch JSON: there is one switch and one path between any two GPUs, so there are no
// flow ids to route by and no NIC to choose. Hence the two defaults below, --flowId=0 and
// --nicSel=merged. They are the sweep driver's `baseline` flags, so its baseline, rate,
// netDeps, milp and p2p configs run here. flowId+nic, nicSel, noRate, noNetDeps and all ask
// for a JSON and stop with an error that says so.
//
//     ./ns3 run "testbed_3node --xml=testbed_3node_a2a_teccl.xml --inputBytes=3000000 --rate=0"
// Testbed sizes are per-GPU chunks in decimal MB, so that is the 1 MB solve: 3 x 10^6 bytes per rank.

#include "mini_harness.h"

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("TESTBED_3NODE");
    LogComponentEnable("AlgoTopo", LOG_LEVEL_WARN);

    mini::Options opt = mini::Options::For("testbed_3node", 3);
    opt.flowId = false;
    opt.nicSel = "merged";
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

    // ---- topology: generated from experiments/topologies/testbed_past/testbed_3node_2026-11-25.topo ----
    // by experiments/scripts/sim/gen_scratch.py; edit the .topo, not this block.
    // rdma values of the .topo not used here (the harness sets its own): CcMode=12 L2AckInterval=1 L2ChunkSize=4000
    constexpr uint32_t kTopoMtu = 4096;
    for (uint32_t i = 0; i < 3; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 1; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    for (uint32_t i = 0; i < 3; ++i) { nvswtches.Add(CreateObject<NVSwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper0.SetChannelAttribute("Delay", StringValue("11818ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("64800Mbps"));

    QbbHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper1.SetChannelAttribute("Delay", StringValue("11842ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("64960Mbps"));

    QbbHelper link_helper2;
    link_helper2.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper2.SetChannelAttribute("Delay", StringValue("11828ns"));
    link_helper2.SetDeviceAttribute("DataRate", StringValue("64960Mbps"));

    QbbHelper link_helper3;
    link_helper3.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper3.SetChannelAttribute("Delay", StringValue("603ns"));
    link_helper3.SetDeviceAttribute("DataRate", StringValue("195470Mbps"));

    QbbHelper link_helper4;
    link_helper4.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper4.SetChannelAttribute("Delay", StringValue("597ns"));
    link_helper4.SetDeviceAttribute("DataRate", StringValue("195470Mbps"));

    QbbHelper link_helper5;
    link_helper5.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper5.SetChannelAttribute("Delay", StringValue("603ns"));
    link_helper5.SetDeviceAttribute("DataRate", StringValue("195360Mbps"));

    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), nvswtches.Get(0));
    NetDeviceContainer devs1_1 = link_helper1.Install(gpunodes.Get(1), nvswtches.Get(1));
    NetDeviceContainer devs2_2 = link_helper2.Install(gpunodes.Get(2), nvswtches.Get(2));
    NetDeviceContainer devs3_3 = link_helper3.Install(nvswtches.Get(0), regswtches.Get(0));
    NetDeviceContainer devs4_4 = link_helper4.Install(nvswtches.Get(1), regswtches.Get(0));
    NetDeviceContainer devs5_5 = link_helper5.Install(nvswtches.Get(2), regswtches.Get(0));
    // ---- end generated topology ----

    opt.mtu = kTopoMtu;
    opt.SetRdmaDefaults();

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);

    return mini::Run(opt, gpunodes, regswtches, nvswtches);
}
