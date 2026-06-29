#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/distributed-ml-module.h"

#include <vector>

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("FAT_TREE_POD_TEST");
    LogComponentEnable("CollectivesApplication", LOG_LEVEL_ALL);
	LogComponentEnable("SwitchNode", LOG_LEVEL_ALL);
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
    for (uint32_t i = 0; i < 4; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(1500));
    link_helper0.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("400Gbps"));

    NetDeviceContainer devs0_0 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(0));

    NetDeviceContainer devs0_1 = link_helper0.Install(regswtches.Get(0), gpunodes.Get(1));

    NetDeviceContainer devs0_2 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(2));

    NetDeviceContainer devs0_3 = link_helper0.Install(regswtches.Get(1), gpunodes.Get(3));

    NetDeviceContainer devs0_4 = link_helper0.Install(regswtches.Get(0), regswtches.Get(2));

    NetDeviceContainer devs0_5 = link_helper0.Install(regswtches.Get(0), regswtches.Get(3));

    NetDeviceContainer devs0_6 = link_helper0.Install(regswtches.Get(1), regswtches.Get(2));

    NetDeviceContainer devs0_7 = link_helper0.Install(regswtches.Get(1), regswtches.Get(3));

    Config::SetDefault("ns3::RdmaHw::CcMode", UintegerValue(12));
    Config::SetDefault("ns3::RdmaHw::L2AckInterval", UintegerValue(0));
    Config::SetDefault("ns3::RdmaHw::L2ChunkSize", UintegerValue(4000));
    Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(1500));

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);


    /*
        p_esw1_sw -> p_esw1_gpu1: devs0_0
        p_esw1_sw -> p_esw1_gpu2: devs0_1
        p_esw2_sw -> p_esw2_gpu1: devs0_2
        p_esw2_sw -> p_esw2_gpu2: devs0_3
        p_esw1_sw -> p_asw1: devs0_4
        p_esw1_sw -> p_asw2: devs0_5
        p_esw2_sw -> p_asw1: devs0_6
        p_esw2_sw -> p_asw2: devs0_7
    */

    const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_DSL_test.txt";
    // algo3nodes.xml has mscclflowid set on every step; star_switch_entry.json maps
    // those same flow ids to switch ports, exercising the pipeline end to end (XML
    // attribute -> mscclTransfer -> RdmaQueuePair -> MscclFlowIdHeader on the wire ->
    // switch lookup).
    std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/fat_tree_pod_symmetric.xml");
    std::string SWITCH_JSON = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/fat_tree_pod_symmetric_switch.json");


    constexpr DataType::Type dtype = DataType::INT32;
    constexpr int N_CHUNKS = 1;
    const uint32_t INPUT_BYTES = inputBytes;
    int CHUNK_SIZE = (INPUT_BYTES / N_CHUNKS) / DataType::GetSizeBytes(dtype);
    bool CORRECTNESS_CHECK = true;

    AlgoTopology topo(gpunodes, regswtches);
    AlgoParseResult result = topo.ParseAlgoXml(XML_ALGO.c_str());
    if (result != AlgoParseResult::ALGO_PARSE_SUCCESS) NS_LOG_ERROR("Encountered issue in parsing XML algorithm, error code " << result);
    AlgoParseResult switchResult = topo.ParseSwitchJson(SWITCH_JSON.c_str());
    if (switchResult != AlgoParseResult::ALGO_PARSE_SUCCESS) NS_LOG_ERROR("Encountered issue in parsing switch JSON, error code " << switchResult);

    static std::ofstream logtxt;

    // log file
    logtxt.open(LOG_FILE);
    if (!logtxt.is_open()){
        NS_FATAL_ERROR("Failed to log file");
    }
    chmod(LOG_FILE.c_str(), 0666);

    // install apps
    CollectivesApplicationHelper app_helper;
    app_helper.SetAttribute("DataType", EnumValue(dtype));
    app_helper.SetAttribute("ChunkSize", UintegerValue(CHUNK_SIZE));
    app_helper.SetAttribute("CorrectnessCheck", BooleanValue(CORRECTNESS_CHECK));
    ApplicationContainer apps = app_helper.Install<GPU>(gpunodes);

    NS_LOG_INFO("Finished installing collective apps.");

    CollectiveTester tester(apps, true, logtxt);
    if (CORRECTNESS_CHECK) {
        tester.SetupAllgather(CHUNK_SIZE * N_CHUNKS, N_CHUNKS);
    }
    else{
        NS_LOG_UNCOND("Skipping correctness check.");
    }
    Simulator::Run();
    Time simTime = Simulator::Now();
    std::cout << "Total simulated time: "
        << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;

    if (CORRECTNESS_CHECK) {
        CollectiveTestResult allgather_res = tester.VerifyAllgather(CHUNK_SIZE * N_CHUNKS, N_CHUNKS);
        if (allgather_res == CollectiveTestResult::TEST_OK) std::cout << "Allgather verified." << std::endl;
        else std::cout << "Allgather incorrect." << std::endl;
    }

    Simulator::Destroy();
    NS_LOG_UNCOND("Done simulation");
    return 0;
}
