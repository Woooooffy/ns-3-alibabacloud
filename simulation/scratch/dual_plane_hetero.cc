// DualPlaneHetero, CLUSTERED leaf assignment: 96 GPUs (12 nodes x 8 rails) behind 8
// NVSwitches, fanning out to 10 regular switches (leaves/spines of the "dual plane" fabric).
// GPU->leaf assignment is CLUSTERED: each leaf owns one contiguous 32-GPU block
// (regswtches 0/1/2 <- gpus 0-31/32-63/64-95). The harness around the topology (algorithm
// parse, correctness check, congestion monitoring) follows two_pod_rail_hostbound.cc.
//
// Inputs follow the "dual_plane_clustered_<coll>[_no_rate].xml" / "dual_plane_clustered_<coll>.json"
// naming; the switch JSON's switch_id_map (0..9 -> TE-CCL ids) matches the regswtches
// declaration order above.

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/distributed-ml-module.h"
// explicit rather than relying on the module umbrellas: the per-NIC bandwidth trace reads
// RdmaHw::tx_bytes directly and resolves each link's far end through QbbChannel
#include "ns3/rdma-driver.h"
#include "ns3/rdma-hw.h"
#include "ns3/qbb-channel.h"
#include "ns3/nvswitch-node.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <tuple>

using namespace ns3;

// ---- event-driven congestion monitoring ----------------------------------------------
// These are TracedCallbacks fired synchronously from inside existing packet events on the
// switch egress ports (QbbNetDevice's QbbEnqueue/QbbDequeue/QbbDrop/QbbPfc trace sources).
// Because they run as a side effect of events already in the queue, they schedule NOTHING
// of their own -- the simulator's event list, its natural termination, and Simulator::Now()
// (hence the reported algorithm latency/bandwidth) are all completely unaffected.

// running per-(switch id, port ifIndex, priority queue) egress occupancy in bytes,
// reconstructed from enqueue/dequeue deltas so each row carries the exact post-event depth.
static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int64_t> g_qBytes;

// QbbEnqueue: fires just before a packet is pushed onto egress queue `qIndex`.
static void OnSwitchEnqueue(FILE* out, uint32_t swId, uint32_t port, Ptr<const Packet> p, uint32_t qIndex) {
    int64_t& depth = g_qBytes[std::make_tuple(swId, port, qIndex)];
    depth += p->GetSize();
    fprintf(out, "%ld,%u,%u,%u,%ld,enq\n", Simulator::Now().GetNanoSeconds(), swId, port, qIndex, depth);
}

// QbbDequeue: fires as a packet leaves egress queue `qIndex` onto the wire.
static void OnSwitchDequeue(FILE* out, uint32_t swId, uint32_t port, Ptr<const Packet> p, uint32_t qIndex) {
    int64_t& depth = g_qBytes[std::make_tuple(swId, port, qIndex)];
    depth -= p->GetSize();
    if (depth < 0) depth = 0; // guard against control pkts (e.g. PFC) not counted on enqueue
    fprintf(out, "%ld,%u,%u,%u,%ld,deq\n", Simulator::Now().GetNanoSeconds(), swId, port, qIndex, depth);
}

// QbbDrop: fires when admission control / buffer overflow discards a packet.
static void OnSwitchDrop(FILE* out, uint32_t swId, uint32_t port, Ptr<const Packet> p, uint32_t qIndex) {
    fprintf(out, "%ld,%u,%u,%u,%u,drop\n", Simulator::Now().GetNanoSeconds(), swId, port, qIndex, p->GetSize());
}

// QbbPfc: fires on the port that RECEIVES a PFC frame (QbbNetDevice::Receive, l3Prot 0xFE),
// so type 1 = this port was PAUSED by the far end -- i.e. the *neighbour's* ingress is
// congested and this port's transmit is being throttled. type 0 = RESUME. Note the direction:
// "gpu fabric NIC pause" means the leaf paused the GPU, throttling host injection; it does
// NOT mean the GPU sent a pause. q_id and bytes are left blank so PFC rows share the drop
// event schema.
static void OnSwitchPfc(FILE* out, uint32_t swId, uint32_t port, uint32_t type) {
    fprintf(out, "%ld,%u,%u,,,%s\n", Simulator::Now().GetNanoSeconds(), swId, port, type == 1 ? "pause" : "resume");
}

// ---- per-NIC host bandwidth sampling ------------------------------------------------------
// RdmaHw::tx_bytes is a running per-port byte counter fed from QbbNetDevice's transmit path
// (RdmaHw::Setup wires m_rdmaUpdateTxBytes), so sampling it periodically costs the simulation
// nothing and gives each GPU NIC's utilization as a function of time.
//
// This is the measurement an aggregate byte count cannot make. Two NIC-selection policies can
// move identical total bytes over each NIC yet differ in when they move them: one may saturate
// plane A while plane B idles and then swap, the other may keep both half-busy throughout.
// Only a time-resolved trace separates "a NIC was left idle" from "both NICs were busy and the
// limit was elsewhere".
struct NicProbe {
    Ptr<RdmaHw> hw;
    uint32_t nodeId;
    uint32_t port;
    const char* kind;   // "nvlink" (peer is an NVSwitch) or "fabric" (peer is a leaf)
    uint64_t lastBytes;
};

static void SampleNicBw(FILE* out, std::vector<NicProbe>* probes, Time interval) {
    const int64_t now = Simulator::Now().GetNanoSeconds();
    const double secs = interval.GetSeconds();
    for (NicProbe& p : *probes) {
        uint64_t cur = p.hw->tx_bytes[p.port];
        uint64_t delta = cur - p.lastBytes;
        p.lastBytes = cur;
        // Idle NICs are emitted too, with delta 0. An omitted row would be indistinguishable
        // from a sample that never ran, and idleness is the whole point of this trace.
        fprintf(out, "%ld,%u,%u,%s,%llu,%.4f\n", now, p.nodeId, p.port, p.kind,
                (unsigned long long) delta, delta * 8.0 / secs / 1e9);
    }
    // Guard against keeping the simulation alive forever: a self-rescheduling event is always
    // pending, so Run() would never see an empty queue and never return. IsFinished() is true
    // exactly when nothing else remains -- this event has already been popped -- which in a
    // discrete-event simulator means the run really is over.
    if (!Simulator::IsFinished()) {
        Simulator::Schedule(interval, &SampleNicBw, out, probes, interval);
    }
}

