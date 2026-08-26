// Rail-optimized SINGLE-PLANE 256-GPU spine-leaf fabric, non-blocking.
// 32 nodes x 8 GPUs = 256 GPUs behind 32 NVSwitches, fanning out to 12 regular switches:
// 8 leaves + 4 spines. Rail-optimized: leaf l attaches to GPU l of every node, so each of a
// node's 8 GPUs lands on a distinct leaf (rail) over one 400Gbps port.
//
// regswtches declaration order (this is also the switch JSON's switch_id_map order):
//   0..7 = leaf0..leaf7,  8..11 = spine0..spine3
//
// Non-blocking: a leaf has 32 downlinks x 400Gbps = 12800Gbps and 4 x 3200Gbps = 12800Gbps
// of uplink. Each leaf-spine edge is cabled as 8 parallel 400Gbps links rather than one fat
// 3200Gbps link, so the schedule can pin a flow to a single cable instead of letting it
// spread across the whole bundle.
//
// Generated from topology/dsl-frontend/examples/rail_optimized_256gpu.topo via
// `python3 topology/main.py <that file>`; the harness around the topology (algorithm parse,
// correctness check, congestion monitoring) follows dual_plane_hetero.cc. Regenerate the
// topology body from the DSL rather than editing the link list here by hand.
//
// Inputs follow the "rail_optimized_256gpu_<coll>[_no_rate].xml" /
// "rail_optimized_256gpu_<coll>.json" naming.

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

// High-water mark of the same occupancy, kept per port unconditionally. It costs one compare
// per packet and is what makes the peak queue readable without the full per-packet trace: at
// the top of an input sweep the row-by-row CSV runs to hundreds of GB, so --qlenRows=0 turns
// the rows off and leaves only this summary.
static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int64_t> g_qMax;
static bool g_qlenRows = true;

// QbbEnqueue: fires just before a packet is pushed onto egress queue `qIndex`.
static void OnSwitchEnqueue(FILE* out, uint32_t swId, uint32_t port, Ptr<const Packet> p, uint32_t qIndex) {
    const auto key = std::make_tuple(swId, port, qIndex);
    int64_t& depth = g_qBytes[key];
    depth += p->GetSize();
    int64_t& peak = g_qMax[key];
    if (depth > peak) peak = depth;
    if (g_qlenRows)
        fprintf(out, "%ld,%u,%u,%u,%ld,enq\n", Simulator::Now().GetNanoSeconds(), swId, port, qIndex, depth);
}

