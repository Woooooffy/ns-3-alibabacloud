#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/distributed-ml-module.h"

#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <map>

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("INCAST_POD_TEST");
    LogComponentEnable("CollectivesApplication", LOG_INFO);
//	LogComponentEnable("SwitchNode", LOG_LEVEL_DEBUG);
    uint32_t inputBytes = (1 << 20);
    CommandLine cmd;
    cmd.AddValue("inputBytes", "Total input size in bytes", inputBytes);
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
    Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(1500));

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

    std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/xml_input/fat_tree_pod_single_spine_alltoall_more_epochs.xml");

    std::string SWITCH_JSON = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/json_input/fat_tree_pod_single_spine_alltoall_more_epochs_switch.json");

    const std::string LOG_FILE = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "Alltoall_DSL_test.txt");

    constexpr DataType::Type dtype = DataType::INT32;
    const uint32_t INPUT_BYTES = inputBytes;
    bool CORRECTNESS_CHECK = true;
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
    Simulator::Run();
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
