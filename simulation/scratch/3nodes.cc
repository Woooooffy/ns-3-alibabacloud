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

    for (uint32_t i = 0; i < 3; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 1; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper0.SetChannelAttribute("Delay", StringValue("1us"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("71Gbps"));

    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), regswtches.Get(0));

    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), regswtches.Get(0));

    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), regswtches.Get(0));

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    InternetStackHelper internetStack;
    internetStack.Install(gpunodes);

    {
        struct _IpAssign { Ptr<NetDevice> dev; const char* hostMask; };
        std::vector<_IpAssign> _ipAssigns = {
            {devs0_0.Get(0), "0.0.0.1"},
            {devs0_1.Get(0), "0.0.0.2"},
            {devs0_2.Get(0), "0.0.0.3"},
        };
        for (auto& a : _ipAssigns) {
            Ipv4AddressHelper _ipv4;
            _ipv4.SetBase("10.0.0.0", "255.0.0.0", a.hostMask);
            NetDeviceContainer _tmp;
            _tmp.Add(a.dev);
            _ipv4.Assign(_tmp);
        }
    }

    // SwitchNode routing tables (BFS ECMP)
    {
        struct _Route { Ptr<SwitchNode> node; Ipv4Address dst; Ptr<NetDevice> dev; };
        std::vector<_Route> _routes = {
            {DynamicCast<SwitchNode>(regswtches.Get(0)), Ipv4Address("10.0.0.1"), devs0_0.Get(1)},
            {DynamicCast<SwitchNode>(regswtches.Get(0)), Ipv4Address("10.0.0.2"), devs0_1.Get(1)},
            {DynamicCast<SwitchNode>(regswtches.Get(0)), Ipv4Address("10.0.0.3"), devs0_2.Get(1)},
        };
        for (auto& r : _routes) {
            r.node->AddTableEntry(r.dst, r.dev->GetIfIndex());
        }
    }

    // RdmaHw + RdmaDriver setup per GPU
    {
        struct _RdmaRoute { Ipv4Address dst; uint32_t ifIndex; bool isNvswitch; };
        struct _GpuRdmaSetup { Ptr<Node> gpu; Ipv4Address myIp; std::vector<_RdmaRoute> routes; };
        std::vector<_GpuRdmaSetup> _gpuSetups = {
            {gpunodes.Get(0), Ipv4Address("10.0.0.1"), {{Ipv4Address("10.0.0.2"), devs0_0.Get(0)->GetIfIndex(), false}, {Ipv4Address("10.0.0.3"), devs0_0.Get(0)->GetIfIndex(), false}}},
            {gpunodes.Get(1), Ipv4Address("10.0.0.2"), {{Ipv4Address("10.0.0.1"), devs0_1.Get(0)->GetIfIndex(), false}, {Ipv4Address("10.0.0.3"), devs0_1.Get(0)->GetIfIndex(), false}}},
            {gpunodes.Get(2), Ipv4Address("10.0.0.3"), {{Ipv4Address("10.0.0.1"), devs0_2.Get(0)->GetIfIndex(), false}, {Ipv4Address("10.0.0.2"), devs0_2.Get(0)->GetIfIndex(), false}}},
        };
        for (auto& g : _gpuSetups) {
            Ptr<RdmaHw> rdmaHw = CreateObject<RdmaHw>();
            rdmaHw->SetAttribute("GPUsPerServer", UintegerValue(1));
            rdmaHw->SetAttribute("CcMode", UintegerValue(12));
            rdmaHw->SetAttribute("L2AckInterval", UintegerValue(0));
            rdmaHw->SetAttribute("L2ChunkSize", UintegerValue(4000));
            rdmaHw->SetAttribute("Mtu", UintegerValue(1500));
            Ptr<RdmaDriver> rdmaDriver = CreateObject<RdmaDriver>();
            rdmaDriver->SetNode(g.gpu);
            rdmaDriver->SetRdmaHw(rdmaHw);
            rdmaDriver->Init();
            for (auto& r : g.routes) {
                rdmaHw->AddTableEntry(r.dst, r.ifIndex, r.isNvswitch);
            }
            DynamicCast<GPU>(g.gpu)->SetMyIp(g.myIp);
            g.gpu->AggregateObject(rdmaDriver);
        }
    }

    // peer IP bookkeeping for the collectives app's RDMA-routed peers
    {
        struct _PeerIp { Ptr<Node> gpu; int16_t peerIdx; Ipv4Address peerIp; };
        std::vector<_PeerIp> _peerIps = {
            {gpunodes.Get(1), 0, Ipv4Address("10.0.0.1")},
            {gpunodes.Get(1), 2, Ipv4Address("10.0.0.3")},
            {gpunodes.Get(2), 0, Ipv4Address("10.0.0.1")},
            {gpunodes.Get(2), 1, Ipv4Address("10.0.0.2")},
            {gpunodes.Get(0), 1, Ipv4Address("10.0.0.2")},
            {gpunodes.Get(0), 2, Ipv4Address("10.0.0.3")},
        };
        for (auto& p : _peerIps) {
            DynamicCast<GPU>(p.gpu)->PushPeerIpAddr(p.peerIdx, p.peerIp);
        }
    }

    // peer RDMA pacing: bandwidth-delay-product window + base RTT per peer
    {
        struct _PeerPacing { Ptr<Node> gpu; int16_t peerIdx; uint32_t winBytes; uint64_t baseRttNs; };
        std::vector<_PeerPacing> _peerPacing = {
            {gpunodes.Get(1), 0, 53500, 6028},
            {gpunodes.Get(1), 2, 53500, 6028},
            {gpunodes.Get(2), 0, 53500, 6028},
            {gpunodes.Get(2), 1, 53500, 6028},
            {gpunodes.Get(0), 1, 53500, 6028},
            {gpunodes.Get(0), 2, 53500, 6028},
        };
        for (auto& p : _peerPacing) {
            DynamicCast<GPU>(p.gpu)->PushPeerWin(p.peerIdx, p.winBytes);
            DynamicCast<GPU>(p.gpu)->PushPeerBaseRtt(p.peerIdx, p.baseRttNs);
        }
    }

    // switch/nvswitch MMU: PFC headroom + ECN thresholds per port (otherwise
    // SwitchMmu's headroom[]/kmin[]/kmax[]/pmax[]/pfc_a_shift[] are uninitialized,
    // which disables realistic PFC backpressure under incast)
    {
        struct _MmuNode { Ptr<Node> node; Ptr<SwitchMmu> mmu; uint32_t nPorts; bool isSwitch; };
        std::vector<_MmuNode> _mmuNodes = {
            {regswtches.Get(0), DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu, 3, true},
        };
        for (auto& n : _mmuNodes) {
            if (n.isSwitch) n.node->SetAttribute("EcnEnabled", BooleanValue(true));
            n.mmu->ConfigNPort(n.nPorts);
            n.mmu->node_id = n.node->GetId();
        }
    }

    {
        struct _MmuPort { Ptr<SwitchMmu> mmu; uint32_t port; uint32_t headroomBytes; uint32_t kminKB; uint32_t kmaxKB; double pmax; uint32_t shift; };
        std::vector<_MmuPort> _mmuPorts = {
            {DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu, devs0_0.Get(1)->GetIfIndex(), 26625, 36, 71, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu, devs0_1.Get(1)->GetIfIndex(), 26625, 36, 71, 0.2, 3},
            {DynamicCast<SwitchNode>(regswtches.Get(0))->m_mmu, devs0_2.Get(1)->GetIfIndex(), 26625, 36, 71, 0.2, 3},
        };
        for (auto& p : _mmuPorts) {
            p.mmu->ConfigEcn(p.port, p.kminKB, p.kmaxKB, p.pmax);
            p.mmu->ConfigHdrm(p.port, p.headroomBytes);
            p.mmu->pfc_a_shift[p.port] = p.shift;
        }
    }

    // Custom flow forwarding: exercise the mscclflowid pipeline (XML attribute ->
    // mscclTransfer -> RdmaQueuePair -> MscclFlowIdHeader on the wire -> switch
    // lookup). With only one switch and one link per GPU there's no actual ECMP
    // fan-out to bypass, so each rule below just points at the same port plain
    // ECMP would already pick -- this only verifies the rule lookup fires, not
    // that it changes the path. Flow ids match teccl_algo.xml's send steps.
    {
        Ptr<SwitchNode> sw0 = DynamicCast<SwitchNode>(regswtches.Get(0));
        sw0->SetAttribute("CustomFlowForwarding", BooleanValue(true));
        struct _FlowRule { uint32_t flowId; Ptr<NetDevice> outDev; };
        std::vector<_FlowRule> _flowRules = {
            {0, devs0_1.Get(1)}, // gpu0 -> gpu1
            {1, devs0_2.Get(1)}, // gpu0 -> gpu2
            {2, devs0_0.Get(1)}, // gpu1 -> gpu0
            {3, devs0_2.Get(1)}, // gpu1 -> gpu2
            {4, devs0_0.Get(1)}, // gpu2 -> gpu0
            {5, devs0_1.Get(1)}, // gpu2 -> gpu1
        };
        for (auto& r : _flowRules) {
            sw0->AddFlowForwardingRule(r.flowId, r.outDev->GetIfIndex());
        }
    }

    /*
        n0 -> sw: devs0_0
        n1 -> sw: devs0_1
        n2 -> sw: devs0_2
    */

		const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_DSL_test.txt";
    // teccl_algo.xml has mscclflowid set on every send step, to test the custom
    // flow forwarding pipeline wired up above.
    std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/teccl_algo.xml");
		// std::string XML_ALGO = "/data/scratch/wangyj05/taccl/taccl/custom_examples/Allgather.n3-Custom-N4-.n1-steps1-tacclsol-improve-1781598576_i1_scRemote1_IBContig.sccl.xml";
	// std::string XML_ALGO = "/data/commit/graphit/wangyj05/workspace/ns-3-alibabacloud/simulation/scratch/teccl.xml";


    // constexpr int N_NODES = 3;
    constexpr DataType::Type dtype = DataType::INT32;
    constexpr int N_CHUNKS = 1;
    const uint32_t INPUT_BYTES = inputBytes;
    int CHUNK_SIZE = (INPUT_BYTES / N_CHUNKS) / DataType::GetSizeBytes(dtype);
    // in elements, so total bytes is CHUNK_SIZE * N_CHUNKS * sizeof(datatype)
    bool CORRECTNESS_CHECK = true;

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
