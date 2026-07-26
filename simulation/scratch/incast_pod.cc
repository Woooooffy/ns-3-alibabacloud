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
    NS_LOG_COMPONENT_DEFINE("INCAST_POD_TEST");
    LogComponentEnable("CollectivesApplication", LOG_INFO);
//	LogComponentEnable("SwitchNode", LOG_LEVEL_DEBUG);
    uint32_t inputBytes = (1 << 20);
    // label distinguishes output files between runs, e.g. --label=with_rate vs --label=no_rate
    std::string label = "run";
    CommandLine cmd;
    cmd.AddValue("inputBytes", "Total input size in bytes", inputBytes);
    cmd.AddValue("label", "Suffix for the congestion-monitor output CSVs", label);
    cmd.Parse(argc, argv);

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

    // single-spine pod: two edge switches (0, 1) each host two GPUs; spine switch (2)
    // interconnects the two edges. Matches the routing in
    // fat_tree_pod_single_spine_alltoall_more_epochs_switch.json (switches 0, 1, 2).
    NetDeviceContainer devs0_0 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(0));
    NetDeviceContainer devs0_1 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(1));
    NetDeviceContainer devs0_2 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(2));
    NetDeviceContainer devs0_3 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(3));
    NetDeviceContainer devs0_4 = link_helper0.Install(regswtches.Get(0), regswtches.Get(2));
    NetDeviceContainer devs0_5 = link_helper0.Install(regswtches.Get(1), regswtches.Get(2));

    Config::SetDefault("ns3::RdmaHw::CcMode", UintegerValue(12));
    Config::SetDefault("ns3::RdmaHw::L2AckInterval", UintegerValue(0));
    Config::SetDefault("ns3::RdmaHw::L2ChunkSize", UintegerValue(4000));
    Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(4096));

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);

    /*
        esw0 -> gpu0: devs0_0
        esw0 -> gpu1: devs0_1
        esw1 -> gpu2: devs0_2
        esw1 -> gpu3: devs0_3
        esw0 -> spine (sw2): devs0_4
        esw1 -> spine (sw2): devs0_5
    */

    std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/xml_input/fat_tree_pod_single_spine_alltoall.xml");

    std::string SWITCH_JSON = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/json_input/fat_tree_pod_single_spine_alltoall_switch.json");

    // All output files go to simulation/scratch/logs. FindSelfDirectory() resolves to
    // simulation/build/scratch, so "../../scratch/logs" hops back up to the source tree.
    const std::string LOG_DIR = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/logs");
    ns3::SystemPath::MakeDirectories(LOG_DIR); // no-op if it already exists

    const std::string LOG_FILE = ns3::SystemPath::Append(LOG_DIR, "Alltoall_DSL_test.txt");

    constexpr DataType::Type dtype = DataType::INT32;
    const uint32_t INPUT_BYTES = inputBytes;
    bool CORRECTNESS_CHECK = false;
    bool FLOW_ID = true;

    AlgoTopology topo(gpunodes, regswtches);
    AlgoParseResult result = topo.ParseAlgoXml(XML_ALGO.c_str());
    if (result != AlgoParseResult::ALGO_PARSE_SUCCESS) NS_LOG_ERROR("Encountered issue in parsing XML algorithm, error code " << result);
    if (FLOW_ID){
        AlgoParseResult switchResult = topo.ParseSwitchJson(SWITCH_JSON.c_str());
        if (switchResult != AlgoParseResult::ALGO_PARSE_SUCCESS) NS_LOG_ERROR("Encountered issue in parsing switch JSON, error code " << switchResult);
    }

    static std::ofstream logtxt;

    // log file
    logtxt.open(LOG_FILE);
    if (!logtxt.is_open()){
        NS_FATAL_ERROR("Failed to log file");
    }
    chmod(LOG_FILE.c_str(), 0666);

    // Chunk count and participant set come straight from the parsed algorithm, so ChunkSize
    // and the tester can never drift from the XML: for alltoall the per-rank input chunk count
    // equals nchunksperloop (16 in ..._more_epochs, 8 in the single-loop variant), and swapping
    // XMLs needs no source edit here.
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

    CollectiveTester tester(apps, true, logtxt);
    if (CORRECTNESS_CHECK) {
        tester.SetupAlltoall(topo, CHUNK_SIZE * N_CHUNKS);
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
    fprintf(eventOut, "time_ns,node_id,port_id,q_id,bytes,op\n"); // node_id 0-3=GPU, 4-6=switch; drops (with size) and PFC pause/resume (bytes/q_id blank)

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
    // congested ingress link by pausing the sender on the far end -- which for edge-switch <-> GPU
    // links is a host NIC, not a switch. So also connect the drop/PFC traces on the GPU NICs;
    // otherwise switch->host backpressure (the common case here) is never recorded. In the events
    // file, node ids 0..3 are GPUs and 4..6 are switches. Queue-occupancy (enqueue/dequeue) stays
    // switch-only, since host egress is just the GPU injecting and isn't the congestion of interest.
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
    std::cout << "Switch queue trace: " << qlenPath << std::endl;
    std::cout << "Switch drop/PFC trace: " << eventPath << std::endl;
    Time simTime = Simulator::Now();
    std::cout << "Total simulated time: "
        << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;

    // alltoall algorithm bandwidth: total data moved per rank / time
    std::cout << "Alltoall algorithm bandwidth: "
        << (double) INPUT_BYTES * N_NODES / simTime.GetSeconds() / 1e9 << " GB/s" << std::endl;
    // alltoall bus bandwidth: each rank exchanges INPUT_BYTES*(P-1)/P with the others
    std::cout << "Alltoall bus bandwidth: "
        << (double) INPUT_BYTES * (N_NODES - 1) / N_NODES / simTime.GetSeconds() / 1e9 << " GB/s" << std::endl;
    if (CORRECTNESS_CHECK) {
        CollectiveTestResult alltoall_res = tester.VerifyAlltoall(topo, CHUNK_SIZE * N_CHUNKS);
        if (alltoall_res == CollectiveTestResult::TEST_OK) std::cout << "Alltoall verified." << std::endl;
        else std::cout << "Alltoall incorrect." << std::endl;
    }

    Simulator::Destroy();
    NS_LOG_UNCOND("Done simulation");
    return 0;
}
