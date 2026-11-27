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
// buffer is. The staging link's LATENCY is not left on the link: all but 1 us of it becomes a
// per-message send overhead (--sendOverheadNs; see after the generated topology), so a message
// still takes the measured end-to-end time but ACKs don't pay it. This is the two-hop model
// (testbed_3node.topo.tmpl), the fabric every planner's testbed schedule runs on. Planners that see only GPUs solved the flat model
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
//     ./ns3 run "testbed_3node --xml=testbed_3node_a2a_teccl-two-alltoall-1MB.xml --inputBytes=3145728"
// Sizes are base 2 (KB = 2^10, MB = 2^20, as nccl-tests counts them), per-GPU chunks C: AllToAll's
// --inputBytes is a rank's whole send buffer, 3 x C (3 x 2^20 for 1 MB), and AllGather's is its one
// chunk, C, with --coll=allgather. scratch/sweep_testbed.py runs every schedule this way.

#include "mini_harness.h"

#include <cmath>

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("TESTBED_3NODE");
    LogComponentEnable("AlgoTopo", LOG_LEVEL_WARN);

    mini::Options opt = mini::Options::For("testbed_3node", 3);
    opt.flowId = false;
    opt.nicSel = "merged";
    // The MSCCL kernel's gridOffset step on the testbed's runtime: NCCL Simple's 4 MiB buffer over
    // NCCL_STEPS = 8 is 512 KiB per FIFO step, and msccl_kernel.cu advances MSCCL_CHUNKSTEPS = 4
    // steps per iteration, 2 MiB. Dependencies between threadblocks are checked per iteration, so
    // a send gated on a receive starts after the receive's first 2 MiB, as on the hardware.
    // (mini_harness.h's default, 0, waits for the whole chunk.) --protoChunkBytes overrides it.
    opt.protoChunkBytes = 2u << 20;
    // Host-staging latency as a per-message send overhead instead of a GPU-host link delay; see
    // the block after the generated topology. -1 derives it there; --sendOverheadNs=0 (or any
    // value >= 0) keeps the generated link delays as they are and uses that overhead instead.
    opt.sendOverheadNs = -1;
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

    // ---- host staging: off the GPU-host links, into a per-message send overhead ----
    // The .topo's staging latency (~11.8 us per GPU) is half an 8-byte NCCL sendrecv between two
    // GPUs, minus the IB hop: mostly host-side work per message (the GPU-host copy, the proxy
    // thread picking it up), measured once per message. As a link delay, every packet and every
    // ACK pays it, so an ACK round trip costs ~50 us, and a connection with more messages than
    // its 8-message window waits one of those per extra window (OptCCL AllToAll K16/K32 at small
    // sizes ran 2-3x the hardware's time). Charged instead once per message, on the send side, a
    // message still takes the measured end-to-end time, and ACKs cost only the wires.
    //
    // Each GPU-host link keeps kStagingWireNs of its delay, not 0: RdmaFabricHelper sizes every
    // switch port's PFC headroom as rate x delay x 3, which would be zero. The overhead is
    // 2 x (staging delay - kStagingWireNs) averaged over the GPUs: a message pays both ends'
    // staging, its sender's and its receiver's, and the per-GPU values differ by < 0.1 us.
    constexpr int64_t kStagingWireNs = 1000;
    if (opt.sendOverheadNs < 0) {
        double movedNs = 0;
        uint32_t nLinks = 0;
        for (uint32_t g = 0; g < gpunodes.GetN(); ++g) {
            Ptr<Node> gpu = gpunodes.Get(g);
            for (uint32_t d = 0; d < gpu->GetNDevices(); ++d) {
                Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(gpu->GetDevice(d));
                if (!dev) continue;
                Ptr<QbbChannel> ch = DynamicCast<QbbChannel>(dev->GetChannel());
                if (!ch) continue;
                Ptr<NetDevice> peer = (ch->GetDevice(0) == dev) ? ch->GetDevice(1) : ch->GetDevice(0);
                if (!DynamicCast<NVSwitchNode>(peer->GetNode())) continue; // only the GPU-host link
                const int64_t wasNs = ch->GetDelay().GetNanoSeconds();
                if (wasNs < kStagingWireNs)
                    NS_FATAL_ERROR("GPU " << g << "'s staging link has " << wasNs << " ns of delay, less than "
                        "the " << kStagingWireNs << " ns it keeps; pass --sendOverheadNs explicitly.");
                movedNs += wasNs - kStagingWireNs;
                ++nLinks;
                ch->SetAttribute("Delay", TimeValue(NanoSeconds(kStagingWireNs)));
            }
        }
        if (nLinks != gpunodes.GetN())
            NS_FATAL_ERROR("found " << nLinks << " GPU-host staging links for " << gpunodes.GetN() << " GPUs");
        opt.sendOverheadNs = static_cast<int64_t>(std::llround(2.0 * movedNs / nLinks));
        std::cout << "Host staging: GPU-host link delay -> " << kStagingWireNs << " ns; the rest, "
                  << movedNs / nLinks << " ns per GPU on average, charged per message as a "
                  << opt.sendOverheadNs << " ns send overhead (both ends' share)" << std::endl;
    }

    opt.mtu = kTopoMtu;
    opt.SetRdmaDefaults();

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);

    return mini::Run(opt, gpunodes, regswtches, nvswtches);
}
