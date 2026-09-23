// Shared harness for the three mini_* benchmark scratches (mini_1gpu_1nic, mini_2gpu_1nic,
// mini_2gpu_2nic). Everything here is the part of dual_plane_hetero.cc /
// rail_optimized_256gpu_dual_plane.cc that has nothing to do with the topology: the command
// line, the RdmaHw defaults, the algorithm/switch-JSON parse, the app install, the
// congestion and per-NIC traces, and the end-of-run report. The three scratches differ only
// in their node/link list (generated from the DSL) and in which schedule they name, so
// copying ~450 lines of identical harness three times would only invite the copies to drift.
//
// This is a header, not a module: ns3's scratch CMakeLists globs *.cc and builds one target
// per file, so a .h here is picked up by whoever includes it and by nothing else. Each
// target therefore gets exactly one copy of the statics below -- no ODR problem, and no
// `inline` gymnastics needed.
//
// Usage from a scratch (see mini_1gpu_1nic.cc):
//     mini::Options opt = mini::Options::For("mini_1g1n", 4);
//     CommandLine cmd; opt.AddTo(cmd); cmd.Parse(argc, argv); opt.Validate();
//     ... build gpunodes / regswtches / nvswtches ...   (QcnEnabled default first)
//     opt.SetRdmaDefaults();
//     RdmaFabricHelper().Build(gpunodes, regswtches, nvswtches);
//     return mini::Run(opt, gpunodes, regswtches, nvswtches);

#ifndef MINI_HARNESS_H
#define MINI_HARNESS_H

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
#include "ns3/custom-header.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <algorithm>
#include <utility>

namespace mini {

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
// the top of an input sweep the row-by-row CSV runs to many GB, so --qlenRows=0 turns the
// rows off and leaves only this summary.
static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int64_t> g_qMax;
static bool g_qlenRows = true;

// Per-port egress accounting, keyed (switch id, port ifIndex), fed from the same dequeue hook.
// SwitchNode::m_txBytes already counts bytes per port, but it lumps ACK/NACK/CNP/PFC in with
// the RDMA data, and it's the data split across a leaf's uplinks that says whether ECMP or the
// flow-id rules balanced the load. first/last are the dequeue times of the port's first and
// last data packet, which gives each port its own busy window and gives the run an end time
// independent of the NIC sampler's interval-rounded Simulator::Now().
struct PortStat {
    uint64_t dataBytes = 0;
    uint64_t ctrlBytes = 0;
    uint64_t dataPkts = 0;
    int64_t firstNs = -1;
    int64_t lastNs = -1;
};
static std::map<std::pair<uint32_t, uint32_t>, PortStat> g_portStats;

// What each switch port is attached to. Filled once when the traces are connected.
struct PortInfo {
    uint32_t peerId;
    bool peerIsSwitch;
    uint64_t rateBps;
};
static std::map<std::pair<uint32_t, uint32_t>, PortInfo> g_portInfo;

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
    const int64_t now = Simulator::Now().GetNanoSeconds();
    PortStat& ps = g_portStats[std::make_pair(swId, port)];
    // Split by protocol, not queue: with SwitchNode::AckHighPrio at its default of 0, ACK and
    // NACK share the data queue, so queue 0 alone would count them as data. RDMA data is UDP.
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header);
    p->PeekHeader(ch);
    if (ch.l3Prot != 0x11) {
        ps.ctrlBytes += p->GetSize();
    } else {
        ps.dataBytes += p->GetSize();
        ++ps.dataPkts;
        if (ps.firstNs < 0) ps.firstNs = now;
        ps.lastNs = now;
    }
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
// On these three topologies it is the measurement that separates the variants. mini_2gpu_1nic
// funnels two GPUs through one host port and mini_2gpu_2nic gives each GPU its own; an
// aggregate byte count cannot tell "the funnel was saturated" from "both ports were busy and
// the limit was in the fabric", and only a time-resolved trace can.
struct NicProbe {
    Ptr<RdmaHw> hw;
    uint32_t nodeId;
    uint32_t port;
    const char* kind;   // "nvlink" (peer is an NVSwitch or a peer GPU) or "fabric" (a leaf)
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

// ---- options -----------------------------------------------------------------------------

struct Options {
    // Set by the scratch, not by the command line: which schedule family this topology's
    // inputs are named after, e.g. "mini_1g1n" -> xml_input/mini_1g1n_<coll>.xml and
    // json_input/mini_1g1n_<coll>.json.
    std::string stem;
    // GPUs in the topology, used only in the banner and to size the default log label.
    uint32_t ranks = 0;

