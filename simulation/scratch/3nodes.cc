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
    NS_LOG_COMPONENT_DEFINE("DGX1_TEST");
    LogComponentEnable("CollectivesApplication", LOG_LEVEL_ALL);
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

    for (uint32_t i = 0; i < 3; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 1; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(1500));
    link_helper0.SetChannelAttribute("Delay", StringValue("1us"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("71Gbps"));

    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), regswtches.Get(0));

    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), regswtches.Get(0));

    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), regswtches.Get(0));

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    InternetStackHelper internetStack;
    internetStack.Install(gpunodes);

    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.1");
        NetDeviceContainer _tmp;
        _tmp.Add(devs0_0.Get(0));
        _ipv4.Assign(_tmp);
    }
    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.2");
        NetDeviceContainer _tmp;
        _tmp.Add(devs0_1.Get(0));
        _ipv4.Assign(_tmp);
    }
    {
        Ipv4AddressHelper _ipv4;
        _ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.3");
        NetDeviceContainer _tmp;
        _tmp.Add(devs0_2.Get(0));
        _ipv4.Assign(_tmp);
    }

    // SwitchNode routing tables (BFS ECMP)
    {
        Ipv4Address _dst("10.0.0.1");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs0_0.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.2");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs0_1.Get(1)->GetIfIndex());
    }
    {
        Ipv4Address _dst("10.0.0.3");
        DynamicCast<SwitchNode>(regswtches.Get(0))->AddTableEntry(_dst, devs0_2.Get(1)->GetIfIndex());
    }

    // RdmaHw + RdmaDriver setup per GPU
    {
        Ptr<RdmaHw> rdmaHw_gpu0 = CreateObject<RdmaHw>();
        rdmaHw_gpu0->SetAttribute("GPUsPerServer", UintegerValue(1));
        rdmaHw_gpu0->SetAttribute("L2AckInterval", UintegerValue(1));
        rdmaHw_gpu0->SetAttribute("L2ChunkSize", UintegerValue(4000));
        rdmaHw_gpu0->SetAttribute("Mtu", UintegerValue(1500));
        Ptr<RdmaDriver> rdmaDriver_gpu0 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu0->SetNode(gpunodes.Get(0));
        rdmaDriver_gpu0->SetRdmaHw(rdmaHw_gpu0);
        rdmaDriver_gpu0->Init();

        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu0->AddTableEntry(_peerIp, devs0_0.Get(0)->GetIfIndex(), false);
        }
        {
            Ipv4Address _peerIp("10.0.0.3");
            rdmaHw_gpu0->AddTableEntry(_peerIp, devs0_0.Get(0)->GetIfIndex(), false);
        }

        DynamicCast<GPU>(gpunodes.Get(0))->SetMyIp(Ipv4Address("10.0.0.1"));
        gpunodes.Get(0)->AggregateObject(rdmaDriver_gpu0);
    }
    {
        Ptr<RdmaHw> rdmaHw_gpu1 = CreateObject<RdmaHw>();
        rdmaHw_gpu1->SetAttribute("GPUsPerServer", UintegerValue(1));
        rdmaHw_gpu1->SetAttribute("L2AckInterval", UintegerValue(1));
        rdmaHw_gpu1->SetAttribute("L2ChunkSize", UintegerValue(4000));
        rdmaHw_gpu1->SetAttribute("Mtu", UintegerValue(1500));
        Ptr<RdmaDriver> rdmaDriver_gpu1 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu1->SetNode(gpunodes.Get(1));
        rdmaDriver_gpu1->SetRdmaHw(rdmaHw_gpu1);
        rdmaDriver_gpu1->Init();

        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu1->AddTableEntry(_peerIp, devs0_1.Get(0)->GetIfIndex(), false);
        }
        {
            Ipv4Address _peerIp("10.0.0.3");
            rdmaHw_gpu1->AddTableEntry(_peerIp, devs0_1.Get(0)->GetIfIndex(), false);
        }

        DynamicCast<GPU>(gpunodes.Get(1))->SetMyIp(Ipv4Address("10.0.0.2"));
        gpunodes.Get(1)->AggregateObject(rdmaDriver_gpu1);
    }
    {
        Ptr<RdmaHw> rdmaHw_gpu2 = CreateObject<RdmaHw>();
        rdmaHw_gpu2->SetAttribute("GPUsPerServer", UintegerValue(1));
        rdmaHw_gpu2->SetAttribute("L2AckInterval", UintegerValue(1));
        rdmaHw_gpu2->SetAttribute("L2ChunkSize", UintegerValue(4000));
        rdmaHw_gpu2->SetAttribute("Mtu", UintegerValue(1500));
        Ptr<RdmaDriver> rdmaDriver_gpu2 = CreateObject<RdmaDriver>();
        rdmaDriver_gpu2->SetNode(gpunodes.Get(2));
        rdmaDriver_gpu2->SetRdmaHw(rdmaHw_gpu2);
        rdmaDriver_gpu2->Init();

        {
            Ipv4Address _peerIp("10.0.0.1");
            rdmaHw_gpu2->AddTableEntry(_peerIp, devs0_2.Get(0)->GetIfIndex(), false);
        }
        {
            Ipv4Address _peerIp("10.0.0.2");
            rdmaHw_gpu2->AddTableEntry(_peerIp, devs0_2.Get(0)->GetIfIndex(), false);
        }

        DynamicCast<GPU>(gpunodes.Get(2))->SetMyIp(Ipv4Address("10.0.0.3"));
        gpunodes.Get(2)->AggregateObject(rdmaDriver_gpu2);
    }

    // peer IP bookkeeping for the collectives app's RDMA-routed peers
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerIpAddr(2, Ipv4Address("10.0.0.3"));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerIpAddr(0, Ipv4Address("10.0.0.1"));
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(1, Ipv4Address("10.0.0.2"));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerIpAddr(2, Ipv4Address("10.0.0.3"));

    // peer RDMA pacing: bandwidth-delay-product window + base RTT per peer
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerWin(0, 53500);
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerBaseRtt(0, 6028);
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerWin(2, 53500);
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerBaseRtt(2, 6028);
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerWin(0, 53500);
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerBaseRtt(0, 6028);
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerWin(1, 53500);
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerBaseRtt(1, 6028);
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerWin(1, 53500);
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerBaseRtt(1, 6028);
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerWin(2, 53500);
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerBaseRtt(2, 6028);

    // switch/nvswitch MMU: PFC headroom + ECN thresholds per port (otherwise
    // SwitchMmu's headroom[]/kmin[]/kmax[]/pmax[]/pfc_a_shift[] are uninitialized,
    // which disables realistic PFC backpressure under incast)
    DynamicCast<SwitchNode>(regswtches.Get(0))->SetAttribute("EcnEnabled", BooleanValue(true));
    {
        uint32_t _port = devs0_0.Get(1)->GetIfIndex();
        DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->ConfigEcn(_port, 36, 71, 0.2);
        DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->ConfigHdrm(_port, 26625);
        DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->pfc_a_shift[_port] = 3;
    }
    {
        uint32_t _port = devs0_1.Get(1)->GetIfIndex();
        DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->ConfigEcn(_port, 36, 71, 0.2);
        DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->ConfigHdrm(_port, 26625);
        DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->pfc_a_shift[_port] = 3;
    }
    {
        uint32_t _port = devs0_2.Get(1)->GetIfIndex();
        DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->ConfigEcn(_port, 36, 71, 0.2);
        DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->ConfigHdrm(_port, 26625);
        DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->pfc_a_shift[_port] = 3;
    }
    DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->ConfigNPort(3);
    DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu->node_id = DynamicCast<SwitchNode>(regswtches.Get(0))->GetId();

    NS_LOG_INFO("Finished DSL emitted setup.");

    /*
        n0 -> sw: devs0_0
        n1 -> sw: devs0_1
        n2 -> sw: devs0_2
    */
const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_DSL_test.txt";
    //std::string XML_ALGO = "/data/scratch/wangyj05/taccl/taccl/custom_examples/Allgather.n3-Custom-N4-.n1-steps1-tacclsol-improve-1781598576_i1_scRemote1_IBContig.sccl.xml";
		std::string XML_ALGO = "/data/commit/graphit/wangyj05/workspace/ns-3-alibabacloud/simulation/scratch/teccl.xml";


    // constexpr int N_NODES = 3;
    constexpr DataType::Type dtype = DataType::INT32;
    constexpr int N_CHUNKS = 1;
    const uint32_t INPUT_BYTES = inputBytes;
    int CHUNK_SIZE = (INPUT_BYTES / N_CHUNKS) / DataType::GetSizeBytes(dtype);
    // in elements, so total bytes is CHUNK_SIZE * N_CHUNKS * sizeof(datatype)
    bool CORRECTNESS_CHECK = false;

    TopoNodeSet topo(gpunodes);
    AlgoParseResult result = ParseAlgoFromXml(XML_ALGO.c_str(), topo);
    if (result != AlgoParseResult::ALGO_PARSE_SUCCESS) NS_LOG_ERROR("Encountered issue in parsing XML algorithm, error code " << result);

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