int main(int argc, char *argv[]) {
    // Picoseconds, not the ns-3 default of nanoseconds. DataRate::CalculateBytesTxTime is
    // Seconds(bytes*8) / m_bps, and Time's integer division truncates at the simulator's
    // resolution -- so a link fast enough that a packet serializes in single-digit ns is
    // modelled meaningfully FAST. For the 4152-byte full-MTU wire packet here (4096 MTU +
    // 56 B header):
    //     NVLink 1800GBps: 2.3067 ns -> 2 ns  => 2076 GBps, +15.33% overspeed
    //     fabric  400Gbps: 83.040 ns -> 83 ns =>  400.2 Gbps, +0.05%
    // The NVLink figure is not hypothetical: at NS resolution the per-NIC trace reports
    // nvlink ports pinned at exactly 4152000 B / 2000 ns = 16608 Gbps, above the 14400 Gbps
    // line rate. At PS the same packet is 2306 ps, leaving 0.03% error on both link classes.
    // Must precede any topology construction; ns-3 rescales already-built Times (ConvertTimes)
    // but the call is only meaningful before the run. int64 ps still spans ~106 days.
    Time::SetResolution(Time::PS);

    NS_LOG_COMPONENT_DEFINE("DUAL_PLANE_HETERO");
//    LogComponentEnable("CollectivesApplication", LOG_INFO);
//    LogComponentEnable("SwitchNode", LOG_LEVEL_DEBUG);
    LogComponentEnable("AlgoTopo", LOG_LEVEL_WARN);

    uint32_t inputBytes = (1 << 20);
    // label distinguishes output files between runs, e.g. --label=with_rate vs --label=no_rate
    std::string label = "dual_plane_clustered";
    // Which dual_plane_clustered_<coll> XML to run. `rate` picks the rate-annotated schedule
    // vs the _no_rate ablation.
    std::string coll = "alltoall";  // allgather | alltoall
    bool rate = true;
    // Make the per-flow XML "rate" a true target (accumulating token-bucket shaper) rather than
    // just an upper bound, so a flow paced below line rate actually runs at its assigned rate.
    bool rateTargeting = true;
    bool flowId = true;              // install the per-flow switch forwarding table from the JSON
    // Baseline NIC-selection model when flowId is off, mirroring NCCL_IB_MERGE_NICS:
    //   1 -> merged virtual device: one qp per NIC per connection, every message split across
    //        them (what NCCL does by default for the two ports of one physical NIC)
    //   0 -> unmerged: one qp per connection, NICs handed out round-robin
    // With flowId on, both are bypassed -- the schedule pins each connection to one plane.
    bool mergeNics = true;
    // Period of the per-NIC bandwidth trace, in ns. 0 disables it, so every existing invocation
    // behaves exactly as before and pays nothing.
    uint32_t nicBwIntervalNs = 0;
    std::string checkLog = "minimal"; // silent | minimal | verbose
    uint32_t maxMismatches = 10;

    CommandLine cmd;
    cmd.AddValue("inputBytes", "Total input size in bytes", inputBytes);
    cmd.AddValue("label", "Suffix for the congestion-monitor output CSVs", label);
    cmd.AddValue("coll", "Collective to run: allgather | alltoall", coll);
    cmd.AddValue("rate", "Use the rate-annotated XML (false = the _no_rate ablation)", rate);
    cmd.AddValue("rateTargeting", "Treat per-flow XML rates as targets, not just caps", rateTargeting);
    cmd.AddValue("flowId", "Install per-flow switch forwarding from the switch JSON", flowId);
    cmd.AddValue("mergeNics", "Baseline only (flowId=false): 1 = NCCL-style merged NIC (one qp per NIC, message split across them), 0 = one qp per connection, round-robin NICs", mergeNics);
    cmd.AddValue("nicBwInterval", "Sample every GPU NIC's transmitted bytes this often, in ns (0 = off). Try 100 at 1MB, 2000 at 128MB.", nicBwIntervalNs);
    cmd.AddValue("checkLog", "Correctness-check logging: silent | minimal | verbose", checkLog);
    cmd.AddValue("maxMismatches", "Mismatch lines to print before giving up (minimal mode)", maxMismatches);
    cmd.Parse(argc, argv);

    if (coll != "allgather" && coll != "alltoall")
        NS_FATAL_ERROR("Unknown --coll value '" << coll << "' (expected allgather|alltoall).");

    NodeContainer gpunodes;
    NodeContainer regswtches;
    NodeContainer nvswtches;

    // PFC backpressure (CheckAndSendPfc) runs unconditionally in SwitchNode, but only
    // has an effect once QcnEnabled lets a stalled NIC's queue resume; ECN marking is
    // separately gated per-switch by the EcnEnabled attribute set below.
    Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(true));

    for (uint32_t i = 0; i < 96; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 10; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    for (uint32_t i = 0; i < 8; ++i) { nvswtches.Add(CreateObject<NVSwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper0.SetChannelAttribute("Delay", StringValue("350ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("1800GBps"));

    QbbHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper1.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("400Gbps"));

    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), nvswtches.Get(0));
    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), nvswtches.Get(0));
    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), nvswtches.Get(0));
    NetDeviceContainer devs0_3 = link_helper0.Install(gpunodes.Get(3), nvswtches.Get(0));
    NetDeviceContainer devs0_4 = link_helper0.Install(gpunodes.Get(4), nvswtches.Get(0));
    NetDeviceContainer devs0_5 = link_helper0.Install(gpunodes.Get(5), nvswtches.Get(0));
    NetDeviceContainer devs0_6 = link_helper0.Install(gpunodes.Get(6), nvswtches.Get(0));
    NetDeviceContainer devs0_7 = link_helper0.Install(gpunodes.Get(7), nvswtches.Get(0));
    NetDeviceContainer devs0_8 = link_helper0.Install(gpunodes.Get(8), nvswtches.Get(0));
    NetDeviceContainer devs0_9 = link_helper0.Install(gpunodes.Get(9), nvswtches.Get(0));
    NetDeviceContainer devs0_10 = link_helper0.Install(gpunodes.Get(10), nvswtches.Get(0));
    NetDeviceContainer devs0_11 = link_helper0.Install(gpunodes.Get(11), nvswtches.Get(0));
    NetDeviceContainer devs0_12 = link_helper0.Install(gpunodes.Get(12), nvswtches.Get(0));
    NetDeviceContainer devs0_13 = link_helper0.Install(gpunodes.Get(13), nvswtches.Get(0));
    NetDeviceContainer devs0_14 = link_helper0.Install(gpunodes.Get(14), nvswtches.Get(0));
    NetDeviceContainer devs0_15 = link_helper0.Install(gpunodes.Get(15), nvswtches.Get(0));
    NetDeviceContainer devs0_16 = link_helper0.Install(gpunodes.Get(16), nvswtches.Get(1));
    NetDeviceContainer devs0_17 = link_helper0.Install(gpunodes.Get(17), nvswtches.Get(1));
    NetDeviceContainer devs0_18 = link_helper0.Install(gpunodes.Get(18), nvswtches.Get(1));
    NetDeviceContainer devs0_19 = link_helper0.Install(gpunodes.Get(19), nvswtches.Get(1));
    NetDeviceContainer devs0_20 = link_helper0.Install(gpunodes.Get(20), nvswtches.Get(1));
    NetDeviceContainer devs0_21 = link_helper0.Install(gpunodes.Get(21), nvswtches.Get(1));
    NetDeviceContainer devs0_22 = link_helper0.Install(gpunodes.Get(22), nvswtches.Get(1));
    NetDeviceContainer devs0_23 = link_helper0.Install(gpunodes.Get(23), nvswtches.Get(1));
    NetDeviceContainer devs0_24 = link_helper0.Install(gpunodes.Get(24), nvswtches.Get(1));
    NetDeviceContainer devs0_25 = link_helper0.Install(gpunodes.Get(25), nvswtches.Get(1));
    NetDeviceContainer devs0_26 = link_helper0.Install(gpunodes.Get(26), nvswtches.Get(1));
    NetDeviceContainer devs0_27 = link_helper0.Install(gpunodes.Get(27), nvswtches.Get(1));
    NetDeviceContainer devs0_28 = link_helper0.Install(gpunodes.Get(28), nvswtches.Get(1));
    NetDeviceContainer devs0_29 = link_helper0.Install(gpunodes.Get(29), nvswtches.Get(1));
    NetDeviceContainer devs0_30 = link_helper0.Install(gpunodes.Get(30), nvswtches.Get(1));
    NetDeviceContainer devs0_31 = link_helper0.Install(gpunodes.Get(31), nvswtches.Get(1));
    NetDeviceContainer devs0_32 = link_helper0.Install(gpunodes.Get(32), nvswtches.Get(2));
    NetDeviceContainer devs0_33 = link_helper0.Install(gpunodes.Get(33), nvswtches.Get(2));
    NetDeviceContainer devs0_34 = link_helper0.Install(gpunodes.Get(34), nvswtches.Get(2));
    NetDeviceContainer devs0_35 = link_helper0.Install(gpunodes.Get(35), nvswtches.Get(2));
    NetDeviceContainer devs0_36 = link_helper0.Install(gpunodes.Get(36), nvswtches.Get(2));
    NetDeviceContainer devs0_37 = link_helper0.Install(gpunodes.Get(37), nvswtches.Get(2));
    NetDeviceContainer devs0_38 = link_helper0.Install(gpunodes.Get(38), nvswtches.Get(2));
    NetDeviceContainer devs0_39 = link_helper0.Install(gpunodes.Get(39), nvswtches.Get(2));
    NetDeviceContainer devs0_40 = link_helper0.Install(gpunodes.Get(40), nvswtches.Get(2));
    NetDeviceContainer devs0_41 = link_helper0.Install(gpunodes.Get(41), nvswtches.Get(2));
    NetDeviceContainer devs0_42 = link_helper0.Install(gpunodes.Get(42), nvswtches.Get(2));
    NetDeviceContainer devs0_43 = link_helper0.Install(gpunodes.Get(43), nvswtches.Get(2));
    NetDeviceContainer devs0_44 = link_helper0.Install(gpunodes.Get(44), nvswtches.Get(2));
    NetDeviceContainer devs0_45 = link_helper0.Install(gpunodes.Get(45), nvswtches.Get(2));
    NetDeviceContainer devs0_46 = link_helper0.Install(gpunodes.Get(46), nvswtches.Get(2));
    NetDeviceContainer devs0_47 = link_helper0.Install(gpunodes.Get(47), nvswtches.Get(2));
    NetDeviceContainer devs0_48 = link_helper0.Install(gpunodes.Get(48), nvswtches.Get(3));
    NetDeviceContainer devs0_49 = link_helper0.Install(gpunodes.Get(49), nvswtches.Get(3));
    NetDeviceContainer devs0_50 = link_helper0.Install(gpunodes.Get(50), nvswtches.Get(3));
    NetDeviceContainer devs0_51 = link_helper0.Install(gpunodes.Get(51), nvswtches.Get(3));
    NetDeviceContainer devs0_52 = link_helper0.Install(gpunodes.Get(52), nvswtches.Get(3));
    NetDeviceContainer devs0_53 = link_helper0.Install(gpunodes.Get(53), nvswtches.Get(3));
    NetDeviceContainer devs0_54 = link_helper0.Install(gpunodes.Get(54), nvswtches.Get(3));
    NetDeviceContainer devs0_55 = link_helper0.Install(gpunodes.Get(55), nvswtches.Get(3));
    NetDeviceContainer devs0_56 = link_helper0.Install(gpunodes.Get(56), nvswtches.Get(3));
    NetDeviceContainer devs0_57 = link_helper0.Install(gpunodes.Get(57), nvswtches.Get(3));
    NetDeviceContainer devs0_58 = link_helper0.Install(gpunodes.Get(58), nvswtches.Get(3));
    NetDeviceContainer devs0_59 = link_helper0.Install(gpunodes.Get(59), nvswtches.Get(3));
    NetDeviceContainer devs0_60 = link_helper0.Install(gpunodes.Get(60), nvswtches.Get(3));
    NetDeviceContainer devs0_61 = link_helper0.Install(gpunodes.Get(61), nvswtches.Get(3));
    NetDeviceContainer devs0_62 = link_helper0.Install(gpunodes.Get(62), nvswtches.Get(3));
    NetDeviceContainer devs0_63 = link_helper0.Install(gpunodes.Get(63), nvswtches.Get(3));
    NetDeviceContainer devs0_64 = link_helper0.Install(gpunodes.Get(64), nvswtches.Get(4));
    NetDeviceContainer devs0_65 = link_helper0.Install(gpunodes.Get(65), nvswtches.Get(4));
    NetDeviceContainer devs0_66 = link_helper0.Install(gpunodes.Get(66), nvswtches.Get(4));
    NetDeviceContainer devs0_67 = link_helper0.Install(gpunodes.Get(67), nvswtches.Get(4));
    NetDeviceContainer devs0_68 = link_helper0.Install(gpunodes.Get(68), nvswtches.Get(4));
    NetDeviceContainer devs0_69 = link_helper0.Install(gpunodes.Get(69), nvswtches.Get(4));
    NetDeviceContainer devs0_70 = link_helper0.Install(gpunodes.Get(70), nvswtches.Get(4));
    NetDeviceContainer devs0_71 = link_helper0.Install(gpunodes.Get(71), nvswtches.Get(4));
    NetDeviceContainer devs0_72 = link_helper0.Install(gpunodes.Get(72), nvswtches.Get(5));
    NetDeviceContainer devs0_73 = link_helper0.Install(gpunodes.Get(73), nvswtches.Get(5));
    NetDeviceContainer devs0_74 = link_helper0.Install(gpunodes.Get(74), nvswtches.Get(5));
    NetDeviceContainer devs0_75 = link_helper0.Install(gpunodes.Get(75), nvswtches.Get(5));
    NetDeviceContainer devs0_76 = link_helper0.Install(gpunodes.Get(76), nvswtches.Get(5));
    NetDeviceContainer devs0_77 = link_helper0.Install(gpunodes.Get(77), nvswtches.Get(5));
    NetDeviceContainer devs0_78 = link_helper0.Install(gpunodes.Get(78), nvswtches.Get(5));
    NetDeviceContainer devs0_79 = link_helper0.Install(gpunodes.Get(79), nvswtches.Get(5));
    NetDeviceContainer devs0_80 = link_helper0.Install(gpunodes.Get(80), nvswtches.Get(6));
    NetDeviceContainer devs0_81 = link_helper0.Install(gpunodes.Get(81), nvswtches.Get(6));
    NetDeviceContainer devs0_82 = link_helper0.Install(gpunodes.Get(82), nvswtches.Get(6));
    NetDeviceContainer devs0_83 = link_helper0.Install(gpunodes.Get(83), nvswtches.Get(6));
    NetDeviceContainer devs0_84 = link_helper0.Install(gpunodes.Get(84), nvswtches.Get(6));
    NetDeviceContainer devs0_85 = link_helper0.Install(gpunodes.Get(85), nvswtches.Get(6));
    NetDeviceContainer devs0_86 = link_helper0.Install(gpunodes.Get(86), nvswtches.Get(6));
    NetDeviceContainer devs0_87 = link_helper0.Install(gpunodes.Get(87), nvswtches.Get(6));
    NetDeviceContainer devs0_88 = link_helper0.Install(gpunodes.Get(88), nvswtches.Get(7));
    NetDeviceContainer devs0_89 = link_helper0.Install(gpunodes.Get(89), nvswtches.Get(7));
    NetDeviceContainer devs0_90 = link_helper0.Install(gpunodes.Get(90), nvswtches.Get(7));
    NetDeviceContainer devs0_91 = link_helper0.Install(gpunodes.Get(91), nvswtches.Get(7));
    NetDeviceContainer devs0_92 = link_helper0.Install(gpunodes.Get(92), nvswtches.Get(7));
    NetDeviceContainer devs0_93 = link_helper0.Install(gpunodes.Get(93), nvswtches.Get(7));
    NetDeviceContainer devs0_94 = link_helper0.Install(gpunodes.Get(94), nvswtches.Get(7));
    NetDeviceContainer devs0_95 = link_helper0.Install(gpunodes.Get(95), nvswtches.Get(7));
    NetDeviceContainer devs1_96 = link_helper1.Install(gpunodes.Get(0), regswtches.Get(0));
    NetDeviceContainer devs1_97 = link_helper1.Install(gpunodes.Get(1), regswtches.Get(0));
    NetDeviceContainer devs1_98 = link_helper1.Install(gpunodes.Get(2), regswtches.Get(0));
    NetDeviceContainer devs1_99 = link_helper1.Install(gpunodes.Get(3), regswtches.Get(0));
    NetDeviceContainer devs1_100 = link_helper1.Install(gpunodes.Get(4), regswtches.Get(0));
    NetDeviceContainer devs1_101 = link_helper1.Install(gpunodes.Get(5), regswtches.Get(0));
    NetDeviceContainer devs1_102 = link_helper1.Install(gpunodes.Get(6), regswtches.Get(0));
    NetDeviceContainer devs1_103 = link_helper1.Install(gpunodes.Get(7), regswtches.Get(0));
    NetDeviceContainer devs1_104 = link_helper1.Install(gpunodes.Get(8), regswtches.Get(0));
    NetDeviceContainer devs1_105 = link_helper1.Install(gpunodes.Get(9), regswtches.Get(0));
    NetDeviceContainer devs1_106 = link_helper1.Install(gpunodes.Get(10), regswtches.Get(0));
    NetDeviceContainer devs1_107 = link_helper1.Install(gpunodes.Get(11), regswtches.Get(0));
    NetDeviceContainer devs1_108 = link_helper1.Install(gpunodes.Get(12), regswtches.Get(0));
    NetDeviceContainer devs1_109 = link_helper1.Install(gpunodes.Get(13), regswtches.Get(0));
    NetDeviceContainer devs1_110 = link_helper1.Install(gpunodes.Get(14), regswtches.Get(0));
    NetDeviceContainer devs1_111 = link_helper1.Install(gpunodes.Get(15), regswtches.Get(0));
    NetDeviceContainer devs1_112 = link_helper1.Install(gpunodes.Get(16), regswtches.Get(0));
    NetDeviceContainer devs1_113 = link_helper1.Install(gpunodes.Get(17), regswtches.Get(0));
    NetDeviceContainer devs1_114 = link_helper1.Install(gpunodes.Get(18), regswtches.Get(0));
    NetDeviceContainer devs1_115 = link_helper1.Install(gpunodes.Get(19), regswtches.Get(0));
    NetDeviceContainer devs1_116 = link_helper1.Install(gpunodes.Get(20), regswtches.Get(0));
    NetDeviceContainer devs1_117 = link_helper1.Install(gpunodes.Get(21), regswtches.Get(0));
    NetDeviceContainer devs1_118 = link_helper1.Install(gpunodes.Get(22), regswtches.Get(0));
    NetDeviceContainer devs1_119 = link_helper1.Install(gpunodes.Get(23), regswtches.Get(0));
    NetDeviceContainer devs1_120 = link_helper1.Install(gpunodes.Get(24), regswtches.Get(0));
    NetDeviceContainer devs1_121 = link_helper1.Install(gpunodes.Get(25), regswtches.Get(0));
    NetDeviceContainer devs1_122 = link_helper1.Install(gpunodes.Get(26), regswtches.Get(0));
    NetDeviceContainer devs1_123 = link_helper1.Install(gpunodes.Get(27), regswtches.Get(0));
    NetDeviceContainer devs1_124 = link_helper1.Install(gpunodes.Get(28), regswtches.Get(0));
    NetDeviceContainer devs1_125 = link_helper1.Install(gpunodes.Get(29), regswtches.Get(0));
    NetDeviceContainer devs1_126 = link_helper1.Install(gpunodes.Get(30), regswtches.Get(0));
    NetDeviceContainer devs1_127 = link_helper1.Install(gpunodes.Get(31), regswtches.Get(0));
    NetDeviceContainer devs1_128 = link_helper1.Install(gpunodes.Get(32), regswtches.Get(1));
    NetDeviceContainer devs1_129 = link_helper1.Install(gpunodes.Get(33), regswtches.Get(1));
    NetDeviceContainer devs1_130 = link_helper1.Install(gpunodes.Get(34), regswtches.Get(1));
    NetDeviceContainer devs1_131 = link_helper1.Install(gpunodes.Get(35), regswtches.Get(1));
    NetDeviceContainer devs1_132 = link_helper1.Install(gpunodes.Get(36), regswtches.Get(1));
    NetDeviceContainer devs1_133 = link_helper1.Install(gpunodes.Get(37), regswtches.Get(1));
    NetDeviceContainer devs1_134 = link_helper1.Install(gpunodes.Get(38), regswtches.Get(1));
    NetDeviceContainer devs1_135 = link_helper1.Install(gpunodes.Get(39), regswtches.Get(1));
    NetDeviceContainer devs1_136 = link_helper1.Install(gpunodes.Get(40), regswtches.Get(1));
    NetDeviceContainer devs1_137 = link_helper1.Install(gpunodes.Get(41), regswtches.Get(1));
    NetDeviceContainer devs1_138 = link_helper1.Install(gpunodes.Get(42), regswtches.Get(1));
    NetDeviceContainer devs1_139 = link_helper1.Install(gpunodes.Get(43), regswtches.Get(1));
    NetDeviceContainer devs1_140 = link_helper1.Install(gpunodes.Get(44), regswtches.Get(1));
    NetDeviceContainer devs1_141 = link_helper1.Install(gpunodes.Get(45), regswtches.Get(1));
    NetDeviceContainer devs1_142 = link_helper1.Install(gpunodes.Get(46), regswtches.Get(1));
    NetDeviceContainer devs1_143 = link_helper1.Install(gpunodes.Get(47), regswtches.Get(1));
    NetDeviceContainer devs1_144 = link_helper1.Install(gpunodes.Get(48), regswtches.Get(1));
    NetDeviceContainer devs1_145 = link_helper1.Install(gpunodes.Get(49), regswtches.Get(1));
    NetDeviceContainer devs1_146 = link_helper1.Install(gpunodes.Get(50), regswtches.Get(1));
    NetDeviceContainer devs1_147 = link_helper1.Install(gpunodes.Get(51), regswtches.Get(1));
    NetDeviceContainer devs1_148 = link_helper1.Install(gpunodes.Get(52), regswtches.Get(1));
    NetDeviceContainer devs1_149 = link_helper1.Install(gpunodes.Get(53), regswtches.Get(1));
    NetDeviceContainer devs1_150 = link_helper1.Install(gpunodes.Get(54), regswtches.Get(1));
    NetDeviceContainer devs1_151 = link_helper1.Install(gpunodes.Get(55), regswtches.Get(1));
    NetDeviceContainer devs1_152 = link_helper1.Install(gpunodes.Get(56), regswtches.Get(1));
    NetDeviceContainer devs1_153 = link_helper1.Install(gpunodes.Get(57), regswtches.Get(1));
    NetDeviceContainer devs1_154 = link_helper1.Install(gpunodes.Get(58), regswtches.Get(1));
    NetDeviceContainer devs1_155 = link_helper1.Install(gpunodes.Get(59), regswtches.Get(1));
    NetDeviceContainer devs1_156 = link_helper1.Install(gpunodes.Get(60), regswtches.Get(1));
    NetDeviceContainer devs1_157 = link_helper1.Install(gpunodes.Get(61), regswtches.Get(1));
    NetDeviceContainer devs1_158 = link_helper1.Install(gpunodes.Get(62), regswtches.Get(1));
    NetDeviceContainer devs1_159 = link_helper1.Install(gpunodes.Get(63), regswtches.Get(1));
    NetDeviceContainer devs1_160 = link_helper1.Install(gpunodes.Get(64), regswtches.Get(2));
    NetDeviceContainer devs1_161 = link_helper1.Install(gpunodes.Get(65), regswtches.Get(2));
    NetDeviceContainer devs1_162 = link_helper1.Install(gpunodes.Get(66), regswtches.Get(2));
    NetDeviceContainer devs1_163 = link_helper1.Install(gpunodes.Get(67), regswtches.Get(2));
    NetDeviceContainer devs1_164 = link_helper1.Install(gpunodes.Get(68), regswtches.Get(2));
    NetDeviceContainer devs1_165 = link_helper1.Install(gpunodes.Get(69), regswtches.Get(2));
    NetDeviceContainer devs1_166 = link_helper1.Install(gpunodes.Get(70), regswtches.Get(2));
    NetDeviceContainer devs1_167 = link_helper1.Install(gpunodes.Get(71), regswtches.Get(2));
    NetDeviceContainer devs1_168 = link_helper1.Install(gpunodes.Get(72), regswtches.Get(2));
    NetDeviceContainer devs1_169 = link_helper1.Install(gpunodes.Get(73), regswtches.Get(2));
    NetDeviceContainer devs1_170 = link_helper1.Install(gpunodes.Get(74), regswtches.Get(2));
    NetDeviceContainer devs1_171 = link_helper1.Install(gpunodes.Get(75), regswtches.Get(2));
    NetDeviceContainer devs1_172 = link_helper1.Install(gpunodes.Get(76), regswtches.Get(2));
    NetDeviceContainer devs1_173 = link_helper1.Install(gpunodes.Get(77), regswtches.Get(2));
    NetDeviceContainer devs1_174 = link_helper1.Install(gpunodes.Get(78), regswtches.Get(2));
    NetDeviceContainer devs1_175 = link_helper1.Install(gpunodes.Get(79), regswtches.Get(2));
    NetDeviceContainer devs1_176 = link_helper1.Install(gpunodes.Get(80), regswtches.Get(2));
    NetDeviceContainer devs1_177 = link_helper1.Install(gpunodes.Get(81), regswtches.Get(2));
    NetDeviceContainer devs1_178 = link_helper1.Install(gpunodes.Get(82), regswtches.Get(2));
    NetDeviceContainer devs1_179 = link_helper1.Install(gpunodes.Get(83), regswtches.Get(2));
    NetDeviceContainer devs1_180 = link_helper1.Install(gpunodes.Get(84), regswtches.Get(2));
    NetDeviceContainer devs1_181 = link_helper1.Install(gpunodes.Get(85), regswtches.Get(2));
    NetDeviceContainer devs1_182 = link_helper1.Install(gpunodes.Get(86), regswtches.Get(2));
    NetDeviceContainer devs1_183 = link_helper1.Install(gpunodes.Get(87), regswtches.Get(2));
    NetDeviceContainer devs1_184 = link_helper1.Install(gpunodes.Get(88), regswtches.Get(2));
    NetDeviceContainer devs1_185 = link_helper1.Install(gpunodes.Get(89), regswtches.Get(2));
    NetDeviceContainer devs1_186 = link_helper1.Install(gpunodes.Get(90), regswtches.Get(2));
    NetDeviceContainer devs1_187 = link_helper1.Install(gpunodes.Get(91), regswtches.Get(2));
    NetDeviceContainer devs1_188 = link_helper1.Install(gpunodes.Get(92), regswtches.Get(2));
    NetDeviceContainer devs1_189 = link_helper1.Install(gpunodes.Get(93), regswtches.Get(2));
    NetDeviceContainer devs1_190 = link_helper1.Install(gpunodes.Get(94), regswtches.Get(2));
    NetDeviceContainer devs1_191 = link_helper1.Install(gpunodes.Get(95), regswtches.Get(2));
    NetDeviceContainer devs1_192 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_193 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_194 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_195 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_196 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_197 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_198 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_199 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_200 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_201 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_202 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_203 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_204 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_205 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_206 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_207 = link_helper1.Install(regswtches.Get(0), regswtches.Get(6));
    NetDeviceContainer devs1_208 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_209 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_210 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_211 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_212 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_213 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_214 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_215 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_216 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_217 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_218 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_219 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_220 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_221 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_222 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_223 = link_helper1.Install(regswtches.Get(0), regswtches.Get(7));
    NetDeviceContainer devs1_224 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_225 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_226 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_227 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_228 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_229 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_230 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_231 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_232 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_233 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_234 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_235 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_236 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_237 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_238 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_239 = link_helper1.Install(regswtches.Get(1), regswtches.Get(6));
    NetDeviceContainer devs1_240 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_241 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_242 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_243 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_244 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_245 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_246 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_247 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_248 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_249 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_250 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_251 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_252 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_253 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_254 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_255 = link_helper1.Install(regswtches.Get(1), regswtches.Get(7));
    NetDeviceContainer devs1_256 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_257 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_258 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_259 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_260 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_261 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_262 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_263 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_264 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_265 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_266 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_267 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_268 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_269 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_270 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_271 = link_helper1.Install(regswtches.Get(2), regswtches.Get(6));
    NetDeviceContainer devs1_272 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_273 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_274 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_275 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_276 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_277 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_278 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_279 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_280 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_281 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_282 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_283 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_284 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_285 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_286 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_287 = link_helper1.Install(regswtches.Get(2), regswtches.Get(7));
    NetDeviceContainer devs1_288 = link_helper1.Install(gpunodes.Get(0), regswtches.Get(3));
    NetDeviceContainer devs1_289 = link_helper1.Install(gpunodes.Get(1), regswtches.Get(3));
    NetDeviceContainer devs1_290 = link_helper1.Install(gpunodes.Get(2), regswtches.Get(3));
    NetDeviceContainer devs1_291 = link_helper1.Install(gpunodes.Get(3), regswtches.Get(3));
    NetDeviceContainer devs1_292 = link_helper1.Install(gpunodes.Get(4), regswtches.Get(3));
    NetDeviceContainer devs1_293 = link_helper1.Install(gpunodes.Get(5), regswtches.Get(3));
    NetDeviceContainer devs1_294 = link_helper1.Install(gpunodes.Get(6), regswtches.Get(3));
    NetDeviceContainer devs1_295 = link_helper1.Install(gpunodes.Get(7), regswtches.Get(3));
    NetDeviceContainer devs1_296 = link_helper1.Install(gpunodes.Get(8), regswtches.Get(3));
    NetDeviceContainer devs1_297 = link_helper1.Install(gpunodes.Get(9), regswtches.Get(3));
    NetDeviceContainer devs1_298 = link_helper1.Install(gpunodes.Get(10), regswtches.Get(3));
    NetDeviceContainer devs1_299 = link_helper1.Install(gpunodes.Get(11), regswtches.Get(3));
    NetDeviceContainer devs1_300 = link_helper1.Install(gpunodes.Get(12), regswtches.Get(3));
    NetDeviceContainer devs1_301 = link_helper1.Install(gpunodes.Get(13), regswtches.Get(3));
    NetDeviceContainer devs1_302 = link_helper1.Install(gpunodes.Get(14), regswtches.Get(3));
    NetDeviceContainer devs1_303 = link_helper1.Install(gpunodes.Get(15), regswtches.Get(3));
    NetDeviceContainer devs1_304 = link_helper1.Install(gpunodes.Get(16), regswtches.Get(3));
    NetDeviceContainer devs1_305 = link_helper1.Install(gpunodes.Get(17), regswtches.Get(3));
    NetDeviceContainer devs1_306 = link_helper1.Install(gpunodes.Get(18), regswtches.Get(3));
    NetDeviceContainer devs1_307 = link_helper1.Install(gpunodes.Get(19), regswtches.Get(3));
    NetDeviceContainer devs1_308 = link_helper1.Install(gpunodes.Get(20), regswtches.Get(3));
    NetDeviceContainer devs1_309 = link_helper1.Install(gpunodes.Get(21), regswtches.Get(3));
    NetDeviceContainer devs1_310 = link_helper1.Install(gpunodes.Get(22), regswtches.Get(3));
    NetDeviceContainer devs1_311 = link_helper1.Install(gpunodes.Get(23), regswtches.Get(3));
    NetDeviceContainer devs1_312 = link_helper1.Install(gpunodes.Get(24), regswtches.Get(3));
    NetDeviceContainer devs1_313 = link_helper1.Install(gpunodes.Get(25), regswtches.Get(3));
    NetDeviceContainer devs1_314 = link_helper1.Install(gpunodes.Get(26), regswtches.Get(3));
    NetDeviceContainer devs1_315 = link_helper1.Install(gpunodes.Get(27), regswtches.Get(3));
    NetDeviceContainer devs1_316 = link_helper1.Install(gpunodes.Get(28), regswtches.Get(3));
    NetDeviceContainer devs1_317 = link_helper1.Install(gpunodes.Get(29), regswtches.Get(3));
    NetDeviceContainer devs1_318 = link_helper1.Install(gpunodes.Get(30), regswtches.Get(3));
    NetDeviceContainer devs1_319 = link_helper1.Install(gpunodes.Get(31), regswtches.Get(3));
    NetDeviceContainer devs1_320 = link_helper1.Install(gpunodes.Get(32), regswtches.Get(4));
    NetDeviceContainer devs1_321 = link_helper1.Install(gpunodes.Get(33), regswtches.Get(4));
    NetDeviceContainer devs1_322 = link_helper1.Install(gpunodes.Get(34), regswtches.Get(4));
    NetDeviceContainer devs1_323 = link_helper1.Install(gpunodes.Get(35), regswtches.Get(4));
    NetDeviceContainer devs1_324 = link_helper1.Install(gpunodes.Get(36), regswtches.Get(4));
    NetDeviceContainer devs1_325 = link_helper1.Install(gpunodes.Get(37), regswtches.Get(4));
    NetDeviceContainer devs1_326 = link_helper1.Install(gpunodes.Get(38), regswtches.Get(4));
    NetDeviceContainer devs1_327 = link_helper1.Install(gpunodes.Get(39), regswtches.Get(4));
    NetDeviceContainer devs1_328 = link_helper1.Install(gpunodes.Get(40), regswtches.Get(4));
    NetDeviceContainer devs1_329 = link_helper1.Install(gpunodes.Get(41), regswtches.Get(4));
    NetDeviceContainer devs1_330 = link_helper1.Install(gpunodes.Get(42), regswtches.Get(4));
    NetDeviceContainer devs1_331 = link_helper1.Install(gpunodes.Get(43), regswtches.Get(4));
    NetDeviceContainer devs1_332 = link_helper1.Install(gpunodes.Get(44), regswtches.Get(4));
    NetDeviceContainer devs1_333 = link_helper1.Install(gpunodes.Get(45), regswtches.Get(4));
    NetDeviceContainer devs1_334 = link_helper1.Install(gpunodes.Get(46), regswtches.Get(4));
    NetDeviceContainer devs1_335 = link_helper1.Install(gpunodes.Get(47), regswtches.Get(4));
    NetDeviceContainer devs1_336 = link_helper1.Install(gpunodes.Get(48), regswtches.Get(4));
    NetDeviceContainer devs1_337 = link_helper1.Install(gpunodes.Get(49), regswtches.Get(4));
    NetDeviceContainer devs1_338 = link_helper1.Install(gpunodes.Get(50), regswtches.Get(4));
    NetDeviceContainer devs1_339 = link_helper1.Install(gpunodes.Get(51), regswtches.Get(4));
    NetDeviceContainer devs1_340 = link_helper1.Install(gpunodes.Get(52), regswtches.Get(4));
    NetDeviceContainer devs1_341 = link_helper1.Install(gpunodes.Get(53), regswtches.Get(4));
    NetDeviceContainer devs1_342 = link_helper1.Install(gpunodes.Get(54), regswtches.Get(4));
    NetDeviceContainer devs1_343 = link_helper1.Install(gpunodes.Get(55), regswtches.Get(4));
    NetDeviceContainer devs1_344 = link_helper1.Install(gpunodes.Get(56), regswtches.Get(4));
    NetDeviceContainer devs1_345 = link_helper1.Install(gpunodes.Get(57), regswtches.Get(4));
    NetDeviceContainer devs1_346 = link_helper1.Install(gpunodes.Get(58), regswtches.Get(4));
    NetDeviceContainer devs1_347 = link_helper1.Install(gpunodes.Get(59), regswtches.Get(4));
    NetDeviceContainer devs1_348 = link_helper1.Install(gpunodes.Get(60), regswtches.Get(4));
    NetDeviceContainer devs1_349 = link_helper1.Install(gpunodes.Get(61), regswtches.Get(4));
    NetDeviceContainer devs1_350 = link_helper1.Install(gpunodes.Get(62), regswtches.Get(4));
    NetDeviceContainer devs1_351 = link_helper1.Install(gpunodes.Get(63), regswtches.Get(4));
    NetDeviceContainer devs1_352 = link_helper1.Install(gpunodes.Get(64), regswtches.Get(5));
    NetDeviceContainer devs1_353 = link_helper1.Install(gpunodes.Get(65), regswtches.Get(5));
    NetDeviceContainer devs1_354 = link_helper1.Install(gpunodes.Get(66), regswtches.Get(5));
    NetDeviceContainer devs1_355 = link_helper1.Install(gpunodes.Get(67), regswtches.Get(5));
    NetDeviceContainer devs1_356 = link_helper1.Install(gpunodes.Get(68), regswtches.Get(5));
    NetDeviceContainer devs1_357 = link_helper1.Install(gpunodes.Get(69), regswtches.Get(5));
    NetDeviceContainer devs1_358 = link_helper1.Install(gpunodes.Get(70), regswtches.Get(5));
    NetDeviceContainer devs1_359 = link_helper1.Install(gpunodes.Get(71), regswtches.Get(5));
    NetDeviceContainer devs1_360 = link_helper1.Install(gpunodes.Get(72), regswtches.Get(5));
    NetDeviceContainer devs1_361 = link_helper1.Install(gpunodes.Get(73), regswtches.Get(5));
    NetDeviceContainer devs1_362 = link_helper1.Install(gpunodes.Get(74), regswtches.Get(5));
    NetDeviceContainer devs1_363 = link_helper1.Install(gpunodes.Get(75), regswtches.Get(5));
    NetDeviceContainer devs1_364 = link_helper1.Install(gpunodes.Get(76), regswtches.Get(5));
    NetDeviceContainer devs1_365 = link_helper1.Install(gpunodes.Get(77), regswtches.Get(5));
    NetDeviceContainer devs1_366 = link_helper1.Install(gpunodes.Get(78), regswtches.Get(5));
    NetDeviceContainer devs1_367 = link_helper1.Install(gpunodes.Get(79), regswtches.Get(5));
    NetDeviceContainer devs1_368 = link_helper1.Install(gpunodes.Get(80), regswtches.Get(5));
    NetDeviceContainer devs1_369 = link_helper1.Install(gpunodes.Get(81), regswtches.Get(5));
    NetDeviceContainer devs1_370 = link_helper1.Install(gpunodes.Get(82), regswtches.Get(5));
    NetDeviceContainer devs1_371 = link_helper1.Install(gpunodes.Get(83), regswtches.Get(5));
    NetDeviceContainer devs1_372 = link_helper1.Install(gpunodes.Get(84), regswtches.Get(5));
    NetDeviceContainer devs1_373 = link_helper1.Install(gpunodes.Get(85), regswtches.Get(5));
    NetDeviceContainer devs1_374 = link_helper1.Install(gpunodes.Get(86), regswtches.Get(5));
    NetDeviceContainer devs1_375 = link_helper1.Install(gpunodes.Get(87), regswtches.Get(5));
    NetDeviceContainer devs1_376 = link_helper1.Install(gpunodes.Get(88), regswtches.Get(5));
    NetDeviceContainer devs1_377 = link_helper1.Install(gpunodes.Get(89), regswtches.Get(5));
    NetDeviceContainer devs1_378 = link_helper1.Install(gpunodes.Get(90), regswtches.Get(5));
    NetDeviceContainer devs1_379 = link_helper1.Install(gpunodes.Get(91), regswtches.Get(5));
    NetDeviceContainer devs1_380 = link_helper1.Install(gpunodes.Get(92), regswtches.Get(5));
    NetDeviceContainer devs1_381 = link_helper1.Install(gpunodes.Get(93), regswtches.Get(5));
    NetDeviceContainer devs1_382 = link_helper1.Install(gpunodes.Get(94), regswtches.Get(5));
    NetDeviceContainer devs1_383 = link_helper1.Install(gpunodes.Get(95), regswtches.Get(5));
    NetDeviceContainer devs1_384 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_385 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_386 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_387 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_388 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_389 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_390 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_391 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_392 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_393 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_394 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_395 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_396 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_397 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_398 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_399 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_400 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_401 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_402 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_403 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_404 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_405 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_406 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_407 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_408 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_409 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_410 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_411 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_412 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_413 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_414 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_415 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_416 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_417 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_418 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_419 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_420 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_421 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_422 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_423 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_424 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_425 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_426 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_427 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_428 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_429 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_430 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_431 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_432 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_433 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_434 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_435 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_436 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_437 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_438 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_439 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_440 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_441 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_442 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_443 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_444 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_445 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_446 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_447 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_448 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_449 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_450 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_451 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_452 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_453 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_454 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_455 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_456 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_457 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_458 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_459 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_460 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_461 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_462 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_463 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_464 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_465 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_466 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_467 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_468 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_469 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_470 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_471 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_472 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_473 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_474 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_475 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_476 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_477 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_478 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_479 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    Config::SetDefault("ns3::RdmaHw::CcMode", UintegerValue(12));
    Config::SetDefault("ns3::RdmaHw::RateTargeting", BooleanValue(rateTargeting));
    Config::SetDefault("ns3::RdmaHw::L2AckInterval", UintegerValue(0));
    Config::SetDefault("ns3::RdmaHw::L2ChunkSize", UintegerValue(4000));
    Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(4096));

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);

    // Algorithm + per-flow forwarding table. The switch JSON's switch_id_map (0..9 -> TE-CCL
    // ids) matches the regswtches declaration order above; there is one JSON per collective,
    // shared by the rate and _no_rate XMLs since routing is identical.
    const std::string XML_NAME = "dual_plane_clustered_" + coll + (rate ? "" : "_no_rate") + ".xml";
    std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(),
                                                  "../../scratch/xml_input/" + XML_NAME);
    std::string SWITCH_JSON = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(),
                                                      "../../scratch/json_input/dual_plane_clustered_" + coll + ".json");

    // All output files go to simulation/scratch/logs. FindSelfDirectory() resolves to
    // simulation/build/scratch, so "../../scratch/logs" hops back up to the source tree.
    const std::string LOG_DIR = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/logs");
    ns3::SystemPath::MakeDirectories(LOG_DIR); // no-op if it already exists

    const std::string LOG_FILE = ns3::SystemPath::Append(LOG_DIR, label + ".txt");

    constexpr DataType::Type dtype = DataType::INT32;
    const uint32_t INPUT_BYTES = inputBytes;
    bool CORRECTNESS_CHECK = true;

    AlgoTopology topo(gpunodes, regswtches);
    AlgoParseResult result = topo.ParseAlgoXml(XML_ALGO.c_str());
    // Fatal, not a log line: a failed parse leaves the topology empty and the failure would
    // otherwise surface far downstream (zero input chunks) with NS_LOG_ERROR off by default.
    if (result != AlgoParseResult::ALGO_PARSE_SUCCESS)
        NS_FATAL_ERROR("Encountered issue in parsing XML algorithm " << XML_ALGO << ", error code " << result);
    if (flowId) {
        AlgoParseResult switchResult = topo.ParseSwitchJson(SWITCH_JSON.c_str());
        // Fatal for the same reason the XML parse above is: ParseSwitchJson returns on the
        // first bad entry, leaving flow forwarding half-installed (CustomFlowForwarding on,
        // table mostly empty), and the run then silently falls back to ECMP everywhere.
        if (switchResult != AlgoParseResult::ALGO_PARSE_SUCCESS)
            NS_FATAL_ERROR("Encountered issue in parsing switch JSON " << SWITCH_JSON << ", error code " << switchResult);
    }

    static std::ofstream logtxt;

    // log file
    logtxt.open(LOG_FILE);
    if (!logtxt.is_open()){
        NS_FATAL_ERROR("Failed to log file");
    }
    chmod(LOG_FILE.c_str(), 0666);

    // Chunk count and participant set come straight from the parsed algorithm, so ChunkSize
    // and the tester can never drift from the XML, and swapping XMLs needs no source edit here.
    const int N_CHUNKS = topo.GetNInputChunks();
    const int N_NODES = (int) topo.GetActiveGpuIds().size();
    NS_ASSERT_MSG(N_CHUNKS > 0, "Parsed algorithm reports zero input chunks; check the XML.");
    const int CHUNK_SIZE = (INPUT_BYTES / N_CHUNKS) / DataType::GetSizeBytes(dtype);

    // install apps
    CollectivesApplicationHelper app_helper;
    app_helper.SetAttribute("DataType", EnumValue(dtype));
    app_helper.SetAttribute("ChunkSize", UintegerValue(CHUNK_SIZE));
    app_helper.SetAttribute("CorrectnessCheck", BooleanValue(CORRECTNESS_CHECK));
    // The schedule's per-connection plane pin and NIC merging are mutually exclusive: merging
    // deliberately spans every plane at once. GetRdmaLaneCount also refuses to merge a pinned
    // connection, so this is belt-and-braces -- but it keeps the reported config honest.
    app_helper.SetAttribute("MergeNics", BooleanValue(!flowId && mergeNics));
    ApplicationContainer apps = app_helper.Install<GPU>(topo);

    NS_LOG_INFO("Finished installing collective apps.");

    // The ctor's `verbose` flag only seeds the log mode; SetLogMode below is what governs.
    CollectiveTester tester(apps, false, logtxt);
    CollectiveLogMode logMode = CollectiveLogMode::MINIMAL;
    if (checkLog == "silent") logMode = CollectiveLogMode::SILENT;
    else if (checkLog == "verbose") logMode = CollectiveLogMode::VERBOSE;
    else if (checkLog != "minimal") NS_FATAL_ERROR("Unknown --checkLog value '" << checkLog << "' (expected silent|minimal|verbose).");
    tester.SetLogMode(logMode);
    tester.SetMaxMismatches(maxMismatches);
    if (CORRECTNESS_CHECK) {
        if (coll == "allgather") tester.SetupAllgather(topo, CHUNK_SIZE * N_CHUNKS);
        else                     tester.SetupAlltoall(topo, CHUNK_SIZE * N_CHUNKS);
    }
    else{
        NS_LOG_UNCOND("Skipping correctness check.");
    }

    // ---- congestion monitoring: event-driven switch egress queue / drop / PFC traces ----
    // Connect to the QbbNetDevice trace sources on every switch egress port. These fire
    // synchronously from within packet events, so they add no simulator events and leave the
    // reported latency/bandwidth (Simulator::Now()) untouched. Each qlen row is emitted on an
    // actual enqueue/dequeue, giving an exact, unsampled occupancy trace.
    std::string qlenPath = ns3::SystemPath::Append(LOG_DIR, "switch_qlen_" + label + ".csv");
    std::string eventPath = ns3::SystemPath::Append(LOG_DIR, "switch_events_" + label + ".csv");
    FILE* qlenOut = fopen(qlenPath.c_str(), "w");
    FILE* eventOut = fopen(eventPath.c_str(), "w");
    if (!qlenOut || !eventOut) NS_FATAL_ERROR("Failed to open congestion-monitor output files.");
    fprintf(qlenOut, "time_ns,sw_id,port_id,q_id,qlen_bytes,op\n");
    // node_id: GPUs are the first 96 ids, then the 10 regswtches (leaves/spines); drops carry a
    // size, PFC pause/resume leave bytes/q_id blank.
    fprintf(eventOut, "time_ns,node_id,port_id,q_id,bytes,op\n");

    for (uint32_t s = 0; s < regswtches.GetN(); ++s) {
        Ptr<Node> sw = regswtches.Get(s);
        uint32_t swId = sw->GetId();
        for (uint32_t d = 0; d < sw->GetNDevices(); ++d) {
            Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(sw->GetDevice(d));
            if (!dev) continue; // skip any non-Qbb (e.g. loopback) device
            uint32_t port = dev->GetIfIndex();
            dev->TraceConnectWithoutContext("QbbEnqueue", MakeBoundCallback(&OnSwitchEnqueue, qlenOut, swId, port));
            dev->TraceConnectWithoutContext("QbbDequeue", MakeBoundCallback(&OnSwitchDequeue, qlenOut, swId, port));
            dev->TraceConnectWithoutContext("QbbDrop",    MakeBoundCallback(&OnSwitchDrop, eventOut, swId, port));
            dev->TraceConnectWithoutContext("QbbPfc",     MakeBoundCallback(&OnSwitchPfc, eventOut, swId, port));
        }
    }

    // The QbbPfc trace fires on the device that RECEIVES a PAUSE, and a switch backpressures a
    // congested ingress link by pausing the sender on the far end -- which for leaf <-> GPU links
    // is a host NIC, not a switch. Connect the drop/PFC traces on the GPU NICs too. Queue-occupancy
    // (enqueue/dequeue) stays switch-only, since host egress is just the GPU injecting and isn't
    // the congestion of interest.
    for (uint32_t g = 0; g < gpunodes.GetN(); ++g) {
        Ptr<Node> gpu = gpunodes.Get(g);
        uint32_t gpuId = gpu->GetId();
        for (uint32_t d = 0; d < gpu->GetNDevices(); ++d) {
            Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(gpu->GetDevice(d));
            if (!dev) continue;
            uint32_t port = dev->GetIfIndex();
            dev->TraceConnectWithoutContext("QbbDrop", MakeBoundCallback(&OnSwitchDrop, eventOut, gpuId, port));
            dev->TraceConnectWithoutContext("QbbPfc",  MakeBoundCallback(&OnSwitchPfc, eventOut, gpuId, port));
        }
    }

    // ---- per-NIC host bandwidth trace (opt-in via --nicBwInterval) ----
    // Probes are built after RdmaFabricHelper::Build, which is what creates each GPU's
    // RdmaDriver/RdmaHw and sizes RdmaHw::tx_bytes to the node's device count.
    static std::vector<NicProbe> nicProbes;
    FILE* nicOut = nullptr;
    std::string nicPath;
    if (nicBwIntervalNs > 0) {
        nicPath = ns3::SystemPath::Append(LOG_DIR, "host_nic_bw_" + label + ".csv");
        nicOut = fopen(nicPath.c_str(), "w");
        if (!nicOut) NS_FATAL_ERROR("Failed to open the per-NIC bandwidth output file.");
        fprintf(nicOut, "time_ns,node_id,port_id,kind,bytes,gbps\n");
        for (uint32_t g = 0; g < gpunodes.GetN(); ++g) {
            Ptr<Node> gpu = gpunodes.Get(g);
            Ptr<RdmaDriver> drv = gpu->GetObject<RdmaDriver>();
            if (!drv) continue;
            for (uint32_t d = 0; d < gpu->GetNDevices(); ++d) {
                Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(gpu->GetDevice(d));
                if (!dev) continue;
                // Classify by what the link actually reaches rather than by device index, so
                // the trace stays correct if the topology's Install order ever changes.
                Ptr<QbbChannel> ch = DynamicCast<QbbChannel>(dev->GetChannel());
                if (!ch) continue;
                Ptr<NetDevice> other = (ch->GetDevice(0) == dev) ? ch->GetDevice(1) : ch->GetDevice(0);
                const char* kind = DynamicCast<NVSwitchNode>(other->GetNode()) ? "nvlink" : "fabric";
                nicProbes.push_back(NicProbe{drv->m_rdma, gpu->GetId(), dev->GetIfIndex(), kind, 0});
            }
        }
        Time iv = NanoSeconds(nicBwIntervalNs);
        Simulator::Schedule(iv, &SampleNicBw, nicOut, &nicProbes, iv);
        std::cout << "Per-NIC bandwidth trace: " << nicPath
                  << " (every " << nicBwIntervalNs << " ns, " << nicProbes.size() << " NICs)" << std::endl;
    }

    Simulator::Run();
    fclose(qlenOut);
    fclose(eventOut);
    if (nicOut) fclose(nicOut);
    std::cout << "NIC selection: " << (flowId ? "schedule-pinned (one qp per connection)"
        : (mergeNics ? "merged NIC (one qp per NIC, message split across them)"
                     : "round-robin (one qp per connection)")) << std::endl;
    std::cout << "Algorithm XML: " << XML_ALGO << std::endl;
    std::cout << "Switch queue trace: " << qlenPath << std::endl;
    std::cout << "Switch drop/PFC trace: " << eventPath << std::endl;
    Time simTime = Simulator::Now();
    std::cout << "Total simulated time: "
        << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;

    // How much of the traffic the switch JSON actually steered. A miss means a flow-id-carrying
    // packet reached a switch holding no rule for it and fell back to ECMP, i.e. the schedule
    // was not in force for that packet. With the sender's NIC pinned to the plane the schedule
    // chose, this should be ~0; a large miss share means the injection side and the routing
    // side disagree and the --flowId comparison is not measuring what it claims to.
    if (flowId) {
        uint64_t hits = 0, misses = 0;
        for (uint32_t s = 0; s < regswtches.GetN(); ++s) {
            Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(regswtches.Get(s));
            if (!sw) continue;
            hits += sw->GetFlowRuleHits();
            misses += sw->GetFlowRuleMisses();
        }
        const uint64_t total = hits + misses;
        std::cout << "Flow-forwarding rule coverage: " << hits << " hit / " << misses << " miss";
        if (total) std::cout << " (" << (100.0 * hits / total) << "% of flow-id packets steered by the schedule)";
        std::cout << std::endl;
    }

    // algorithm bandwidth: total data moved per rank / time
    std::cout << coll << " algorithm bandwidth: "
        << (double) INPUT_BYTES * N_NODES / simTime.GetSeconds() / 1e9 << " GB/s" << std::endl;
    if (CORRECTNESS_CHECK) {
        CollectiveTestResult res = (coll == "allgather")
            ? tester.VerifyAllgather(topo, CHUNK_SIZE * N_CHUNKS)
            : tester.VerifyAlltoall(topo, CHUNK_SIZE * N_CHUNKS);
        if (res == CollectiveTestResult::TEST_OK) std::cout << coll << " verified." << std::endl;
        else std::cout << coll << " incorrect." << std::endl;
    }

    Simulator::Destroy();
    NS_LOG_UNCOND("Done simulation");
    return 0;
}