    uint64_t inputBytes = (1ull << 20);
    // label distinguishes output files between runs, e.g. --label=with_rate vs --label=no_rate
    std::string label;
    // Collective to run: allgather | alltoall. Spelled the same way as in the big scratches
    // (and as sweep_dual_plane_features.py passes it) so the sweep driver needs no special
    // case, even though the TE-CCL files themselves are named with the short suffix that
    // CollSuffix() maps this to.
    std::string coll = "alltoall";
    // `rate` picks the rate-annotated schedule vs a "_no_rate" ablation of the same solve.
    bool rate = true;
    // Make the per-flow XML "rate" a true target (accumulating token-bucket shaper) rather
    // than just an upper bound, so a flow paced below line rate actually runs at its rate.
    bool rateTargeting = true;
    // Honor the XML netdepid/netdeps wire-ordering dependences. These are what pace a
    // time-indexed (TE-CCL) solve; off releases every buffer-ready send at once. Safe to
    // ablate -- buffer readiness is still enforced by depid/deps.
    bool netDeps = true;
    // Network-side only: put the schedule's flow id on the wire and install the per-flow
    // forwarding table from the JSON, so switches route by it instead of hashing ECMP. When
    // false the header is not merely ignored, it is never added, so neither arm carries its 4
    // bytes per packet. On these topologies this is the load-bearing knob: the 2:1 leaf taper
    // exists precisely so that spreading a leaf's traffic over BOTH spines is necessary, and
    // ECMP hashing has no reason to split it the way the solve did.
    bool flowId = true;
    // How each connection picks its NIC, independent of --flowId:
    //   schedule -> the NIC the switch JSON dictates (one qp per connection, pinned)
    //   merged   -> NCCL_IB_MERGE_NICS: one qp per NIC, every message split across them
    //   rr       -> one qp per connection, NICs handed out round-robin
    // Note that "schedule" can only bind where a GPU is multi-homed onto the fabric, which of
    // these three is mini_2gpu_2nic alone -- and even there both of a host's NICs reach the
    // same leaf, so all three settings pick an equivalent port. On mini_2gpu_1nic the GPUs do
    // not touch the fabric at all (the one NIC hangs off the root complex), so the JSON's
    // ingress-hop lookup finds nothing and ParseSwitchJson reports 0 connections pinned. That
    // is expected here, not a failure: there is no NIC choice to make.
    std::string nicSel = "schedule";
    // Overrides the derived XML filename (not the path) so an alternative schedule for the
    // same collective can be run without touching the scratch. Empty = derive from stem/coll.
    std::string xmlName = "";
    // A schedule VARIANT of the same topology and collective: solved differently, but against
    // the same fabric, so it is selected by suffixing both input stems rather than by naming
    // files. `--sched=milp` reads xml_input/<stem>_<coll>_milp[_no_rate].xml alongside
    // json_input/<stem>_<coll>_milp.json. Empty = the topology's own default solve.
    //
    // This is what makes a "baseline baseline" possible: the mini_1g1n milp solve fixes
    // strictly one chunk per (src, dst) GPU pair, so it takes exactly one path per pair and
    // makes no multipath decision at all. Run it with every dynamism knob off
    // (--rate=0 --netDeps=0 --flowId=0 --nicSel=merged) and what is left is direct,
    // ECMP-forwarded, unpaced point-to-point traffic -- the floor the ablations are measured
    // against. Note the variant may carry a different nchunksperloop than the default solve
    // (4 vs 8 on mini_1g1n); --inputBytes is a rank's whole input either way, so the bytes
    // per GPU pair -- and hence the comparison -- are unchanged.
    std::string sched = "";
    // Period of the per-NIC bandwidth trace, in ns. 0 disables it and costs nothing.
    uint32_t nicBwIntervalNs = 0;
    // The per-packet queue trace is exact but grows with the traffic: one row per enqueue and
    // one per dequeue at every hop. 0 leaves only switch_qlen_max_<label>.csv (the per-port
    // high-water marks, which are tracked either way).
    bool qlenRows = true;
    std::string checkLog = "minimal"; // silent | minimal | verbose
    uint32_t maxMismatches = 10;
    // Run the collective tester. Cheap at these sizes -- 4 or 8 ranks -- which is the point of
    // having mini topologies at all, so unlike the big scratches it defaults ON here.
    bool correctness = true;
    // Algorithm pipelining granularity (the MSCCL kernel's gridOffset loop); 0 disables it.
    // Compared against the size of ONE chunk (inputBytes / nchunksperloop), so at small
    // --inputBytes any sane value leaves the gridOffset and maxAllowedCount loops inert.
    uint32_t protoChunkBytes = 0;
    // Transport pipelining depth (NCCL_STEPS analogue): how many messages a qp may have in
    // flight before waiting for a completion. It binds only when --l2Ack is nonzero: with acks
    // off the sender self-acknowledges at send completion and messages retire without a round
    // trip, so no depth of in-flight messages is ever reached.
    uint32_t maxMsgsInFlight = 8;
    // Acknowledgement mode for RdmaHw::L2AckInterval: 0 is no-ack, where the sender infers
    // completion from its own send completion and nothing waits a round trip; nonzero turns
    // acks on. Not a cosmetic knob: it changes the transport under every other setting here,
    // so runs that differ in it are not comparable with each other.
    uint32_t l2AckInterval = 1;
    // Mid-message ack coalescing, in packets (RdmaHw::AckEveryNPackets). Packets closing a
    // message are always acknowledged regardless; the counter exists only to keep the sender's
    // unacknowledged bytes inside its BDP window, since snd_una advances on acks and
    // RdmaQueuePair::IsWinBound gates on snd_nxt - snd_una.
    //
    // Sized against that window. A fabric hop here is 700 ns at 100 Gbps, so a two-hop round
    // trip is 2*(2*700) + 2*327.7 = 3455 ns and the window is 100e9 * 3455e-9 / 8 = 43 KB,
    // ~10 packets at a 4096 B MTU. 8 sits just inside that, so the sawtooth never touches the
    // window edge and the fabric, not the ack cadence, is what limits the run.
    uint32_t ackEveryNPkts = 8;