// QbbDequeue: fires as a packet leaves egress queue `qIndex` onto the wire.
static void OnSwitchDequeue(FILE* out, uint32_t swId, uint32_t port, Ptr<const Packet> p, uint32_t qIndex) {
    int64_t& depth = g_qBytes[std::make_tuple(swId, port, qIndex)];
    depth -= p->GetSize();
    if (depth < 0) depth = 0; // guard against control pkts (e.g. PFC) not counted on enqueue
    if (g_qlenRows)
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
    // KNOWN MODEL LIMITATION -- deliberately left in place; see the measurement below before
    // "fixing" it. DataRate::CalculateBytesTxTime is Seconds(bytes*8) / m_bps, and Time's
    // integer division truncates at the simulator's resolution (ns-3 default: nanoseconds).
    // A link fast enough that a packet serializes in single-digit ns is therefore modelled
    // FAST. For the 4152-byte full-MTU wire packet here (4096 MTU + 56 B header):
    //     NVLink 1800GBps: 2.3067 ns -> 2 ns  => 2076 GBps, +15.33% overspeed
    //     fabric  400Gbps: 83.040 ns -> 83 ns =>  400.2 Gbps, +0.05%
    // Not hypothetical: the per-NIC trace reports nvlink ports pinned at exactly
    // 4152000 B / 2000 ns = 16608 Gbps, above the 14400 Gbps line rate. It is why
    // check_congestion.py's "NVLink aggregate peak" can exceed 96 x 14.4 Tbps -- do not read
    // that number as a real utilization.
    //
    // Why it is tolerated for the fabric-bound collectives this scratch runs: on the 128MB
    // alltoall the busiest GPU needs 44.7 us of NVLink time against 1247.2 us of fabric time
    // (3.6%), the overspeed understates NVLink by at most 5.9 us per GPU, and the per-NIC
    // trace shows 0 of 96 fabric NICs idling before they finish -- so NVLink is never on the
    // critical path and the wall-clock error is ~0 (0.47% even assuming full serialization).
    //
    // Time::SetResolution(Time::PS) corrects it (fabric becomes exact at 83040 ps, NVLink
    // drops to +0.03%) but costs a large amount of WALL-CLOCK run time, because finer ticks
    // defeat event coalescing in the rate pacer: QbbNetDevice gates on
    // `qp->m_nextAvail > Simulator::Now()` (DequeueAndTransmit) and the same comparison in
    // GetNextQindex. At NS a sub-ns pacing deadline truncates to exactly Now(), the qp counts
    // as available and transmits inline; at PS it sits 1-999 ps in the future, so each packet
    // instead costs a cancelled/rescheduled DequeueAndTransmit wakeup. RateTargeting (on by
    // default) pushes m_nextAvail into the future deliberately, so this hits nearly every
    // packet. Enable PS only for an NVLink-BOUND workload (allreduce, small messages, or
    // anything where NVLink demand approaches fabric demand), where the accuracy is worth the
    // slowdown. It must precede any topology construction.

    NS_LOG_COMPONENT_DEFINE("RAIL_OPT_256GPU");
//    LogComponentEnable("CollectivesApplication", LOG_INFO);
//    LogComponentEnable("SwitchNode", LOG_LEVEL_DEBUG);
    LogComponentEnable("AlgoTopo", LOG_LEVEL_WARN);

    uint64_t inputBytes = (1ull << 20);
    // label distinguishes output files between runs, e.g. --label=with_rate vs --label=no_rate
    std::string label = "rail_optimized_256gpu";
    // Which rail_optimized_256gpu_<coll> XML to run. `rate` picks the rate-annotated schedule
    // vs the _no_rate ablation.
    std::string coll = "alltoall";  // allgather | alltoall
    bool rate = true;
    // Make the per-flow XML "rate" a true target (accumulating token-bucket shaper) rather than
    // just an upper bound, so a flow paced below line rate actually runs at its assigned rate.
    bool rateTargeting = true;
    // Honor the XML netdepid/netdeps wire-ordering dependences. These are what pace a
    // time-indexed (TE-CCL) solve; off releases every buffer-ready send at once. Safe to ablate
    // -- buffer readiness is still enforced by depid/deps.
    bool netDeps = true;
    // Network-side only: put the schedule's flow id on the wire and install the per-flow
    // forwarding table from the JSON, so switches route by it instead of hashing ECMP. When
    // false the header is not merely ignored, it is never added, so neither arm carries its 4
    // bytes per packet. Deliberately says nothing about which NIC a connection uses -- that is
    // --nicSel, and the two are now independent knobs.
    bool flowId = true;
    // How each connection picks its NIC, independent of --flowId:
    //   schedule -> the NIC the switch JSON dictates (one qp per connection, pinned to a plane)
    //   merged   -> NCCL_IB_MERGE_NICS: one qp per NIC, every message split across them
    //   rr       -> one qp per connection, NICs handed out round-robin
    std::string nicSel = "schedule";
    // Overrides the derived XML filename (not the path) so an alternative schedule for the
    // same collective can be run without touching this file: --coll stays "alltoall", which
    // is what the tester and the switch JSON name are keyed on, while the algorithm itself is
    // swapped. Empty means "derive it from --coll and --rate", the original behaviour.
    std::string xmlName = "";
    // Period of the per-NIC bandwidth trace, in ns. 0 disables it, so every existing invocation
    // behaves exactly as before and pays nothing.
    uint32_t nicBwIntervalNs = 0;
    // The per-packet queue trace is exact but grows with the traffic: one row per enqueue and
    // one per dequeue at every hop. Fine at 1 MB, hundreds of GB at 96 GB of input -- so it can
    // be turned off, leaving switch_qlen_max_<label>.csv (the per-port high-water marks, which
    // are tracked either way).
    bool qlenRows = true;
    std::string checkLog = "minimal"; // silent | minimal | verbose
    uint32_t maxMismatches = 10;
    // Algorithm pipelining granularity (the MSCCL kernel's gridOffset loop); 0 disables it.
    // Note this is compared against the size of ONE chunk, which here is inputBytes/384 -- so
    // at the default 1 MB input a chunk is small and any sane value leaves both the gridOffset
    // and maxAllowedCount loops inert. To exercise them, either raise inputBytes until a chunk
    // approaches this, or set this below the chunk size.
    uint32_t protoChunkBytes = 0;
    // Transport pipelining depth (NCCL_STEPS analogue): how many messages a qp may have in
    // flight before waiting for a completion. It binds only when --l2Ack is nonzero: with acks
    // off the sender self-acknowledges at send completion and messages retire without a round
    // trip, so no depth of in-flight messages is ever reached.
    uint32_t maxMsgsInFlight = 8;
    // Receiver ack cadence, in bytes, for RdmaHw::L2AckInterval; 0 is no-ack mode, where the
    // sender infers completion from its own send completion and nothing waits a round trip.
    // Not a cosmetic knob: it changes the transport under every other setting here, so runs
    // that differ in it are not comparable with each other.
    uint32_t l2AckInterval = 1;

    CommandLine cmd;
    cmd.AddValue("inputBytes", "Total input size in bytes", inputBytes);
    cmd.AddValue("label", "Suffix for the congestion-monitor output CSVs", label);
    cmd.AddValue("coll", "Collective to run: allgather | alltoall", coll);
    cmd.AddValue("rate", "Use the rate-annotated XML (false = the _no_rate ablation)", rate);
    cmd.AddValue("rateTargeting", "Treat per-flow XML rates as targets, not just caps", rateTargeting);
    cmd.AddValue("flowId", "Network only: carry msccl flow ids and install per-flow switch forwarding from the JSON (does not affect NIC selection)", flowId);
    cmd.AddValue("xml", "XML schedule filename inside scratch/xml_input, overriding the one derived from --coll/--rate (empty = derive)", xmlName);
    cmd.AddValue("nicSel", "NIC selection: schedule (switch JSON pins the NIC) | merged (NCCL-style merged NIC, one qp per NIC) | rr (one qp per connection, round-robin NICs)", nicSel);
    cmd.AddValue("netDeps", "Honor the XML netdepid/netdeps network dependences (false = release every buffer-ready send immediately)", netDeps);
    cmd.AddValue("qlenRows", "Write the per-packet switch queue trace (0 = only the per-port peak summary, which is all a large sweep can afford on disk)", qlenRows);
    cmd.AddValue("nicBwInterval", "Sample every GPU NIC's transmitted bytes this often, in ns (0 = off). Try 100 at 1MB, 2000 at 128MB.", nicBwIntervalNs);
    cmd.AddValue("checkLog", "Correctness-check logging: silent | minimal | verbose", checkLog);
    cmd.AddValue("maxMismatches", "Mismatch lines to print before giving up (minimal mode)", maxMismatches);
    cmd.AddValue("protoChunkBytes", "Pipelining granularity in bytes; 0 disables pipelining", protoChunkBytes);
    cmd.AddValue("maxMsgsInFlight", "Messages a qp may have in flight at once", maxMsgsInFlight);
    cmd.AddValue("l2Ack", "Receiver ack interval in bytes (0 = no-ack mode, sender self-acknowledges at send completion)", l2AckInterval);
    cmd.Parse(argc, argv);

    g_qlenRows = qlenRows;

    if (nicSel != "schedule" && nicSel != "merged" && nicSel != "rr")
        NS_FATAL_ERROR("Unknown --nicSel value '" << nicSel << "' (expected schedule|merged|rr).");
    if (coll != "allgather" && coll != "alltoall")
        NS_FATAL_ERROR("Unknown --coll value '" << coll << "' (expected allgather|alltoall).");

    NodeContainer gpunodes;
    NodeContainer regswtches;
    NodeContainer nvswtches;

    // PFC backpressure (CheckAndSendPfc) runs unconditionally in SwitchNode, but only
    // has an effect once QcnEnabled lets a stalled NIC's queue resume; ECN marking is
    // separately gated per-switch by the EcnEnabled attribute set below.
    Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(true));

    for (uint32_t i = 0; i < 256; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 12; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    for (uint32_t i = 0; i < 32; ++i) { nvswtches.Add(CreateObject<NVSwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper0.SetChannelAttribute("Delay", StringValue("700ns"));
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
    NetDeviceContainer devs0_8 = link_helper0.Install(gpunodes.Get(8), nvswtches.Get(1));
    NetDeviceContainer devs0_9 = link_helper0.Install(gpunodes.Get(9), nvswtches.Get(1));
    NetDeviceContainer devs0_10 = link_helper0.Install(gpunodes.Get(10), nvswtches.Get(1));
    NetDeviceContainer devs0_11 = link_helper0.Install(gpunodes.Get(11), nvswtches.Get(1));
    NetDeviceContainer devs0_12 = link_helper0.Install(gpunodes.Get(12), nvswtches.Get(1));
    NetDeviceContainer devs0_13 = link_helper0.Install(gpunodes.Get(13), nvswtches.Get(1));
    NetDeviceContainer devs0_14 = link_helper0.Install(gpunodes.Get(14), nvswtches.Get(1));
    NetDeviceContainer devs0_15 = link_helper0.Install(gpunodes.Get(15), nvswtches.Get(1));
    NetDeviceContainer devs0_16 = link_helper0.Install(gpunodes.Get(16), nvswtches.Get(2));
    NetDeviceContainer devs0_17 = link_helper0.Install(gpunodes.Get(17), nvswtches.Get(2));
    NetDeviceContainer devs0_18 = link_helper0.Install(gpunodes.Get(18), nvswtches.Get(2));
    NetDeviceContainer devs0_19 = link_helper0.Install(gpunodes.Get(19), nvswtches.Get(2));
    NetDeviceContainer devs0_20 = link_helper0.Install(gpunodes.Get(20), nvswtches.Get(2));
    NetDeviceContainer devs0_21 = link_helper0.Install(gpunodes.Get(21), nvswtches.Get(2));
    NetDeviceContainer devs0_22 = link_helper0.Install(gpunodes.Get(22), nvswtches.Get(2));
    NetDeviceContainer devs0_23 = link_helper0.Install(gpunodes.Get(23), nvswtches.Get(2));
    NetDeviceContainer devs0_24 = link_helper0.Install(gpunodes.Get(24), nvswtches.Get(3));
    NetDeviceContainer devs0_25 = link_helper0.Install(gpunodes.Get(25), nvswtches.Get(3));
    NetDeviceContainer devs0_26 = link_helper0.Install(gpunodes.Get(26), nvswtches.Get(3));
    NetDeviceContainer devs0_27 = link_helper0.Install(gpunodes.Get(27), nvswtches.Get(3));
    NetDeviceContainer devs0_28 = link_helper0.Install(gpunodes.Get(28), nvswtches.Get(3));
    NetDeviceContainer devs0_29 = link_helper0.Install(gpunodes.Get(29), nvswtches.Get(3));
    NetDeviceContainer devs0_30 = link_helper0.Install(gpunodes.Get(30), nvswtches.Get(3));
    NetDeviceContainer devs0_31 = link_helper0.Install(gpunodes.Get(31), nvswtches.Get(3));
    NetDeviceContainer devs0_32 = link_helper0.Install(gpunodes.Get(32), nvswtches.Get(4));
    NetDeviceContainer devs0_33 = link_helper0.Install(gpunodes.Get(33), nvswtches.Get(4));
    NetDeviceContainer devs0_34 = link_helper0.Install(gpunodes.Get(34), nvswtches.Get(4));
    NetDeviceContainer devs0_35 = link_helper0.Install(gpunodes.Get(35), nvswtches.Get(4));
    NetDeviceContainer devs0_36 = link_helper0.Install(gpunodes.Get(36), nvswtches.Get(4));
    NetDeviceContainer devs0_37 = link_helper0.Install(gpunodes.Get(37), nvswtches.Get(4));
    NetDeviceContainer devs0_38 = link_helper0.Install(gpunodes.Get(38), nvswtches.Get(4));
    NetDeviceContainer devs0_39 = link_helper0.Install(gpunodes.Get(39), nvswtches.Get(4));
    NetDeviceContainer devs0_40 = link_helper0.Install(gpunodes.Get(40), nvswtches.Get(5));
    NetDeviceContainer devs0_41 = link_helper0.Install(gpunodes.Get(41), nvswtches.Get(5));
    NetDeviceContainer devs0_42 = link_helper0.Install(gpunodes.Get(42), nvswtches.Get(5));
    NetDeviceContainer devs0_43 = link_helper0.Install(gpunodes.Get(43), nvswtches.Get(5));
    NetDeviceContainer devs0_44 = link_helper0.Install(gpunodes.Get(44), nvswtches.Get(5));
    NetDeviceContainer devs0_45 = link_helper0.Install(gpunodes.Get(45), nvswtches.Get(5));
    NetDeviceContainer devs0_46 = link_helper0.Install(gpunodes.Get(46), nvswtches.Get(5));
    NetDeviceContainer devs0_47 = link_helper0.Install(gpunodes.Get(47), nvswtches.Get(5));
    NetDeviceContainer devs0_48 = link_helper0.Install(gpunodes.Get(48), nvswtches.Get(6));
    NetDeviceContainer devs0_49 = link_helper0.Install(gpunodes.Get(49), nvswtches.Get(6));
    NetDeviceContainer devs0_50 = link_helper0.Install(gpunodes.Get(50), nvswtches.Get(6));
    NetDeviceContainer devs0_51 = link_helper0.Install(gpunodes.Get(51), nvswtches.Get(6));
    NetDeviceContainer devs0_52 = link_helper0.Install(gpunodes.Get(52), nvswtches.Get(6));
    NetDeviceContainer devs0_53 = link_helper0.Install(gpunodes.Get(53), nvswtches.Get(6));
    NetDeviceContainer devs0_54 = link_helper0.Install(gpunodes.Get(54), nvswtches.Get(6));
    NetDeviceContainer devs0_55 = link_helper0.Install(gpunodes.Get(55), nvswtches.Get(6));
    NetDeviceContainer devs0_56 = link_helper0.Install(gpunodes.Get(56), nvswtches.Get(7));
    NetDeviceContainer devs0_57 = link_helper0.Install(gpunodes.Get(57), nvswtches.Get(7));
    NetDeviceContainer devs0_58 = link_helper0.Install(gpunodes.Get(58), nvswtches.Get(7));
    NetDeviceContainer devs0_59 = link_helper0.Install(gpunodes.Get(59), nvswtches.Get(7));
    NetDeviceContainer devs0_60 = link_helper0.Install(gpunodes.Get(60), nvswtches.Get(7));
    NetDeviceContainer devs0_61 = link_helper0.Install(gpunodes.Get(61), nvswtches.Get(7));
    NetDeviceContainer devs0_62 = link_helper0.Install(gpunodes.Get(62), nvswtches.Get(7));
    NetDeviceContainer devs0_63 = link_helper0.Install(gpunodes.Get(63), nvswtches.Get(7));
    NetDeviceContainer devs0_64 = link_helper0.Install(gpunodes.Get(64), nvswtches.Get(8));
    NetDeviceContainer devs0_65 = link_helper0.Install(gpunodes.Get(65), nvswtches.Get(8));
    NetDeviceContainer devs0_66 = link_helper0.Install(gpunodes.Get(66), nvswtches.Get(8));
    NetDeviceContainer devs0_67 = link_helper0.Install(gpunodes.Get(67), nvswtches.Get(8));
    NetDeviceContainer devs0_68 = link_helper0.Install(gpunodes.Get(68), nvswtches.Get(8));
    NetDeviceContainer devs0_69 = link_helper0.Install(gpunodes.Get(69), nvswtches.Get(8));
    NetDeviceContainer devs0_70 = link_helper0.Install(gpunodes.Get(70), nvswtches.Get(8));
    NetDeviceContainer devs0_71 = link_helper0.Install(gpunodes.Get(71), nvswtches.Get(8));
    NetDeviceContainer devs0_72 = link_helper0.Install(gpunodes.Get(72), nvswtches.Get(9));
    NetDeviceContainer devs0_73 = link_helper0.Install(gpunodes.Get(73), nvswtches.Get(9));
    NetDeviceContainer devs0_74 = link_helper0.Install(gpunodes.Get(74), nvswtches.Get(9));
    NetDeviceContainer devs0_75 = link_helper0.Install(gpunodes.Get(75), nvswtches.Get(9));
    NetDeviceContainer devs0_76 = link_helper0.Install(gpunodes.Get(76), nvswtches.Get(9));
    NetDeviceContainer devs0_77 = link_helper0.Install(gpunodes.Get(77), nvswtches.Get(9));
    NetDeviceContainer devs0_78 = link_helper0.Install(gpunodes.Get(78), nvswtches.Get(9));
    NetDeviceContainer devs0_79 = link_helper0.Install(gpunodes.Get(79), nvswtches.Get(9));
    NetDeviceContainer devs0_80 = link_helper0.Install(gpunodes.Get(80), nvswtches.Get(10));
    NetDeviceContainer devs0_81 = link_helper0.Install(gpunodes.Get(81), nvswtches.Get(10));
    NetDeviceContainer devs0_82 = link_helper0.Install(gpunodes.Get(82), nvswtches.Get(10));
    NetDeviceContainer devs0_83 = link_helper0.Install(gpunodes.Get(83), nvswtches.Get(10));
    NetDeviceContainer devs0_84 = link_helper0.Install(gpunodes.Get(84), nvswtches.Get(10));
    NetDeviceContainer devs0_85 = link_helper0.Install(gpunodes.Get(85), nvswtches.Get(10));
    NetDeviceContainer devs0_86 = link_helper0.Install(gpunodes.Get(86), nvswtches.Get(10));
    NetDeviceContainer devs0_87 = link_helper0.Install(gpunodes.Get(87), nvswtches.Get(10));
    NetDeviceContainer devs0_88 = link_helper0.Install(gpunodes.Get(88), nvswtches.Get(11));
    NetDeviceContainer devs0_89 = link_helper0.Install(gpunodes.Get(89), nvswtches.Get(11));
    NetDeviceContainer devs0_90 = link_helper0.Install(gpunodes.Get(90), nvswtches.Get(11));
    NetDeviceContainer devs0_91 = link_helper0.Install(gpunodes.Get(91), nvswtches.Get(11));
    NetDeviceContainer devs0_92 = link_helper0.Install(gpunodes.Get(92), nvswtches.Get(11));
    NetDeviceContainer devs0_93 = link_helper0.Install(gpunodes.Get(93), nvswtches.Get(11));
    NetDeviceContainer devs0_94 = link_helper0.Install(gpunodes.Get(94), nvswtches.Get(11));
    NetDeviceContainer devs0_95 = link_helper0.Install(gpunodes.Get(95), nvswtches.Get(11));
    NetDeviceContainer devs0_96 = link_helper0.Install(gpunodes.Get(96), nvswtches.Get(12));
    NetDeviceContainer devs0_97 = link_helper0.Install(gpunodes.Get(97), nvswtches.Get(12));
    NetDeviceContainer devs0_98 = link_helper0.Install(gpunodes.Get(98), nvswtches.Get(12));
    NetDeviceContainer devs0_99 = link_helper0.Install(gpunodes.Get(99), nvswtches.Get(12));
    NetDeviceContainer devs0_100 = link_helper0.Install(gpunodes.Get(100), nvswtches.Get(12));
    NetDeviceContainer devs0_101 = link_helper0.Install(gpunodes.Get(101), nvswtches.Get(12));
    NetDeviceContainer devs0_102 = link_helper0.Install(gpunodes.Get(102), nvswtches.Get(12));
    NetDeviceContainer devs0_103 = link_helper0.Install(gpunodes.Get(103), nvswtches.Get(12));
    NetDeviceContainer devs0_104 = link_helper0.Install(gpunodes.Get(104), nvswtches.Get(13));
    NetDeviceContainer devs0_105 = link_helper0.Install(gpunodes.Get(105), nvswtches.Get(13));
    NetDeviceContainer devs0_106 = link_helper0.Install(gpunodes.Get(106), nvswtches.Get(13));
    NetDeviceContainer devs0_107 = link_helper0.Install(gpunodes.Get(107), nvswtches.Get(13));
    NetDeviceContainer devs0_108 = link_helper0.Install(gpunodes.Get(108), nvswtches.Get(13));
    NetDeviceContainer devs0_109 = link_helper0.Install(gpunodes.Get(109), nvswtches.Get(13));
    NetDeviceContainer devs0_110 = link_helper0.Install(gpunodes.Get(110), nvswtches.Get(13));
    NetDeviceContainer devs0_111 = link_helper0.Install(gpunodes.Get(111), nvswtches.Get(13));
    NetDeviceContainer devs0_112 = link_helper0.Install(gpunodes.Get(112), nvswtches.Get(14));
    NetDeviceContainer devs0_113 = link_helper0.Install(gpunodes.Get(113), nvswtches.Get(14));
    NetDeviceContainer devs0_114 = link_helper0.Install(gpunodes.Get(114), nvswtches.Get(14));
    NetDeviceContainer devs0_115 = link_helper0.Install(gpunodes.Get(115), nvswtches.Get(14));
    NetDeviceContainer devs0_116 = link_helper0.Install(gpunodes.Get(116), nvswtches.Get(14));
    NetDeviceContainer devs0_117 = link_helper0.Install(gpunodes.Get(117), nvswtches.Get(14));
    NetDeviceContainer devs0_118 = link_helper0.Install(gpunodes.Get(118), nvswtches.Get(14));
    NetDeviceContainer devs0_119 = link_helper0.Install(gpunodes.Get(119), nvswtches.Get(14));
    NetDeviceContainer devs0_120 = link_helper0.Install(gpunodes.Get(120), nvswtches.Get(15));
    NetDeviceContainer devs0_121 = link_helper0.Install(gpunodes.Get(121), nvswtches.Get(15));
    NetDeviceContainer devs0_122 = link_helper0.Install(gpunodes.Get(122), nvswtches.Get(15));
    NetDeviceContainer devs0_123 = link_helper0.Install(gpunodes.Get(123), nvswtches.Get(15));
    NetDeviceContainer devs0_124 = link_helper0.Install(gpunodes.Get(124), nvswtches.Get(15));
    NetDeviceContainer devs0_125 = link_helper0.Install(gpunodes.Get(125), nvswtches.Get(15));
    NetDeviceContainer devs0_126 = link_helper0.Install(gpunodes.Get(126), nvswtches.Get(15));
    NetDeviceContainer devs0_127 = link_helper0.Install(gpunodes.Get(127), nvswtches.Get(15));
    NetDeviceContainer devs0_128 = link_helper0.Install(gpunodes.Get(128), nvswtches.Get(16));
    NetDeviceContainer devs0_129 = link_helper0.Install(gpunodes.Get(129), nvswtches.Get(16));
    NetDeviceContainer devs0_130 = link_helper0.Install(gpunodes.Get(130), nvswtches.Get(16));
    NetDeviceContainer devs0_131 = link_helper0.Install(gpunodes.Get(131), nvswtches.Get(16));
    NetDeviceContainer devs0_132 = link_helper0.Install(gpunodes.Get(132), nvswtches.Get(16));
    NetDeviceContainer devs0_133 = link_helper0.Install(gpunodes.Get(133), nvswtches.Get(16));
    NetDeviceContainer devs0_134 = link_helper0.Install(gpunodes.Get(134), nvswtches.Get(16));
    NetDeviceContainer devs0_135 = link_helper0.Install(gpunodes.Get(135), nvswtches.Get(16));
    NetDeviceContainer devs0_136 = link_helper0.Install(gpunodes.Get(136), nvswtches.Get(17));
    NetDeviceContainer devs0_137 = link_helper0.Install(gpunodes.Get(137), nvswtches.Get(17));
    NetDeviceContainer devs0_138 = link_helper0.Install(gpunodes.Get(138), nvswtches.Get(17));
    NetDeviceContainer devs0_139 = link_helper0.Install(gpunodes.Get(139), nvswtches.Get(17));
    NetDeviceContainer devs0_140 = link_helper0.Install(gpunodes.Get(140), nvswtches.Get(17));
    NetDeviceContainer devs0_141 = link_helper0.Install(gpunodes.Get(141), nvswtches.Get(17));
    NetDeviceContainer devs0_142 = link_helper0.Install(gpunodes.Get(142), nvswtches.Get(17));
    NetDeviceContainer devs0_143 = link_helper0.Install(gpunodes.Get(143), nvswtches.Get(17));
    NetDeviceContainer devs0_144 = link_helper0.Install(gpunodes.Get(144), nvswtches.Get(18));
    NetDeviceContainer devs0_145 = link_helper0.Install(gpunodes.Get(145), nvswtches.Get(18));
    NetDeviceContainer devs0_146 = link_helper0.Install(gpunodes.Get(146), nvswtches.Get(18));
    NetDeviceContainer devs0_147 = link_helper0.Install(gpunodes.Get(147), nvswtches.Get(18));
    NetDeviceContainer devs0_148 = link_helper0.Install(gpunodes.Get(148), nvswtches.Get(18));
    NetDeviceContainer devs0_149 = link_helper0.Install(gpunodes.Get(149), nvswtches.Get(18));
    NetDeviceContainer devs0_150 = link_helper0.Install(gpunodes.Get(150), nvswtches.Get(18));
    NetDeviceContainer devs0_151 = link_helper0.Install(gpunodes.Get(151), nvswtches.Get(18));
    NetDeviceContainer devs0_152 = link_helper0.Install(gpunodes.Get(152), nvswtches.Get(19));
    NetDeviceContainer devs0_153 = link_helper0.Install(gpunodes.Get(153), nvswtches.Get(19));
    NetDeviceContainer devs0_154 = link_helper0.Install(gpunodes.Get(154), nvswtches.Get(19));
    NetDeviceContainer devs0_155 = link_helper0.Install(gpunodes.Get(155), nvswtches.Get(19));
    NetDeviceContainer devs0_156 = link_helper0.Install(gpunodes.Get(156), nvswtches.Get(19));
    NetDeviceContainer devs0_157 = link_helper0.Install(gpunodes.Get(157), nvswtches.Get(19));
    NetDeviceContainer devs0_158 = link_helper0.Install(gpunodes.Get(158), nvswtches.Get(19));
    NetDeviceContainer devs0_159 = link_helper0.Install(gpunodes.Get(159), nvswtches.Get(19));
    NetDeviceContainer devs0_160 = link_helper0.Install(gpunodes.Get(160), nvswtches.Get(20));
    NetDeviceContainer devs0_161 = link_helper0.Install(gpunodes.Get(161), nvswtches.Get(20));
    NetDeviceContainer devs0_162 = link_helper0.Install(gpunodes.Get(162), nvswtches.Get(20));
    NetDeviceContainer devs0_163 = link_helper0.Install(gpunodes.Get(163), nvswtches.Get(20));
    NetDeviceContainer devs0_164 = link_helper0.Install(gpunodes.Get(164), nvswtches.Get(20));
    NetDeviceContainer devs0_165 = link_helper0.Install(gpunodes.Get(165), nvswtches.Get(20));
    NetDeviceContainer devs0_166 = link_helper0.Install(gpunodes.Get(166), nvswtches.Get(20));
    NetDeviceContainer devs0_167 = link_helper0.Install(gpunodes.Get(167), nvswtches.Get(20));
    NetDeviceContainer devs0_168 = link_helper0.Install(gpunodes.Get(168), nvswtches.Get(21));
    NetDeviceContainer devs0_169 = link_helper0.Install(gpunodes.Get(169), nvswtches.Get(21));
    NetDeviceContainer devs0_170 = link_helper0.Install(gpunodes.Get(170), nvswtches.Get(21));
    NetDeviceContainer devs0_171 = link_helper0.Install(gpunodes.Get(171), nvswtches.Get(21));
    NetDeviceContainer devs0_172 = link_helper0.Install(gpunodes.Get(172), nvswtches.Get(21));
    NetDeviceContainer devs0_173 = link_helper0.Install(gpunodes.Get(173), nvswtches.Get(21));
    NetDeviceContainer devs0_174 = link_helper0.Install(gpunodes.Get(174), nvswtches.Get(21));
    NetDeviceContainer devs0_175 = link_helper0.Install(gpunodes.Get(175), nvswtches.Get(21));
    NetDeviceContainer devs0_176 = link_helper0.Install(gpunodes.Get(176), nvswtches.Get(22));
    NetDeviceContainer devs0_177 = link_helper0.Install(gpunodes.Get(177), nvswtches.Get(22));
    NetDeviceContainer devs0_178 = link_helper0.Install(gpunodes.Get(178), nvswtches.Get(22));
    NetDeviceContainer devs0_179 = link_helper0.Install(gpunodes.Get(179), nvswtches.Get(22));
    NetDeviceContainer devs0_180 = link_helper0.Install(gpunodes.Get(180), nvswtches.Get(22));
    NetDeviceContainer devs0_181 = link_helper0.Install(gpunodes.Get(181), nvswtches.Get(22));
    NetDeviceContainer devs0_182 = link_helper0.Install(gpunodes.Get(182), nvswtches.Get(22));
    NetDeviceContainer devs0_183 = link_helper0.Install(gpunodes.Get(183), nvswtches.Get(22));
    NetDeviceContainer devs0_184 = link_helper0.Install(gpunodes.Get(184), nvswtches.Get(23));
    NetDeviceContainer devs0_185 = link_helper0.Install(gpunodes.Get(185), nvswtches.Get(23));
    NetDeviceContainer devs0_186 = link_helper0.Install(gpunodes.Get(186), nvswtches.Get(23));
    NetDeviceContainer devs0_187 = link_helper0.Install(gpunodes.Get(187), nvswtches.Get(23));
    NetDeviceContainer devs0_188 = link_helper0.Install(gpunodes.Get(188), nvswtches.Get(23));
    NetDeviceContainer devs0_189 = link_helper0.Install(gpunodes.Get(189), nvswtches.Get(23));
    NetDeviceContainer devs0_190 = link_helper0.Install(gpunodes.Get(190), nvswtches.Get(23));
    NetDeviceContainer devs0_191 = link_helper0.Install(gpunodes.Get(191), nvswtches.Get(23));
    NetDeviceContainer devs0_192 = link_helper0.Install(gpunodes.Get(192), nvswtches.Get(24));
    NetDeviceContainer devs0_193 = link_helper0.Install(gpunodes.Get(193), nvswtches.Get(24));
    NetDeviceContainer devs0_194 = link_helper0.Install(gpunodes.Get(194), nvswtches.Get(24));
    NetDeviceContainer devs0_195 = link_helper0.Install(gpunodes.Get(195), nvswtches.Get(24));
    NetDeviceContainer devs0_196 = link_helper0.Install(gpunodes.Get(196), nvswtches.Get(24));
    NetDeviceContainer devs0_197 = link_helper0.Install(gpunodes.Get(197), nvswtches.Get(24));
    NetDeviceContainer devs0_198 = link_helper0.Install(gpunodes.Get(198), nvswtches.Get(24));
    NetDeviceContainer devs0_199 = link_helper0.Install(gpunodes.Get(199), nvswtches.Get(24));
    NetDeviceContainer devs0_200 = link_helper0.Install(gpunodes.Get(200), nvswtches.Get(25));
    NetDeviceContainer devs0_201 = link_helper0.Install(gpunodes.Get(201), nvswtches.Get(25));
    NetDeviceContainer devs0_202 = link_helper0.Install(gpunodes.Get(202), nvswtches.Get(25));
    NetDeviceContainer devs0_203 = link_helper0.Install(gpunodes.Get(203), nvswtches.Get(25));
    NetDeviceContainer devs0_204 = link_helper0.Install(gpunodes.Get(204), nvswtches.Get(25));
    NetDeviceContainer devs0_205 = link_helper0.Install(gpunodes.Get(205), nvswtches.Get(25));
    NetDeviceContainer devs0_206 = link_helper0.Install(gpunodes.Get(206), nvswtches.Get(25));
    NetDeviceContainer devs0_207 = link_helper0.Install(gpunodes.Get(207), nvswtches.Get(25));
    NetDeviceContainer devs0_208 = link_helper0.Install(gpunodes.Get(208), nvswtches.Get(26));
    NetDeviceContainer devs0_209 = link_helper0.Install(gpunodes.Get(209), nvswtches.Get(26));
    NetDeviceContainer devs0_210 = link_helper0.Install(gpunodes.Get(210), nvswtches.Get(26));
    NetDeviceContainer devs0_211 = link_helper0.Install(gpunodes.Get(211), nvswtches.Get(26));
    NetDeviceContainer devs0_212 = link_helper0.Install(gpunodes.Get(212), nvswtches.Get(26));
    NetDeviceContainer devs0_213 = link_helper0.Install(gpunodes.Get(213), nvswtches.Get(26));
    NetDeviceContainer devs0_214 = link_helper0.Install(gpunodes.Get(214), nvswtches.Get(26));
    NetDeviceContainer devs0_215 = link_helper0.Install(gpunodes.Get(215), nvswtches.Get(26));
    NetDeviceContainer devs0_216 = link_helper0.Install(gpunodes.Get(216), nvswtches.Get(27));
    NetDeviceContainer devs0_217 = link_helper0.Install(gpunodes.Get(217), nvswtches.Get(27));
    NetDeviceContainer devs0_218 = link_helper0.Install(gpunodes.Get(218), nvswtches.Get(27));
    NetDeviceContainer devs0_219 = link_helper0.Install(gpunodes.Get(219), nvswtches.Get(27));
    NetDeviceContainer devs0_220 = link_helper0.Install(gpunodes.Get(220), nvswtches.Get(27));
    NetDeviceContainer devs0_221 = link_helper0.Install(gpunodes.Get(221), nvswtches.Get(27));
    NetDeviceContainer devs0_222 = link_helper0.Install(gpunodes.Get(222), nvswtches.Get(27));
    NetDeviceContainer devs0_223 = link_helper0.Install(gpunodes.Get(223), nvswtches.Get(27));
    NetDeviceContainer devs0_224 = link_helper0.Install(gpunodes.Get(224), nvswtches.Get(28));
    NetDeviceContainer devs0_225 = link_helper0.Install(gpunodes.Get(225), nvswtches.Get(28));
    NetDeviceContainer devs0_226 = link_helper0.Install(gpunodes.Get(226), nvswtches.Get(28));
    NetDeviceContainer devs0_227 = link_helper0.Install(gpunodes.Get(227), nvswtches.Get(28));
    NetDeviceContainer devs0_228 = link_helper0.Install(gpunodes.Get(228), nvswtches.Get(28));
    NetDeviceContainer devs0_229 = link_helper0.Install(gpunodes.Get(229), nvswtches.Get(28));
    NetDeviceContainer devs0_230 = link_helper0.Install(gpunodes.Get(230), nvswtches.Get(28));
    NetDeviceContainer devs0_231 = link_helper0.Install(gpunodes.Get(231), nvswtches.Get(28));
    NetDeviceContainer devs0_232 = link_helper0.Install(gpunodes.Get(232), nvswtches.Get(29));
    NetDeviceContainer devs0_233 = link_helper0.Install(gpunodes.Get(233), nvswtches.Get(29));
    NetDeviceContainer devs0_234 = link_helper0.Install(gpunodes.Get(234), nvswtches.Get(29));
    NetDeviceContainer devs0_235 = link_helper0.Install(gpunodes.Get(235), nvswtches.Get(29));
    NetDeviceContainer devs0_236 = link_helper0.Install(gpunodes.Get(236), nvswtches.Get(29));
    NetDeviceContainer devs0_237 = link_helper0.Install(gpunodes.Get(237), nvswtches.Get(29));
    NetDeviceContainer devs0_238 = link_helper0.Install(gpunodes.Get(238), nvswtches.Get(29));
    NetDeviceContainer devs0_239 = link_helper0.Install(gpunodes.Get(239), nvswtches.Get(29));
    NetDeviceContainer devs0_240 = link_helper0.Install(gpunodes.Get(240), nvswtches.Get(30));
    NetDeviceContainer devs0_241 = link_helper0.Install(gpunodes.Get(241), nvswtches.Get(30));
    NetDeviceContainer devs0_242 = link_helper0.Install(gpunodes.Get(242), nvswtches.Get(30));
    NetDeviceContainer devs0_243 = link_helper0.Install(gpunodes.Get(243), nvswtches.Get(30));
    NetDeviceContainer devs0_244 = link_helper0.Install(gpunodes.Get(244), nvswtches.Get(30));
    NetDeviceContainer devs0_245 = link_helper0.Install(gpunodes.Get(245), nvswtches.Get(30));
    NetDeviceContainer devs0_246 = link_helper0.Install(gpunodes.Get(246), nvswtches.Get(30));
    NetDeviceContainer devs0_247 = link_helper0.Install(gpunodes.Get(247), nvswtches.Get(30));
    NetDeviceContainer devs0_248 = link_helper0.Install(gpunodes.Get(248), nvswtches.Get(31));
    NetDeviceContainer devs0_249 = link_helper0.Install(gpunodes.Get(249), nvswtches.Get(31));
    NetDeviceContainer devs0_250 = link_helper0.Install(gpunodes.Get(250), nvswtches.Get(31));
    NetDeviceContainer devs0_251 = link_helper0.Install(gpunodes.Get(251), nvswtches.Get(31));
    NetDeviceContainer devs0_252 = link_helper0.Install(gpunodes.Get(252), nvswtches.Get(31));
    NetDeviceContainer devs0_253 = link_helper0.Install(gpunodes.Get(253), nvswtches.Get(31));
    NetDeviceContainer devs0_254 = link_helper0.Install(gpunodes.Get(254), nvswtches.Get(31));
    NetDeviceContainer devs0_255 = link_helper0.Install(gpunodes.Get(255), nvswtches.Get(31));
    NetDeviceContainer devs1_256 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(0));
    NetDeviceContainer devs1_257 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(8));
    NetDeviceContainer devs1_258 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(16));
    NetDeviceContainer devs1_259 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(24));
    NetDeviceContainer devs1_260 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(32));
    NetDeviceContainer devs1_261 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(40));
    NetDeviceContainer devs1_262 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(48));
    NetDeviceContainer devs1_263 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(56));
    NetDeviceContainer devs1_264 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(64));
    NetDeviceContainer devs1_265 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(72));
    NetDeviceContainer devs1_266 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(80));
    NetDeviceContainer devs1_267 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(88));
    NetDeviceContainer devs1_268 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(96));
    NetDeviceContainer devs1_269 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(104));
    NetDeviceContainer devs1_270 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(112));
    NetDeviceContainer devs1_271 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(120));
    NetDeviceContainer devs1_272 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(128));
    NetDeviceContainer devs1_273 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(136));
    NetDeviceContainer devs1_274 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(144));
    NetDeviceContainer devs1_275 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(152));
    NetDeviceContainer devs1_276 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(160));
    NetDeviceContainer devs1_277 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(168));
    NetDeviceContainer devs1_278 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(176));
    NetDeviceContainer devs1_279 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(184));
    NetDeviceContainer devs1_280 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(192));
    NetDeviceContainer devs1_281 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(200));
    NetDeviceContainer devs1_282 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(208));
    NetDeviceContainer devs1_283 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(216));
    NetDeviceContainer devs1_284 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(224));
    NetDeviceContainer devs1_285 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(232));
    NetDeviceContainer devs1_286 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(240));
    NetDeviceContainer devs1_287 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(248));
    NetDeviceContainer devs1_288 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(1));
    NetDeviceContainer devs1_289 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(9));
    NetDeviceContainer devs1_290 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(17));
    NetDeviceContainer devs1_291 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(25));
    NetDeviceContainer devs1_292 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(33));
    NetDeviceContainer devs1_293 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(41));
    NetDeviceContainer devs1_294 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(49));
    NetDeviceContainer devs1_295 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(57));
    NetDeviceContainer devs1_296 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(65));
    NetDeviceContainer devs1_297 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(73));
    NetDeviceContainer devs1_298 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(81));
    NetDeviceContainer devs1_299 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(89));
    NetDeviceContainer devs1_300 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(97));
    NetDeviceContainer devs1_301 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(105));
    NetDeviceContainer devs1_302 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(113));
    NetDeviceContainer devs1_303 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(121));
    NetDeviceContainer devs1_304 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(129));
    NetDeviceContainer devs1_305 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(137));
    NetDeviceContainer devs1_306 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(145));
    NetDeviceContainer devs1_307 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(153));
    NetDeviceContainer devs1_308 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(161));
    NetDeviceContainer devs1_309 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(169));
    NetDeviceContainer devs1_310 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(177));
    NetDeviceContainer devs1_311 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(185));
    NetDeviceContainer devs1_312 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(193));
    NetDeviceContainer devs1_313 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(201));
    NetDeviceContainer devs1_314 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(209));
    NetDeviceContainer devs1_315 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(217));
    NetDeviceContainer devs1_316 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(225));
    NetDeviceContainer devs1_317 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(233));
    NetDeviceContainer devs1_318 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(241));
    NetDeviceContainer devs1_319 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(249));
    NetDeviceContainer devs1_320 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(2));
    NetDeviceContainer devs1_321 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(10));
    NetDeviceContainer devs1_322 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(18));
    NetDeviceContainer devs1_323 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(26));
    NetDeviceContainer devs1_324 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(34));
    NetDeviceContainer devs1_325 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(42));
    NetDeviceContainer devs1_326 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(50));
    NetDeviceContainer devs1_327 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(58));
    NetDeviceContainer devs1_328 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(66));
    NetDeviceContainer devs1_329 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(74));
    NetDeviceContainer devs1_330 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(82));
    NetDeviceContainer devs1_331 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(90));
    NetDeviceContainer devs1_332 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(98));
    NetDeviceContainer devs1_333 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(106));
    NetDeviceContainer devs1_334 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(114));
    NetDeviceContainer devs1_335 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(122));
    NetDeviceContainer devs1_336 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(130));
    NetDeviceContainer devs1_337 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(138));
    NetDeviceContainer devs1_338 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(146));
    NetDeviceContainer devs1_339 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(154));
    NetDeviceContainer devs1_340 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(162));
    NetDeviceContainer devs1_341 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(170));
    NetDeviceContainer devs1_342 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(178));
    NetDeviceContainer devs1_343 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(186));
    NetDeviceContainer devs1_344 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(194));
    NetDeviceContainer devs1_345 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(202));
    NetDeviceContainer devs1_346 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(210));
    NetDeviceContainer devs1_347 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(218));
    NetDeviceContainer devs1_348 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(226));
    NetDeviceContainer devs1_349 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(234));
    NetDeviceContainer devs1_350 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(242));
    NetDeviceContainer devs1_351 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(250));
    NetDeviceContainer devs1_352 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(3));
    NetDeviceContainer devs1_353 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(11));
    NetDeviceContainer devs1_354 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(19));
    NetDeviceContainer devs1_355 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(27));
    NetDeviceContainer devs1_356 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(35));
    NetDeviceContainer devs1_357 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(43));
    NetDeviceContainer devs1_358 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(51));
    NetDeviceContainer devs1_359 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(59));
    NetDeviceContainer devs1_360 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(67));
    NetDeviceContainer devs1_361 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(75));
    NetDeviceContainer devs1_362 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(83));
    NetDeviceContainer devs1_363 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(91));
    NetDeviceContainer devs1_364 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(99));
    NetDeviceContainer devs1_365 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(107));
    NetDeviceContainer devs1_366 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(115));
    NetDeviceContainer devs1_367 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(123));
    NetDeviceContainer devs1_368 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(131));
    NetDeviceContainer devs1_369 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(139));
    NetDeviceContainer devs1_370 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(147));
    NetDeviceContainer devs1_371 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(155));
    NetDeviceContainer devs1_372 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(163));
    NetDeviceContainer devs1_373 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(171));
    NetDeviceContainer devs1_374 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(179));
    NetDeviceContainer devs1_375 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(187));
    NetDeviceContainer devs1_376 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(195));
    NetDeviceContainer devs1_377 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(203));
    NetDeviceContainer devs1_378 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(211));
    NetDeviceContainer devs1_379 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(219));
    NetDeviceContainer devs1_380 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(227));
    NetDeviceContainer devs1_381 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(235));
    NetDeviceContainer devs1_382 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(243));
    NetDeviceContainer devs1_383 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(251));
    NetDeviceContainer devs1_384 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(4));
    NetDeviceContainer devs1_385 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(12));
    NetDeviceContainer devs1_386 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(20));
    NetDeviceContainer devs1_387 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(28));
    NetDeviceContainer devs1_388 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(36));
    NetDeviceContainer devs1_389 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(44));
    NetDeviceContainer devs1_390 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(52));
    NetDeviceContainer devs1_391 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(60));
    NetDeviceContainer devs1_392 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(68));
    NetDeviceContainer devs1_393 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(76));
    NetDeviceContainer devs1_394 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(84));
    NetDeviceContainer devs1_395 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(92));
    NetDeviceContainer devs1_396 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(100));
    NetDeviceContainer devs1_397 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(108));
    NetDeviceContainer devs1_398 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(116));
    NetDeviceContainer devs1_399 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(124));
    NetDeviceContainer devs1_400 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(132));
    NetDeviceContainer devs1_401 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(140));
    NetDeviceContainer devs1_402 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(148));
    NetDeviceContainer devs1_403 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(156));
    NetDeviceContainer devs1_404 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(164));
    NetDeviceContainer devs1_405 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(172));
    NetDeviceContainer devs1_406 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(180));
    NetDeviceContainer devs1_407 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(188));
    NetDeviceContainer devs1_408 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(196));
    NetDeviceContainer devs1_409 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(204));
    NetDeviceContainer devs1_410 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(212));
    NetDeviceContainer devs1_411 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(220));
    NetDeviceContainer devs1_412 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(228));
    NetDeviceContainer devs1_413 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(236));
    NetDeviceContainer devs1_414 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(244));
    NetDeviceContainer devs1_415 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(252));
    NetDeviceContainer devs1_416 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(5));
    NetDeviceContainer devs1_417 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(13));
    NetDeviceContainer devs1_418 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(21));
    NetDeviceContainer devs1_419 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(29));
    NetDeviceContainer devs1_420 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(37));
    NetDeviceContainer devs1_421 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(45));
    NetDeviceContainer devs1_422 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(53));
    NetDeviceContainer devs1_423 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(61));
    NetDeviceContainer devs1_424 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(69));
    NetDeviceContainer devs1_425 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(77));
    NetDeviceContainer devs1_426 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(85));
    NetDeviceContainer devs1_427 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(93));
    NetDeviceContainer devs1_428 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(101));
    NetDeviceContainer devs1_429 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(109));
    NetDeviceContainer devs1_430 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(117));
    NetDeviceContainer devs1_431 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(125));
    NetDeviceContainer devs1_432 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(133));
    NetDeviceContainer devs1_433 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(141));
    NetDeviceContainer devs1_434 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(149));
    NetDeviceContainer devs1_435 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(157));
    NetDeviceContainer devs1_436 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(165));
    NetDeviceContainer devs1_437 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(173));
    NetDeviceContainer devs1_438 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(181));
    NetDeviceContainer devs1_439 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(189));
    NetDeviceContainer devs1_440 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(197));
    NetDeviceContainer devs1_441 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(205));
    NetDeviceContainer devs1_442 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(213));
    NetDeviceContainer devs1_443 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(221));
    NetDeviceContainer devs1_444 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(229));
    NetDeviceContainer devs1_445 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(237));
    NetDeviceContainer devs1_446 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(245));
    NetDeviceContainer devs1_447 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(253));
    NetDeviceContainer devs1_448 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(6));
    NetDeviceContainer devs1_449 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(14));
    NetDeviceContainer devs1_450 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(22));
    NetDeviceContainer devs1_451 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(30));
    NetDeviceContainer devs1_452 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(38));
    NetDeviceContainer devs1_453 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(46));
    NetDeviceContainer devs1_454 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(54));
    NetDeviceContainer devs1_455 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(62));
    NetDeviceContainer devs1_456 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(70));
    NetDeviceContainer devs1_457 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(78));
    NetDeviceContainer devs1_458 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(86));
    NetDeviceContainer devs1_459 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(94));
    NetDeviceContainer devs1_460 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(102));
    NetDeviceContainer devs1_461 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(110));
    NetDeviceContainer devs1_462 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(118));
    NetDeviceContainer devs1_463 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(126));
    NetDeviceContainer devs1_464 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(134));
    NetDeviceContainer devs1_465 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(142));
    NetDeviceContainer devs1_466 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(150));
    NetDeviceContainer devs1_467 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(158));
    NetDeviceContainer devs1_468 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(166));
    NetDeviceContainer devs1_469 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(174));
    NetDeviceContainer devs1_470 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(182));
    NetDeviceContainer devs1_471 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(190));
    NetDeviceContainer devs1_472 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(198));
    NetDeviceContainer devs1_473 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(206));
    NetDeviceContainer devs1_474 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(214));
    NetDeviceContainer devs1_475 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(222));
    NetDeviceContainer devs1_476 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(230));
    NetDeviceContainer devs1_477 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(238));
    NetDeviceContainer devs1_478 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(246));
    NetDeviceContainer devs1_479 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(254));
    NetDeviceContainer devs1_480 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(7));
    NetDeviceContainer devs1_481 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(15));
    NetDeviceContainer devs1_482 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(23));
    NetDeviceContainer devs1_483 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(31));
    NetDeviceContainer devs1_484 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(39));
    NetDeviceContainer devs1_485 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(47));
    NetDeviceContainer devs1_486 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(55));
    NetDeviceContainer devs1_487 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(63));
    NetDeviceContainer devs1_488 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(71));
    NetDeviceContainer devs1_489 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(79));
    NetDeviceContainer devs1_490 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(87));
    NetDeviceContainer devs1_491 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(95));
    NetDeviceContainer devs1_492 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(103));
    NetDeviceContainer devs1_493 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(111));
    NetDeviceContainer devs1_494 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(119));
    NetDeviceContainer devs1_495 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(127));
    NetDeviceContainer devs1_496 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(135));
    NetDeviceContainer devs1_497 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(143));
    NetDeviceContainer devs1_498 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(151));
    NetDeviceContainer devs1_499 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(159));
    NetDeviceContainer devs1_500 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(167));
    NetDeviceContainer devs1_501 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(175));
    NetDeviceContainer devs1_502 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(183));
    NetDeviceContainer devs1_503 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(191));
    NetDeviceContainer devs1_504 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(199));
    NetDeviceContainer devs1_505 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(207));
    NetDeviceContainer devs1_506 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(215));
    NetDeviceContainer devs1_507 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(223));
    NetDeviceContainer devs1_508 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(231));
    NetDeviceContainer devs1_509 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(239));
    NetDeviceContainer devs1_510 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(247));
    NetDeviceContainer devs1_511 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(255));
    NetDeviceContainer devs1_512 = link_helper1.Install(regswtches.Get(0), regswtches.Get(8));
    NetDeviceContainer devs1_513 = link_helper1.Install(regswtches.Get(0), regswtches.Get(8));
    NetDeviceContainer devs1_514 = link_helper1.Install(regswtches.Get(0), regswtches.Get(8));
    NetDeviceContainer devs1_515 = link_helper1.Install(regswtches.Get(0), regswtches.Get(8));
    NetDeviceContainer devs1_516 = link_helper1.Install(regswtches.Get(0), regswtches.Get(8));
    NetDeviceContainer devs1_517 = link_helper1.Install(regswtches.Get(0), regswtches.Get(8));
    NetDeviceContainer devs1_518 = link_helper1.Install(regswtches.Get(0), regswtches.Get(8));
    NetDeviceContainer devs1_519 = link_helper1.Install(regswtches.Get(0), regswtches.Get(8));
    NetDeviceContainer devs1_520 = link_helper1.Install(regswtches.Get(0), regswtches.Get(9));
    NetDeviceContainer devs1_521 = link_helper1.Install(regswtches.Get(0), regswtches.Get(9));
    NetDeviceContainer devs1_522 = link_helper1.Install(regswtches.Get(0), regswtches.Get(9));
    NetDeviceContainer devs1_523 = link_helper1.Install(regswtches.Get(0), regswtches.Get(9));
    NetDeviceContainer devs1_524 = link_helper1.Install(regswtches.Get(0), regswtches.Get(9));
    NetDeviceContainer devs1_525 = link_helper1.Install(regswtches.Get(0), regswtches.Get(9));
    NetDeviceContainer devs1_526 = link_helper1.Install(regswtches.Get(0), regswtches.Get(9));
    NetDeviceContainer devs1_527 = link_helper1.Install(regswtches.Get(0), regswtches.Get(9));
    NetDeviceContainer devs1_528 = link_helper1.Install(regswtches.Get(0), regswtches.Get(10));
    NetDeviceContainer devs1_529 = link_helper1.Install(regswtches.Get(0), regswtches.Get(10));
    NetDeviceContainer devs1_530 = link_helper1.Install(regswtches.Get(0), regswtches.Get(10));
    NetDeviceContainer devs1_531 = link_helper1.Install(regswtches.Get(0), regswtches.Get(10));
    NetDeviceContainer devs1_532 = link_helper1.Install(regswtches.Get(0), regswtches.Get(10));
    NetDeviceContainer devs1_533 = link_helper1.Install(regswtches.Get(0), regswtches.Get(10));
    NetDeviceContainer devs1_534 = link_helper1.Install(regswtches.Get(0), regswtches.Get(10));
    NetDeviceContainer devs1_535 = link_helper1.Install(regswtches.Get(0), regswtches.Get(10));
    NetDeviceContainer devs1_536 = link_helper1.Install(regswtches.Get(0), regswtches.Get(11));
    NetDeviceContainer devs1_537 = link_helper1.Install(regswtches.Get(0), regswtches.Get(11));
    NetDeviceContainer devs1_538 = link_helper1.Install(regswtches.Get(0), regswtches.Get(11));
    NetDeviceContainer devs1_539 = link_helper1.Install(regswtches.Get(0), regswtches.Get(11));
    NetDeviceContainer devs1_540 = link_helper1.Install(regswtches.Get(0), regswtches.Get(11));
    NetDeviceContainer devs1_541 = link_helper1.Install(regswtches.Get(0), regswtches.Get(11));
    NetDeviceContainer devs1_542 = link_helper1.Install(regswtches.Get(0), regswtches.Get(11));
    NetDeviceContainer devs1_543 = link_helper1.Install(regswtches.Get(0), regswtches.Get(11));
    NetDeviceContainer devs1_544 = link_helper1.Install(regswtches.Get(1), regswtches.Get(8));
    NetDeviceContainer devs1_545 = link_helper1.Install(regswtches.Get(1), regswtches.Get(8));
    NetDeviceContainer devs1_546 = link_helper1.Install(regswtches.Get(1), regswtches.Get(8));
    NetDeviceContainer devs1_547 = link_helper1.Install(regswtches.Get(1), regswtches.Get(8));
    NetDeviceContainer devs1_548 = link_helper1.Install(regswtches.Get(1), regswtches.Get(8));
    NetDeviceContainer devs1_549 = link_helper1.Install(regswtches.Get(1), regswtches.Get(8));
    NetDeviceContainer devs1_550 = link_helper1.Install(regswtches.Get(1), regswtches.Get(8));
    NetDeviceContainer devs1_551 = link_helper1.Install(regswtches.Get(1), regswtches.Get(8));
    NetDeviceContainer devs1_552 = link_helper1.Install(regswtches.Get(1), regswtches.Get(9));
    NetDeviceContainer devs1_553 = link_helper1.Install(regswtches.Get(1), regswtches.Get(9));
    NetDeviceContainer devs1_554 = link_helper1.Install(regswtches.Get(1), regswtches.Get(9));
    NetDeviceContainer devs1_555 = link_helper1.Install(regswtches.Get(1), regswtches.Get(9));
    NetDeviceContainer devs1_556 = link_helper1.Install(regswtches.Get(1), regswtches.Get(9));
    NetDeviceContainer devs1_557 = link_helper1.Install(regswtches.Get(1), regswtches.Get(9));
    NetDeviceContainer devs1_558 = link_helper1.Install(regswtches.Get(1), regswtches.Get(9));
    NetDeviceContainer devs1_559 = link_helper1.Install(regswtches.Get(1), regswtches.Get(9));
    NetDeviceContainer devs1_560 = link_helper1.Install(regswtches.Get(1), regswtches.Get(10));
    NetDeviceContainer devs1_561 = link_helper1.Install(regswtches.Get(1), regswtches.Get(10));
    NetDeviceContainer devs1_562 = link_helper1.Install(regswtches.Get(1), regswtches.Get(10));
    NetDeviceContainer devs1_563 = link_helper1.Install(regswtches.Get(1), regswtches.Get(10));
    NetDeviceContainer devs1_564 = link_helper1.Install(regswtches.Get(1), regswtches.Get(10));
    NetDeviceContainer devs1_565 = link_helper1.Install(regswtches.Get(1), regswtches.Get(10));
    NetDeviceContainer devs1_566 = link_helper1.Install(regswtches.Get(1), regswtches.Get(10));
    NetDeviceContainer devs1_567 = link_helper1.Install(regswtches.Get(1), regswtches.Get(10));
    NetDeviceContainer devs1_568 = link_helper1.Install(regswtches.Get(1), regswtches.Get(11));
    NetDeviceContainer devs1_569 = link_helper1.Install(regswtches.Get(1), regswtches.Get(11));
    NetDeviceContainer devs1_570 = link_helper1.Install(regswtches.Get(1), regswtches.Get(11));
    NetDeviceContainer devs1_571 = link_helper1.Install(regswtches.Get(1), regswtches.Get(11));
    NetDeviceContainer devs1_572 = link_helper1.Install(regswtches.Get(1), regswtches.Get(11));
    NetDeviceContainer devs1_573 = link_helper1.Install(regswtches.Get(1), regswtches.Get(11));
    NetDeviceContainer devs1_574 = link_helper1.Install(regswtches.Get(1), regswtches.Get(11));
    NetDeviceContainer devs1_575 = link_helper1.Install(regswtches.Get(1), regswtches.Get(11));
    NetDeviceContainer devs1_576 = link_helper1.Install(regswtches.Get(2), regswtches.Get(8));
    NetDeviceContainer devs1_577 = link_helper1.Install(regswtches.Get(2), regswtches.Get(8));
    NetDeviceContainer devs1_578 = link_helper1.Install(regswtches.Get(2), regswtches.Get(8));
    NetDeviceContainer devs1_579 = link_helper1.Install(regswtches.Get(2), regswtches.Get(8));
    NetDeviceContainer devs1_580 = link_helper1.Install(regswtches.Get(2), regswtches.Get(8));
    NetDeviceContainer devs1_581 = link_helper1.Install(regswtches.Get(2), regswtches.Get(8));
    NetDeviceContainer devs1_582 = link_helper1.Install(regswtches.Get(2), regswtches.Get(8));
    NetDeviceContainer devs1_583 = link_helper1.Install(regswtches.Get(2), regswtches.Get(8));
    NetDeviceContainer devs1_584 = link_helper1.Install(regswtches.Get(2), regswtches.Get(9));
    NetDeviceContainer devs1_585 = link_helper1.Install(regswtches.Get(2), regswtches.Get(9));
    NetDeviceContainer devs1_586 = link_helper1.Install(regswtches.Get(2), regswtches.Get(9));
    NetDeviceContainer devs1_587 = link_helper1.Install(regswtches.Get(2), regswtches.Get(9));
    NetDeviceContainer devs1_588 = link_helper1.Install(regswtches.Get(2), regswtches.Get(9));
    NetDeviceContainer devs1_589 = link_helper1.Install(regswtches.Get(2), regswtches.Get(9));
    NetDeviceContainer devs1_590 = link_helper1.Install(regswtches.Get(2), regswtches.Get(9));
    NetDeviceContainer devs1_591 = link_helper1.Install(regswtches.Get(2), regswtches.Get(9));
    NetDeviceContainer devs1_592 = link_helper1.Install(regswtches.Get(2), regswtches.Get(10));
    NetDeviceContainer devs1_593 = link_helper1.Install(regswtches.Get(2), regswtches.Get(10));
    NetDeviceContainer devs1_594 = link_helper1.Install(regswtches.Get(2), regswtches.Get(10));
    NetDeviceContainer devs1_595 = link_helper1.Install(regswtches.Get(2), regswtches.Get(10));
    NetDeviceContainer devs1_596 = link_helper1.Install(regswtches.Get(2), regswtches.Get(10));
    NetDeviceContainer devs1_597 = link_helper1.Install(regswtches.Get(2), regswtches.Get(10));
    NetDeviceContainer devs1_598 = link_helper1.Install(regswtches.Get(2), regswtches.Get(10));
    NetDeviceContainer devs1_599 = link_helper1.Install(regswtches.Get(2), regswtches.Get(10));
    NetDeviceContainer devs1_600 = link_helper1.Install(regswtches.Get(2), regswtches.Get(11));
    NetDeviceContainer devs1_601 = link_helper1.Install(regswtches.Get(2), regswtches.Get(11));
    NetDeviceContainer devs1_602 = link_helper1.Install(regswtches.Get(2), regswtches.Get(11));
    NetDeviceContainer devs1_603 = link_helper1.Install(regswtches.Get(2), regswtches.Get(11));
    NetDeviceContainer devs1_604 = link_helper1.Install(regswtches.Get(2), regswtches.Get(11));
    NetDeviceContainer devs1_605 = link_helper1.Install(regswtches.Get(2), regswtches.Get(11));
    NetDeviceContainer devs1_606 = link_helper1.Install(regswtches.Get(2), regswtches.Get(11));
    NetDeviceContainer devs1_607 = link_helper1.Install(regswtches.Get(2), regswtches.Get(11));
    NetDeviceContainer devs1_608 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_609 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_610 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_611 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_612 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_613 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_614 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_615 = link_helper1.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs1_616 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_617 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_618 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_619 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_620 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_621 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_622 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_623 = link_helper1.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs1_624 = link_helper1.Install(regswtches.Get(3), regswtches.Get(10));
    NetDeviceContainer devs1_625 = link_helper1.Install(regswtches.Get(3), regswtches.Get(10));
    NetDeviceContainer devs1_626 = link_helper1.Install(regswtches.Get(3), regswtches.Get(10));
    NetDeviceContainer devs1_627 = link_helper1.Install(regswtches.Get(3), regswtches.Get(10));
    NetDeviceContainer devs1_628 = link_helper1.Install(regswtches.Get(3), regswtches.Get(10));
    NetDeviceContainer devs1_629 = link_helper1.Install(regswtches.Get(3), regswtches.Get(10));
    NetDeviceContainer devs1_630 = link_helper1.Install(regswtches.Get(3), regswtches.Get(10));
    NetDeviceContainer devs1_631 = link_helper1.Install(regswtches.Get(3), regswtches.Get(10));
    NetDeviceContainer devs1_632 = link_helper1.Install(regswtches.Get(3), regswtches.Get(11));
    NetDeviceContainer devs1_633 = link_helper1.Install(regswtches.Get(3), regswtches.Get(11));
    NetDeviceContainer devs1_634 = link_helper1.Install(regswtches.Get(3), regswtches.Get(11));
    NetDeviceContainer devs1_635 = link_helper1.Install(regswtches.Get(3), regswtches.Get(11));
    NetDeviceContainer devs1_636 = link_helper1.Install(regswtches.Get(3), regswtches.Get(11));
    NetDeviceContainer devs1_637 = link_helper1.Install(regswtches.Get(3), regswtches.Get(11));
    NetDeviceContainer devs1_638 = link_helper1.Install(regswtches.Get(3), regswtches.Get(11));
    NetDeviceContainer devs1_639 = link_helper1.Install(regswtches.Get(3), regswtches.Get(11));
    NetDeviceContainer devs1_640 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_641 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_642 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_643 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_644 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_645 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_646 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_647 = link_helper1.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs1_648 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_649 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_650 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_651 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_652 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_653 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_654 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_655 = link_helper1.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs1_656 = link_helper1.Install(regswtches.Get(4), regswtches.Get(10));
    NetDeviceContainer devs1_657 = link_helper1.Install(regswtches.Get(4), regswtches.Get(10));
    NetDeviceContainer devs1_658 = link_helper1.Install(regswtches.Get(4), regswtches.Get(10));
    NetDeviceContainer devs1_659 = link_helper1.Install(regswtches.Get(4), regswtches.Get(10));
    NetDeviceContainer devs1_660 = link_helper1.Install(regswtches.Get(4), regswtches.Get(10));
    NetDeviceContainer devs1_661 = link_helper1.Install(regswtches.Get(4), regswtches.Get(10));
    NetDeviceContainer devs1_662 = link_helper1.Install(regswtches.Get(4), regswtches.Get(10));
    NetDeviceContainer devs1_663 = link_helper1.Install(regswtches.Get(4), regswtches.Get(10));
    NetDeviceContainer devs1_664 = link_helper1.Install(regswtches.Get(4), regswtches.Get(11));
    NetDeviceContainer devs1_665 = link_helper1.Install(regswtches.Get(4), regswtches.Get(11));
    NetDeviceContainer devs1_666 = link_helper1.Install(regswtches.Get(4), regswtches.Get(11));
    NetDeviceContainer devs1_667 = link_helper1.Install(regswtches.Get(4), regswtches.Get(11));
    NetDeviceContainer devs1_668 = link_helper1.Install(regswtches.Get(4), regswtches.Get(11));
    NetDeviceContainer devs1_669 = link_helper1.Install(regswtches.Get(4), regswtches.Get(11));
    NetDeviceContainer devs1_670 = link_helper1.Install(regswtches.Get(4), regswtches.Get(11));
    NetDeviceContainer devs1_671 = link_helper1.Install(regswtches.Get(4), regswtches.Get(11));
    NetDeviceContainer devs1_672 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_673 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_674 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_675 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_676 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_677 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_678 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_679 = link_helper1.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs1_680 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_681 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_682 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_683 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_684 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_685 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_686 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_687 = link_helper1.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs1_688 = link_helper1.Install(regswtches.Get(5), regswtches.Get(10));
    NetDeviceContainer devs1_689 = link_helper1.Install(regswtches.Get(5), regswtches.Get(10));
    NetDeviceContainer devs1_690 = link_helper1.Install(regswtches.Get(5), regswtches.Get(10));
    NetDeviceContainer devs1_691 = link_helper1.Install(regswtches.Get(5), regswtches.Get(10));
    NetDeviceContainer devs1_692 = link_helper1.Install(regswtches.Get(5), regswtches.Get(10));
    NetDeviceContainer devs1_693 = link_helper1.Install(regswtches.Get(5), regswtches.Get(10));
    NetDeviceContainer devs1_694 = link_helper1.Install(regswtches.Get(5), regswtches.Get(10));
    NetDeviceContainer devs1_695 = link_helper1.Install(regswtches.Get(5), regswtches.Get(10));
    NetDeviceContainer devs1_696 = link_helper1.Install(regswtches.Get(5), regswtches.Get(11));
    NetDeviceContainer devs1_697 = link_helper1.Install(regswtches.Get(5), regswtches.Get(11));
    NetDeviceContainer devs1_698 = link_helper1.Install(regswtches.Get(5), regswtches.Get(11));
    NetDeviceContainer devs1_699 = link_helper1.Install(regswtches.Get(5), regswtches.Get(11));
    NetDeviceContainer devs1_700 = link_helper1.Install(regswtches.Get(5), regswtches.Get(11));
    NetDeviceContainer devs1_701 = link_helper1.Install(regswtches.Get(5), regswtches.Get(11));
    NetDeviceContainer devs1_702 = link_helper1.Install(regswtches.Get(5), regswtches.Get(11));
    NetDeviceContainer devs1_703 = link_helper1.Install(regswtches.Get(5), regswtches.Get(11));
    NetDeviceContainer devs1_704 = link_helper1.Install(regswtches.Get(6), regswtches.Get(8));
    NetDeviceContainer devs1_705 = link_helper1.Install(regswtches.Get(6), regswtches.Get(8));
    NetDeviceContainer devs1_706 = link_helper1.Install(regswtches.Get(6), regswtches.Get(8));
    NetDeviceContainer devs1_707 = link_helper1.Install(regswtches.Get(6), regswtches.Get(8));
    NetDeviceContainer devs1_708 = link_helper1.Install(regswtches.Get(6), regswtches.Get(8));
    NetDeviceContainer devs1_709 = link_helper1.Install(regswtches.Get(6), regswtches.Get(8));
    NetDeviceContainer devs1_710 = link_helper1.Install(regswtches.Get(6), regswtches.Get(8));
    NetDeviceContainer devs1_711 = link_helper1.Install(regswtches.Get(6), regswtches.Get(8));
    NetDeviceContainer devs1_712 = link_helper1.Install(regswtches.Get(6), regswtches.Get(9));
    NetDeviceContainer devs1_713 = link_helper1.Install(regswtches.Get(6), regswtches.Get(9));
    NetDeviceContainer devs1_714 = link_helper1.Install(regswtches.Get(6), regswtches.Get(9));
    NetDeviceContainer devs1_715 = link_helper1.Install(regswtches.Get(6), regswtches.Get(9));
    NetDeviceContainer devs1_716 = link_helper1.Install(regswtches.Get(6), regswtches.Get(9));
    NetDeviceContainer devs1_717 = link_helper1.Install(regswtches.Get(6), regswtches.Get(9));
    NetDeviceContainer devs1_718 = link_helper1.Install(regswtches.Get(6), regswtches.Get(9));
    NetDeviceContainer devs1_719 = link_helper1.Install(regswtches.Get(6), regswtches.Get(9));
    NetDeviceContainer devs1_720 = link_helper1.Install(regswtches.Get(6), regswtches.Get(10));
    NetDeviceContainer devs1_721 = link_helper1.Install(regswtches.Get(6), regswtches.Get(10));
    NetDeviceContainer devs1_722 = link_helper1.Install(regswtches.Get(6), regswtches.Get(10));
    NetDeviceContainer devs1_723 = link_helper1.Install(regswtches.Get(6), regswtches.Get(10));
    NetDeviceContainer devs1_724 = link_helper1.Install(regswtches.Get(6), regswtches.Get(10));
    NetDeviceContainer devs1_725 = link_helper1.Install(regswtches.Get(6), regswtches.Get(10));
    NetDeviceContainer devs1_726 = link_helper1.Install(regswtches.Get(6), regswtches.Get(10));
    NetDeviceContainer devs1_727 = link_helper1.Install(regswtches.Get(6), regswtches.Get(10));
    NetDeviceContainer devs1_728 = link_helper1.Install(regswtches.Get(6), regswtches.Get(11));
    NetDeviceContainer devs1_729 = link_helper1.Install(regswtches.Get(6), regswtches.Get(11));
    NetDeviceContainer devs1_730 = link_helper1.Install(regswtches.Get(6), regswtches.Get(11));
    NetDeviceContainer devs1_731 = link_helper1.Install(regswtches.Get(6), regswtches.Get(11));
    NetDeviceContainer devs1_732 = link_helper1.Install(regswtches.Get(6), regswtches.Get(11));
    NetDeviceContainer devs1_733 = link_helper1.Install(regswtches.Get(6), regswtches.Get(11));
    NetDeviceContainer devs1_734 = link_helper1.Install(regswtches.Get(6), regswtches.Get(11));
    NetDeviceContainer devs1_735 = link_helper1.Install(regswtches.Get(6), regswtches.Get(11));
    NetDeviceContainer devs1_736 = link_helper1.Install(regswtches.Get(7), regswtches.Get(8));
    NetDeviceContainer devs1_737 = link_helper1.Install(regswtches.Get(7), regswtches.Get(8));
    NetDeviceContainer devs1_738 = link_helper1.Install(regswtches.Get(7), regswtches.Get(8));
    NetDeviceContainer devs1_739 = link_helper1.Install(regswtches.Get(7), regswtches.Get(8));
    NetDeviceContainer devs1_740 = link_helper1.Install(regswtches.Get(7), regswtches.Get(8));
    NetDeviceContainer devs1_741 = link_helper1.Install(regswtches.Get(7), regswtches.Get(8));
    NetDeviceContainer devs1_742 = link_helper1.Install(regswtches.Get(7), regswtches.Get(8));
    NetDeviceContainer devs1_743 = link_helper1.Install(regswtches.Get(7), regswtches.Get(8));
    NetDeviceContainer devs1_744 = link_helper1.Install(regswtches.Get(7), regswtches.Get(9));
    NetDeviceContainer devs1_745 = link_helper1.Install(regswtches.Get(7), regswtches.Get(9));
    NetDeviceContainer devs1_746 = link_helper1.Install(regswtches.Get(7), regswtches.Get(9));
    NetDeviceContainer devs1_747 = link_helper1.Install(regswtches.Get(7), regswtches.Get(9));
    NetDeviceContainer devs1_748 = link_helper1.Install(regswtches.Get(7), regswtches.Get(9));
    NetDeviceContainer devs1_749 = link_helper1.Install(regswtches.Get(7), regswtches.Get(9));
    NetDeviceContainer devs1_750 = link_helper1.Install(regswtches.Get(7), regswtches.Get(9));
    NetDeviceContainer devs1_751 = link_helper1.Install(regswtches.Get(7), regswtches.Get(9));
    NetDeviceContainer devs1_752 = link_helper1.Install(regswtches.Get(7), regswtches.Get(10));
    NetDeviceContainer devs1_753 = link_helper1.Install(regswtches.Get(7), regswtches.Get(10));
    NetDeviceContainer devs1_754 = link_helper1.Install(regswtches.Get(7), regswtches.Get(10));
    NetDeviceContainer devs1_755 = link_helper1.Install(regswtches.Get(7), regswtches.Get(10));
    NetDeviceContainer devs1_756 = link_helper1.Install(regswtches.Get(7), regswtches.Get(10));
    NetDeviceContainer devs1_757 = link_helper1.Install(regswtches.Get(7), regswtches.Get(10));
    NetDeviceContainer devs1_758 = link_helper1.Install(regswtches.Get(7), regswtches.Get(10));
    NetDeviceContainer devs1_759 = link_helper1.Install(regswtches.Get(7), regswtches.Get(10));
    NetDeviceContainer devs1_760 = link_helper1.Install(regswtches.Get(7), regswtches.Get(11));
    NetDeviceContainer devs1_761 = link_helper1.Install(regswtches.Get(7), regswtches.Get(11));
    NetDeviceContainer devs1_762 = link_helper1.Install(regswtches.Get(7), regswtches.Get(11));
    NetDeviceContainer devs1_763 = link_helper1.Install(regswtches.Get(7), regswtches.Get(11));
    NetDeviceContainer devs1_764 = link_helper1.Install(regswtches.Get(7), regswtches.Get(11));
    NetDeviceContainer devs1_765 = link_helper1.Install(regswtches.Get(7), regswtches.Get(11));
    NetDeviceContainer devs1_766 = link_helper1.Install(regswtches.Get(7), regswtches.Get(11));
    NetDeviceContainer devs1_767 = link_helper1.Install(regswtches.Get(7), regswtches.Get(11));
    Config::SetDefault("ns3::RdmaHw::CcMode", UintegerValue(12));
    Config::SetDefault("ns3::RdmaHw::RateTargeting", BooleanValue(rateTargeting));
    Config::SetDefault("ns3::RdmaHw::L2AckInterval", UintegerValue(l2AckInterval));
    Config::SetDefault("ns3::RdmaHw::L2ChunkSize", UintegerValue(4000));
    Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(4096));
    Config::SetDefault("ns3::RdmaHw::MaxMsgsInFlight", UintegerValue(maxMsgsInFlight));

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);

    // Algorithm + per-flow forwarding table. The switch JSON's switch_id_map (0..11 -> TE-CCL
    // ids) matches the regswtches declaration order above (0-7 = leaf0..leaf7; 8-11 = spine0..spine3); there is one
    // JSON per collective, shared by the rate and _no_rate XMLs since routing is identical.
    const std::string XML_NAME = xmlName.empty()
        ? "rail_optimized_256gpu_" + coll + (rate ? "" : "_no_rate") + ".xml"
        : xmlName;
    std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(),
                                                  "../../scratch/xml_input/" + XML_NAME);
    std::string SWITCH_JSON = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(),
                                                      "../../scratch/json_input/rail_optimized_256gpu_" + coll + ".json");

    // All output files go to simulation/scratch/logs. FindSelfDirectory() resolves to
    // simulation/build/scratch, so "../../scratch/logs" hops back up to the source tree.
    const std::string LOG_DIR = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/logs");
    ns3::SystemPath::MakeDirectories(LOG_DIR); // no-op if it already exists

    const std::string LOG_FILE = ns3::SystemPath::Append(LOG_DIR, label + ".txt");

    constexpr DataType::Type dtype = DataType::INT32;
    const uint64_t INPUT_BYTES = inputBytes;
    bool CORRECTNESS_CHECK = false;

    AlgoTopology topo(gpunodes, regswtches);
    AlgoParseResult result = topo.ParseAlgoXml(XML_ALGO.c_str());
    // Fatal, not a log line: a failed parse leaves the topology empty and the failure would
    // otherwise surface far downstream (zero input chunks) with NS_LOG_ERROR off by default.
    if (result != AlgoParseResult::ALGO_PARSE_SUCCESS)
        NS_FATAL_ERROR("Encountered issue in parsing XML algorithm " << XML_ALGO << ", error code " << result);
    // The JSON feeds two independent consumers, so it is parsed whenever EITHER wants it, with
    // each effect switched on separately. --flowId=0 --nicSel=schedule is a real configuration:
    // plain ECMP switches, but each connection still injected on the plane the schedule chose.
    const bool pinNics = (nicSel == "schedule");
    if (flowId || pinNics) {
        AlgoParseResult switchResult = topo.ParseSwitchJson(SWITCH_JSON.c_str(), flowId, pinNics);
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
    app_helper.SetAttribute("ProtoChunkBytes", UintegerValue(protoChunkBytes));
    app_helper.SetAttribute("NicSelection", StringValue(
        nicSel == "schedule" ? "SCHEDULED" : (nicSel == "merged" ? "MERGED" : "ROUND_ROBIN")));
    app_helper.SetAttribute("NetworkFlowIds", BooleanValue(flowId));
    app_helper.SetAttribute("HonorNetDeps", BooleanValue(netDeps));
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
    // node_id: GPUs are the first 256 ids, then the 12 regswtches (leaves/spines); drops carry
    // a size, PFC pause/resume leave bytes/q_id blank.
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

    // Per-port peak egress occupancy, always written. It is the whole of the queue record when
    // --qlenRows=0, and a cheap cross-check of the row trace when it is on.
    std::string qmaxPath = ns3::SystemPath::Append(LOG_DIR, "switch_qlen_max_" + label + ".csv");
    if (FILE* qmaxOut = fopen(qmaxPath.c_str(), "w")) {
        fprintf(qmaxOut, "sw_id,port_id,q_id,max_qlen_bytes\n");
        for (const auto& kv : g_qMax) {
            fprintf(qmaxOut, "%u,%u,%u,%ld\n", std::get<0>(kv.first), std::get<1>(kv.first),
                    std::get<2>(kv.first), kv.second);
        }
        fclose(qmaxOut);
    }
    if (nicOut) fclose(nicOut);
    std::cout << "NIC selection: " << (nicSel == "schedule" ? "schedule-pinned (one qp per connection)"
        : (nicSel == "merged" ? "merged NIC (one qp per NIC, message split across them)"
                              : "round-robin (one qp per connection)")) << std::endl;
    std::cout << "Network flow ids: " << (flowId ? "on (custom headers + per-flow switch forwarding)"
                                                 : "off (no header on the wire, plain ECMP)") << std::endl;
    std::cout << "Network deps (netdepid/netdeps): " << (netDeps ? "honored" : "skipped") << std::endl;
    std::cout << "Algorithm XML: " << XML_ALGO << std::endl;
    std::cout << "Switch queue trace: " << (qlenRows ? qlenPath : std::string("(rows off)")) << std::endl;
    std::cout << "Switch peak-queue summary: " << qmaxPath << std::endl;
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
