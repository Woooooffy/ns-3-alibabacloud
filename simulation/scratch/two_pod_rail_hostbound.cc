// TwoPodRailHostBound: the RATE configuration of TE-CCL's 2-pod rail-optimized topology.
// Topology body generated from topology/examples/two_pod_rail_hostbound.topo via topology/main.py;
// the harness around it (algorithm parse, correctness check, congestion monitoring) follows
// hetero_cluster.cc / rail.cc.
//
//   16 GPUs (8 nodes x 2 rails) behind 8 NVSwitches, 4 leaves (pod x rail), 2 unequal spines.
//   ns-3 switch ids line up with the TE-CCL indexing the switch JSON assumes:
//     regswtches 0..3 = leaf(podA,r0), leaf(podA,r1), leaf(podB,r0), leaf(podB,r1)  -> TE-CCL 24..27
//     regswtches 4..5 = spine0 (50 GBps), spine1 (25 GBps)                          -> TE-CCL 28..29
//   GPU->leaf runs at 25 GBps, which is the binding cut: both streams on that link are pinned
//   with zero slack, so a rate-oblivious emitter cannot hide in headroom. That is the point of
//   this configuration, hence RateTargeting defaults to on here.

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/distributed-ml-module.h"

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

// QbbPfc: type 1 = PAUSE sent upstream (this port's ingress is congested), 0 = RESUME.
// q_id and bytes columns are left blank so PFC rows share the drop event schema.
static void OnSwitchPfc(FILE* out, uint32_t swId, uint32_t port, uint32_t type) {
    fprintf(out, "%ld,%u,%u,,,%s\n", Simulator::Now().GetNanoSeconds(), swId, port, type == 1 ? "pause" : "resume");
}

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("TWO_POD_RAIL_HOSTBOUND");
//    LogComponentEnable("CollectivesApplication", LOG_INFO);
//    LogComponentEnable("SwitchNode", LOG_LEVEL_DEBUG);
    LogComponentEnable("AlgoTopo", LOG_LEVEL_WARN);

    uint32_t inputBytes = (1 << 20);
    // label distinguishes output files between runs, e.g. --label=with_rate vs --label=no_rate
    std::string label = "two_pod_hostbound";
    // Which of the four two_pod_hostbound_* XMLs to run. `rate` picks the rate-annotated
    // schedule vs the _no_rate ablation -- the comparison this topology exists to make.
    std::string coll = "allgather";  // allgather | alltoall
    bool rate = true;
    // Make the per-flow XML "rate" a true target (accumulating token-bucket shaper) rather than
    // just an upper bound, so a flow paced below line rate actually runs at its assigned rate.
    bool rateTargeting = true;
    bool flowId = true;              // install the per-flow switch forwarding table from the JSON
    std::string checkLog = "minimal"; // silent | minimal | verbose
    uint32_t maxMismatches = 10;

    CommandLine cmd;
    cmd.AddValue("inputBytes", "Total input size in bytes", inputBytes);
    cmd.AddValue("label", "Suffix for the congestion-monitor output CSVs", label);
    cmd.AddValue("coll", "Collective to run: allgather | alltoall", coll);
    cmd.AddValue("rate", "Use the rate-annotated XML (false = the _no_rate ablation)", rate);
    cmd.AddValue("rateTargeting", "Treat per-flow XML rates as targets, not just caps", rateTargeting);
    cmd.AddValue("flowId", "Install per-flow switch forwarding from the switch JSON", flowId);
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

    for (uint32_t i = 0; i < 16; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 6; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    for (uint32_t i = 0; i < 8; ++i) { nvswtches.Add(CreateObject<NVSwitchNode>()); }

    // NVLink: GPU <-> its node's NVSwitch, 900 GBps / 350ns.
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper0.SetChannelAttribute("Delay", StringValue("350ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("900GBps"));

    // 25 GBps / 700ns: both the GPU->leaf rail (the binding cut) and leaf->spine1.
    QbbHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper1.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("25GBps"));

    // 50 GBps / 700ns: leaf->spine0, the fat half of the deliberately unequal spine pair.
    QbbHelper link_helper2;
    link_helper2.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper2.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper2.SetDeviceAttribute("DataRate", StringValue("50GBps"));

    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), nvswtches.Get(0));
    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), nvswtches.Get(0));
    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), nvswtches.Get(1));
    NetDeviceContainer devs0_3 = link_helper0.Install(gpunodes.Get(3), nvswtches.Get(1));
    NetDeviceContainer devs0_4 = link_helper0.Install(gpunodes.Get(4), nvswtches.Get(2));
    NetDeviceContainer devs0_5 = link_helper0.Install(gpunodes.Get(5), nvswtches.Get(2));
    NetDeviceContainer devs0_6 = link_helper0.Install(gpunodes.Get(6), nvswtches.Get(3));
    NetDeviceContainer devs0_7 = link_helper0.Install(gpunodes.Get(7), nvswtches.Get(3));
    NetDeviceContainer devs0_8 = link_helper0.Install(gpunodes.Get(8), nvswtches.Get(4));
    NetDeviceContainer devs0_9 = link_helper0.Install(gpunodes.Get(9), nvswtches.Get(4));
    NetDeviceContainer devs0_10 = link_helper0.Install(gpunodes.Get(10), nvswtches.Get(5));
    NetDeviceContainer devs0_11 = link_helper0.Install(gpunodes.Get(11), nvswtches.Get(5));
    NetDeviceContainer devs0_12 = link_helper0.Install(gpunodes.Get(12), nvswtches.Get(6));
    NetDeviceContainer devs0_13 = link_helper0.Install(gpunodes.Get(13), nvswtches.Get(6));
    NetDeviceContainer devs0_14 = link_helper0.Install(gpunodes.Get(14), nvswtches.Get(7));
    NetDeviceContainer devs0_15 = link_helper0.Install(gpunodes.Get(15), nvswtches.Get(7));

    // Rail downlinks: leaf(pod, rail) <- the rail-r GPU of every node in that pod.
    NetDeviceContainer devs1_16 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(0));
    NetDeviceContainer devs1_17 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(1));
    NetDeviceContainer devs1_18 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(2));
    NetDeviceContainer devs1_19 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(3));
    NetDeviceContainer devs1_20 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(4));
    NetDeviceContainer devs1_21 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(5));
    NetDeviceContainer devs1_22 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(6));
    NetDeviceContainer devs1_23 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(7));
    NetDeviceContainer devs1_24 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(8));
    NetDeviceContainer devs1_25 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(9));
    NetDeviceContainer devs1_26 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(10));
    NetDeviceContainer devs1_27 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(11));
    NetDeviceContainer devs1_28 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(12));
    NetDeviceContainer devs1_29 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(13));
    NetDeviceContainer devs1_30 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(14));
    NetDeviceContainer devs1_31 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(15));

    // Leaf-spine full mesh: every leaf reaches spine0 at 50 GBps and spine1 at 25 GBps.
    NetDeviceContainer devs2_32 = link_helper2.Install(regswtches.Get(0), regswtches.Get(4));
    NetDeviceContainer devs1_33 = link_helper1.Install(regswtches.Get(0), regswtches.Get(5));
    NetDeviceContainer devs2_34 = link_helper2.Install(regswtches.Get(1), regswtches.Get(4));
    NetDeviceContainer devs1_35 = link_helper1.Install(regswtches.Get(1), regswtches.Get(5));
    NetDeviceContainer devs2_36 = link_helper2.Install(regswtches.Get(2), regswtches.Get(4));
    NetDeviceContainer devs1_37 = link_helper1.Install(regswtches.Get(2), regswtches.Get(5));
    NetDeviceContainer devs2_38 = link_helper2.Install(regswtches.Get(3), regswtches.Get(4));
    NetDeviceContainer devs1_39 = link_helper1.Install(regswtches.Get(3), regswtches.Get(5));

    Config::SetDefault("ns3::RdmaHw::CcMode", UintegerValue(12));
    Config::SetDefault("ns3::RdmaHw::RateTargeting", BooleanValue(rateTargeting));
    Config::SetDefault("ns3::RdmaHw::L2AckInterval", UintegerValue(0));
    Config::SetDefault("ns3::RdmaHw::L2ChunkSize", UintegerValue(4000));
    Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(4096));

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);

    // Algorithm + per-flow forwarding table. The switch JSON's switch_id_map (0..5 -> TE-CCL
    // 24..29) matches the leaf-then-spine order the containers are built in above; there is one
    // JSON per collective, shared by the rate and _no_rate XMLs since routing is identical.
    const std::string XML_NAME = "two_pod_hostbound_" + coll + (rate ? "" : "_no_rate") + ".xml";
    std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(),
                                                  "../../scratch/xml_input/" + XML_NAME);
    std::string SWITCH_JSON = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(),
                                                      "../../scratch/json_input/two_pod_hostbound_" + coll + ".json");

    // All output files go to simulation/scratch/logs. FindSelfDirectory() resolves to
    // simulation/build/scratch, so "../../scratch/logs" hops back up to the source tree.
    const std::string LOG_DIR = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/logs");
    ns3::SystemPath::MakeDirectories(LOG_DIR); // no-op if it already exists

    const std::string LOG_FILE = ns3::SystemPath::Append(LOG_DIR, "two_pod_hostbound_" + label + ".txt");

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
        // table mostly empty), and the run then silently falls back to ECMP everywhere -- which
        // on this topology means the unequal spines get an equal split and the numbers are junk.
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
    // node_id: GPUs are the first 16 ids, then the 6 switches (leaf0-3, spine0-1); drops carry a
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
    // is a host NIC, not a switch. Since the GPU->leaf rail is the binding cut here, that is
    // exactly where the backpressure shows up, so connect the drop/PFC traces on the GPU NICs too.
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

    Simulator::Run();
    fclose(qlenOut);
    fclose(eventOut);
    std::cout << "Algorithm XML: " << XML_ALGO << std::endl;
    std::cout << "Switch queue trace: " << qlenPath << std::endl;
    std::cout << "Switch drop/PFC trace: " << eventPath << std::endl;
    Time simTime = Simulator::Now();
    std::cout << "Total simulated time: "
        << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;

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