    static Options For(const std::string& stem, uint32_t ranks) {
        Options o;
        o.stem = stem;
        o.ranks = ranks;
        o.label = stem;
        return o;
    }

    void AddTo(CommandLine& cmd) {
        cmd.AddValue("inputBytes", "Total input size in bytes (one rank's whole input)", inputBytes);
        cmd.AddValue("label", "Suffix for the congestion-monitor output CSVs", label);
        cmd.AddValue("coll", "Collective to run: allgather | alltoall", coll);
        cmd.AddValue("rate", "Use the rate-annotated XML (false = the _no_rate ablation)", rate);
        cmd.AddValue("rateTargeting", "Treat per-flow XML rates as targets, not just caps", rateTargeting);
        cmd.AddValue("flowId", "Network only: carry msccl flow ids and install per-flow switch forwarding from the JSON (does not affect NIC selection)", flowId);
        cmd.AddValue("xml", "XML schedule filename inside scratch/xml_input, overriding the one derived from --coll/--rate (empty = derive)", xmlName);
        cmd.AddValue("sched", "Schedule variant suffix applied to BOTH input stems, e.g. milp -> <stem>_<coll>_milp[_no_rate].xml and <stem>_<coll>_milp.json (empty = the topology's default solve)", sched);
        cmd.AddValue("nicSel", "NIC selection: schedule (switch JSON pins the NIC) | merged (NCCL-style merged NIC, one qp per NIC) | rr (one qp per connection, round-robin NICs)", nicSel);
        cmd.AddValue("netDeps", "Honor the XML netdepid/netdeps network dependences (false = release every buffer-ready send immediately)", netDeps);
        cmd.AddValue("qlenRows", "Write the per-packet switch queue trace (0 = only the per-port peak summary)", qlenRows);
        cmd.AddValue("nicBwInterval", "Sample every GPU NIC's transmitted bytes this often, in ns (0 = off)", nicBwIntervalNs);
        cmd.AddValue("checkLog", "Correctness-check logging: silent | minimal | verbose", checkLog);
        cmd.AddValue("maxMismatches", "Mismatch lines to print before giving up (minimal mode)", maxMismatches);
        cmd.AddValue("correctness", "Run the collective correctness check (cheap at these sizes, so on by default)", correctness);
        cmd.AddValue("protoChunkBytes", "Pipelining granularity in bytes; 0 disables pipelining", protoChunkBytes);
        cmd.AddValue("maxMsgsInFlight", "Messages a qp may have in flight at once", maxMsgsInFlight);
        cmd.AddValue("l2Ack", "Ack mode: 0 = no-ack (sender self-acknowledges at send completion), nonzero = acks on", l2AckInterval);
        cmd.AddValue("ackEveryNPkts", "Mid-message ack coalescing in packets; message-closing packets are acked regardless (0 = only those)", ackEveryNPkts);
    }

    // TE-CCL names its outputs with a short collective tag; the scratches and the sweep
    // driver speak the long name. One place to translate, so neither side has to know about
    // the other's spelling.
    std::string CollSuffix() const { return coll == "allgather" ? "ag" : "a2a"; }
    bool IsAllgather() const { return coll == "allgather"; }

    void Validate() {
        if (nicSel != "schedule" && nicSel != "merged" && nicSel != "rr")
            NS_FATAL_ERROR("Unknown --nicSel value '" << nicSel << "' (expected schedule|merged|rr).");
        if (coll != "allgather" && coll != "alltoall")
            NS_FATAL_ERROR("Unknown --coll value '" << coll << "' (expected allgather|alltoall).");
        if (checkLog != "silent" && checkLog != "minimal" && checkLog != "verbose")
            NS_FATAL_ERROR("Unknown --checkLog value '" << checkLog << "' (expected silent|minimal|verbose).");
        g_qlenRows = qlenRows;
    }

    // Must run after the links are installed (so the helpers' own Mtu/DataRate are already
    // fixed) and before RdmaFabricHelper::Build, which is what instantiates RdmaHw.
    void SetRdmaDefaults() const {
        Config::SetDefault("ns3::RdmaHw::CcMode", UintegerValue(12));
        Config::SetDefault("ns3::RdmaHw::RateTargeting", BooleanValue(rateTargeting));
        Config::SetDefault("ns3::RdmaHw::L2AckInterval", UintegerValue(l2AckInterval));
        Config::SetDefault("ns3::RdmaHw::AckEveryNPackets", UintegerValue(ackEveryNPkts));
        // Go-back-N rewind quantum, meaningful only under L2BackToZero, which is off.
        Config::SetDefault("ns3::RdmaHw::L2BackToZero", BooleanValue(false));
        Config::SetDefault("ns3::RdmaHw::L2ChunkSize", UintegerValue(0));
        // Stated rather than inherited: with CcMode 12 matching no congestion-control branch
        // in RdmaHw, the per-peer window stays at the BDP RdmaFabricHelper computed, and
        // pacing comes entirely from the schedule's own rates.
        Config::SetDefault("ns3::RdmaHw::VarWin", BooleanValue(false));
        Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(4096));
        Config::SetDefault("ns3::RdmaHw::MaxMsgsInFlight", UintegerValue(maxMsgsInFlight));
    }
};

// ---- the run ------------------------------------------------------------------------------

static int Run(const Options& opt, NodeContainer gpunodes, NodeContainer regswtches,
               NodeContainer nvswtches) {
    // The switch JSON's switch_id_map (0..N -> TE-CCL ids) matches the regswtches declaration
    // order in each scratch, which is the order the DSL emitted; one JSON per schedule, shared
    // by the rate and _no_rate XMLs since routing is identical between them.
    // --sched names a variant solve of the same topology/collective and suffixes both stems;
    // an explicit --xml still overrides the XML half, so the two can be combined.
    const std::string STEM = opt.stem + "_" + opt.CollSuffix()
                           + (opt.sched.empty() ? "" : "_" + opt.sched);
    const std::string XML_NAME = opt.xmlName.empty()
        ? STEM + (opt.rate ? "" : "_no_rate") + ".xml"
        : opt.xmlName;
    std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(),
                                                   "../../scratch/xml_input/" + XML_NAME);
    std::string SWITCH_JSON = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(),
                                                      "../../scratch/json_input/" + STEM + ".json");

    // All output files go to simulation/scratch/logs. FindSelfDirectory() resolves to
    // simulation/build/scratch, so "../../scratch/logs" hops back up to the source tree.
    const std::string LOG_DIR = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/logs");
    ns3::SystemPath::MakeDirectories(LOG_DIR); // no-op if it already exists

    const std::string LOG_FILE = ns3::SystemPath::Append(LOG_DIR, opt.label + ".txt");

    constexpr DataType::Type dtype = DataType::INT32;
    const uint64_t INPUT_BYTES = opt.inputBytes;

    AlgoTopology topo(gpunodes, regswtches);
    AlgoParseResult result = topo.ParseAlgoXml(XML_ALGO.c_str());
    // Fatal, not a log line: a failed parse leaves the topology empty and the failure would
    // otherwise surface far downstream (zero input chunks) with NS_LOG_ERROR off by default.
    if (result != AlgoParseResult::ALGO_PARSE_SUCCESS)
        NS_FATAL_ERROR("Encountered issue in parsing XML algorithm " << XML_ALGO << ", error code " << result);
    // The JSON feeds two independent consumers, so it is parsed whenever EITHER wants it, with
    // each effect switched on separately. --flowId=0 --nicSel=schedule is a real configuration:
    // plain ECMP switches, but each connection still injected on the NIC the schedule chose.
    const bool pinNics = (opt.nicSel == "schedule");
    if (opt.flowId || pinNics) {
        AlgoParseResult switchResult = topo.ParseSwitchJson(SWITCH_JSON.c_str(), opt.flowId, pinNics);
        // Fatal for the same reason the XML parse above is: ParseSwitchJson returns on the
        // first bad entry, leaving flow forwarding half-installed (CustomFlowForwarding on,
        // table mostly empty), and the run then silently falls back to ECMP everywhere.
        if (switchResult != AlgoParseResult::ALGO_PARSE_SUCCESS)
            NS_FATAL_ERROR("Encountered issue in parsing switch JSON " << SWITCH_JSON << ", error code " << switchResult);
    }

    static std::ofstream logtxt;
    logtxt.open(LOG_FILE);
    if (!logtxt.is_open()) NS_FATAL_ERROR("Failed to open " << LOG_FILE);
    chmod(LOG_FILE.c_str(), 0666);

    // Chunk count and participant set come straight from the parsed algorithm, so ChunkSize
    // and the tester can never drift from the XML, and swapping XMLs needs no source edit.
    const int N_CHUNKS = topo.GetNInputChunks();
    const int N_NODES = (int) topo.GetActiveGpuIds().size();
    // Fatal rather than NS_ASSERT: these two are configuration mistakes, and an assert is
    // compiled out of an optimized build -- which is the build a sweep runs.
    if (N_CHUNKS <= 0)
        NS_FATAL_ERROR("Parsed algorithm reports zero input chunks; check " << XML_ALGO << ".");
    const int CHUNK_SIZE = (INPUT_BYTES / N_CHUNKS) / DataType::GetSizeBytes(dtype);
    // A chunk of zero elements moves no bytes and makes the whole run vacuous, which at these
    // chunk counts (8, 32, 48) is an easy mistake to make: anything below 4 bytes per chunk
    // rounds away entirely, and the run then reports a suspiciously fast time for no traffic.
    if (CHUNK_SIZE <= 0)
        NS_FATAL_ERROR("--inputBytes=" << INPUT_BYTES << " over " << N_CHUNKS
            << " chunks rounds to a zero-element chunk; raise it to at least "
            << (uint64_t) N_CHUNKS * DataType::GetSizeBytes(dtype) << " bytes.");

    // install apps
    CollectivesApplicationHelper app_helper;
    app_helper.SetAttribute("DataType", EnumValue(dtype));
    app_helper.SetAttribute("ChunkSize", UintegerValue(CHUNK_SIZE));
    app_helper.SetAttribute("CorrectnessCheck", BooleanValue(opt.correctness));
    app_helper.SetAttribute("ProtoChunkBytes", UintegerValue(opt.protoChunkBytes));
    app_helper.SetAttribute("NicSelection", StringValue(
        opt.nicSel == "schedule" ? "SCHEDULED" : (opt.nicSel == "merged" ? "MERGED" : "ROUND_ROBIN")));
    app_helper.SetAttribute("NetworkFlowIds", BooleanValue(opt.flowId));
    app_helper.SetAttribute("HonorNetDeps", BooleanValue(opt.netDeps));
    ApplicationContainer apps = app_helper.Install<GPU>(topo);

    // The ctor's `verbose` flag only seeds the log mode; SetLogMode below is what governs.
    CollectiveTester tester(apps, false, logtxt);
    CollectiveLogMode logMode = CollectiveLogMode::MINIMAL;
    if (opt.checkLog == "silent") logMode = CollectiveLogMode::SILENT;
    else if (opt.checkLog == "verbose") logMode = CollectiveLogMode::VERBOSE;
    tester.SetLogMode(logMode);
    tester.SetMaxMismatches(opt.maxMismatches);
    const bool isAllgather = opt.IsAllgather();
    if (opt.correctness) {
        if (isAllgather) tester.SetupAllgather(topo, CHUNK_SIZE * N_CHUNKS);
        else             tester.SetupAlltoall(topo, CHUNK_SIZE * N_CHUNKS);
    } else {
        NS_LOG_UNCOND("Skipping correctness check.");
    }

    // ---- congestion monitoring: event-driven switch egress queue / drop / PFC traces ----
    // Connect to the QbbNetDevice trace sources on every switch egress port. These fire
    // synchronously from within packet events, so they add no simulator events and leave the
    // reported latency/bandwidth (Simulator::Now()) untouched. Each qlen row is emitted on an
    // actual enqueue/dequeue, giving an exact, unsampled occupancy trace.
    std::string qlenPath = ns3::SystemPath::Append(LOG_DIR, "switch_qlen_" + opt.label + ".csv");
    std::string eventPath = ns3::SystemPath::Append(LOG_DIR, "switch_events_" + opt.label + ".csv");
    FILE* qlenOut = fopen(qlenPath.c_str(), "w");
    FILE* eventOut = fopen(eventPath.c_str(), "w");
    if (!qlenOut || !eventOut) NS_FATAL_ERROR("Failed to open congestion-monitor output files.");
    fprintf(qlenOut, "time_ns,sw_id,port_id,q_id,qlen_bytes,op\n");
    // node_id: GPUs are the first ids, then the regswtches (leaves/spines); drops carry a
    // size, PFC pause/resume leave bytes/q_id blank.
    fprintf(eventOut, "time_ns,node_id,port_id,q_id,bytes,op\n");

    for (uint32_t s = 0; s < regswtches.GetN(); ++s) {
        Ptr<Node> sw = regswtches.Get(s);
        uint32_t swId = sw->GetId();
        for (uint32_t d = 0; d < sw->GetNDevices(); ++d) {
            Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(sw->GetDevice(d));
            if (!dev) continue; // skip any non-Qbb (e.g. loopback) device
            uint32_t port = dev->GetIfIndex();
            if (Ptr<QbbChannel> ch = DynamicCast<QbbChannel>(dev->GetChannel())) {
                Ptr<NetDevice> other = (ch->GetDevice(0) == dev) ? ch->GetDevice(1) : ch->GetDevice(0);
                g_portInfo[std::make_pair(swId, port)] = PortInfo{
                    other->GetNode()->GetId(), (bool) DynamicCast<SwitchNode>(other->GetNode()),
                    dev->GetDataRate().GetBitRate()};
            }
            dev->TraceConnectWithoutContext("QbbEnqueue", MakeBoundCallback(&OnSwitchEnqueue, qlenOut, swId, port));
            dev->TraceConnectWithoutContext("QbbDequeue", MakeBoundCallback(&OnSwitchDequeue, qlenOut, swId, port));
            dev->TraceConnectWithoutContext("QbbDrop",    MakeBoundCallback(&OnSwitchDrop, eventOut, swId, port));
            dev->TraceConnectWithoutContext("QbbPfc",     MakeBoundCallback(&OnSwitchPfc, eventOut, swId, port));
        }
    }

    // The QbbPfc trace fires on the device that RECEIVES a PAUSE, and a switch backpressures a
    // congested ingress link by pausing the sender on the far end -- which for leaf <-> host
    // links is a NIC, not a switch. Connect the drop/PFC traces on the GPU NICs too.
    // Queue-occupancy (enqueue/dequeue) stays switch-only, since host egress is just the GPU
    // injecting and isn't the congestion of interest.
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
    if (opt.nicBwIntervalNs > 0) {
        std::string nicPath = ns3::SystemPath::Append(LOG_DIR, "host_nic_bw_" + opt.label + ".csv");
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
                // the trace stays correct if a topology's Install order ever changes. Anything
                // that is not a programmable switch is host-internal: an NVSwitch, a PCIe root
                // complex (which the DSL folds to NVSwitchNode), or a peer GPU across a
                // direct NVLink bridge.
                Ptr<QbbChannel> ch = DynamicCast<QbbChannel>(dev->GetChannel());
                if (!ch) continue;
                Ptr<NetDevice> other = (ch->GetDevice(0) == dev) ? ch->GetDevice(1) : ch->GetDevice(0);
                const char* kind = DynamicCast<SwitchNode>(other->GetNode()) ? "fabric" : "nvlink";
                nicProbes.push_back(NicProbe{drv->m_rdma, gpu->GetId(), dev->GetIfIndex(), kind, 0});
            }
        }
        Time iv = NanoSeconds(opt.nicBwIntervalNs);
        Simulator::Schedule(iv, &SampleNicBw, nicOut, &nicProbes, iv);
        std::cout << "Per-NIC bandwidth trace: " << nicPath
                  << " (every " << opt.nicBwIntervalNs << " ns, " << nicProbes.size() << " NICs)" << std::endl;
    }

    Simulator::Run();
    fclose(qlenOut);
    fclose(eventOut);

    // Per-port peak egress occupancy, always written. It is the whole of the queue record when
    // --qlenRows=0, and a cheap cross-check of the row trace when it is on.
    std::string qmaxPath = ns3::SystemPath::Append(LOG_DIR, "switch_qlen_max_" + opt.label + ".csv");
    if (FILE* qmaxOut = fopen(qmaxPath.c_str(), "w")) {
        fprintf(qmaxOut, "sw_id,port_id,q_id,max_qlen_bytes\n");
        for (const auto& kv : g_qMax) {
            fprintf(qmaxOut, "%u,%u,%u,%ld\n", std::get<0>(kv.first), std::get<1>(kv.first),
                    std::get<2>(kv.first), kv.second);
        }
        fclose(qmaxOut);
    }
    if (nicOut) fclose(nicOut);

    // Per-port egress utilization, always written; one row per switch port including idle ones,
    // since an unused uplink is the imbalance. busy_ns is the time the port's data needed at
    // line rate, so busy_ns / fabric_end is its utilization over the run and the largest busy_ns
    // anywhere is the lower bound the network put on the run. fabric_end is the last data dequeue
    // anywhere, not Simulator::Now(), which the NIC sampler rounds up to its interval.
    std::string portPath = ns3::SystemPath::Append(LOG_DIR, "switch_port_util_" + opt.label + ".csv");
    int64_t fabricEnd = 0;
    for (const auto& kv : g_portStats) fabricEnd = std::max(fabricEnd, kv.second.lastNs);
    struct Busiest { uint32_t sw = 0, port = 0, peer = 0; double busyNs = 0; } busiest;
    std::vector<uint64_t> s2sBytes; // data bytes on every switch->switch port
    if (FILE* portOut = fopen(portPath.c_str(), "w")) {
        fprintf(portOut, "sw_id,port_id,peer_id,peer_kind,rate_gbps,data_bytes,ctrl_bytes,data_pkts,"
                         "first_ns,last_ns,busy_ns,util_run,util_active\n");
        for (const auto& kv : g_portInfo) {
            const PortInfo& pi = kv.second;
            const auto it = g_portStats.find(kv.first);
            const PortStat ps = (it == g_portStats.end()) ? PortStat() : it->second;
            const double busyNs = pi.rateBps ? ps.dataBytes * 8.0 / pi.rateBps * 1e9 : 0;
            // Dequeue times mark a packet's start on the wire, so the window gets the average
            // packet's serialization time added back to cover the last one.
            const double windowNs = ps.dataPkts
                ? (ps.lastNs - ps.firstNs) + busyNs / ps.dataPkts : 0;
            fprintf(portOut, "%u,%u,%u,%s,%.3f,%llu,%llu,%llu,%ld,%ld,%.1f,%.4f,%.4f\n",
                    kv.first.first, kv.first.second, pi.peerId, pi.peerIsSwitch ? "switch" : "host",
                    pi.rateBps / 1e9, (unsigned long long) ps.dataBytes,
                    (unsigned long long) ps.ctrlBytes, (unsigned long long) ps.dataPkts,
                    ps.firstNs, ps.lastNs, busyNs,
                    fabricEnd ? busyNs / fabricEnd : 0.0, windowNs ? busyNs / windowNs : 0.0);
            if (pi.peerIsSwitch) s2sBytes.push_back(ps.dataBytes);
            if (busyNs > busiest.busyNs)
                busiest = Busiest{kv.first.first, kv.first.second, pi.peerId, busyNs};
        }
        fclose(portOut);
    }

    std::cout << "Topology: " << opt.stem << " (" << opt.ranks << " GPUs, "
              << regswtches.GetN() << " switches, " << nvswtches.GetN() << " host switches)" << std::endl;
    std::cout << "NIC selection: " << (opt.nicSel == "schedule" ? "schedule-pinned (one qp per connection)"
        : (opt.nicSel == "merged" ? "merged NIC (one qp per NIC, message split across them)"
                                  : "round-robin (one qp per connection)")) << std::endl;
    std::cout << "Network flow ids: " << (opt.flowId ? "on (custom headers + per-flow switch forwarding)"
                                                     : "off (no header on the wire, plain ECMP)") << std::endl;
    std::cout << "Network deps (netdepid/netdeps): " << (opt.netDeps ? "honored" : "skipped") << std::endl;
    std::cout << "Algorithm XML: " << XML_ALGO << std::endl;
    std::cout << "Switch queue trace: " << (opt.qlenRows ? qlenPath : std::string("(rows off)")) << std::endl;
    std::cout << "Switch peak-queue summary: " << qmaxPath << std::endl;
    std::cout << "Switch drop/PFC trace: " << eventPath << std::endl;
    // The collective's runtime is when its last step completed on any rank. Simulator::Now() is
    // the last event of any kind, which with --nicBwInterval is the sampler tick after the run
    // ended, so it overstates the runtime by up to one interval. Both are printed; the sweep
    // reads "Total simulated time", and algbw uses the same number.
    Time simTime;
    for (uint32_t i = 0; i < apps.GetN(); ++i) {
        if (Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(apps.Get(i)))
            simTime = std::max(simTime, app->GetLastStepTime());
    }
    std::cout << "Total simulated time: " << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;
    std::cout << "Simulator end (last event of any kind): " << Simulator::Now().GetNanoSeconds()
              << " nanoseconds" << std::endl;

    std::cout << "Switch port utilization: " << portPath << std::endl;
    std::cout << "Fabric end (last switch data dequeue): " << fabricEnd << " ns" << std::endl;
    if (!s2sBytes.empty()) {
        uint64_t sum = 0, mx = 0, mn = UINT64_MAX;
        for (uint64_t b : s2sBytes) { sum += b; mx = std::max(mx, b); mn = std::min(mn, b); }
        const double mean = (double) sum / s2sBytes.size();
        std::cout << "Switch-to-switch data bytes over " << s2sBytes.size() << " ports: min "
                  << mn << " / mean " << (uint64_t) mean << " / max " << mx
                  << " (max/mean " << (mean > 0 ? mx / mean : 0.0) << ")" << std::endl;
    }
    if (busiest.busyNs > 0) {
        std::cout << "Busiest switch port: sw " << busiest.sw << " port " << busiest.port
                  << " -> node " << busiest.peer << ", " << (uint64_t) busiest.busyNs
                  << " ns of data at line rate (" << 100.0 * busiest.busyNs / fabricEnd
                  << "% of fabric end)" << std::endl;
    }

    // How much of the traffic the switch JSON actually steered. A miss means a flow-id-carrying
    // packet reached a switch holding no rule for it and fell back to ECMP, i.e. the schedule
    // was not in force for that packet.
    if (opt.flowId) {
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

    // The same question for the other half of the schedule: --rate selects a rate-annotated
    // XML, but a cap that never reaches the shaper, one that never binds, and one the shaper
    // cannot express (a message of a single MTU has no inter-packet gap to stretch) all
    // produce runs indistinguishable from --rate=0. This says which of those happened.
    RdmaHw::PrintPaceStats(std::cout);

    // algorithm bandwidth: total data moved per rank / time
    const std::string collName = isAllgather ? "allgather" : "alltoall";
    std::cout << collName << " algorithm bandwidth: "
              << (double) INPUT_BYTES * N_NODES / simTime.GetSeconds() / 1e9 << " GB/s" << std::endl;
    if (opt.correctness) {
        CollectiveTestResult res = isAllgather
            ? tester.VerifyAllgather(topo, CHUNK_SIZE * N_CHUNKS)
            : tester.VerifyAlltoall(topo, CHUNK_SIZE * N_CHUNKS);
        if (res == CollectiveTestResult::TEST_OK) std::cout << collName << " verified." << std::endl;
        else std::cout << collName << " incorrect." << std::endl;
    }

    Simulator::Destroy();
    NS_LOG_UNCOND("Done simulation");
    return 0;
}

} // namespace mini

#endif // MINI_HARNESS_H
