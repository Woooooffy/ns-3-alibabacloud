#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ethernet-switch-module.h"
#include "ns3/distributed-ml-module.h"

#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <map>

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("SCRATCH");
    LogComponentEnable("CustomSwitchImpl", LOG_LEVEL_INFO);
    uint32_t inputBytes = (1 << 20);
	CommandLine cmd;
	cmd.AddValue("inputBytes", "Total input size in bytes", inputBytes);
	cmd.Parse(argc, argv);
    #ifdef FLOW_ID_TEST
    NodeContainer gpunodes;
    NodeContainer swtches;
    P4Helper sw_helper;
    sw_helper.SetDeviceAttribute("EnableCustomImpl", BooleanValue(true));
    // Two 200Gbps streams can converge on a single output port (e.g. GPU0→GPU2
    // and GPU1→GPU2 both start at the same simulated tick).  With INPUT_BYTES/MTU
    // fragments per stream, the switch's per-port queue must hold at least that
    // many packets or it silently drops fragments and the allgather produces zeros.
    // 1 000 000 >> any transfer size we intend to test here.
    sw_helper.SetDeviceAttribute("QueueMaxSize", UintegerValue(500000));

    gpunodes.Create<GPU>(3);
    swtches.Create(1);
    SwitchedEthernetHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper0.SetChannelAttribute("Delay", StringValue("1us"));
    link_helper0.SetChannelAttribute("DataRate", StringValue("200Gbps"));

    sw_helper.SetDeviceAttribute("Mtu", UintegerValue(9000));
    NetDeviceContainer sw_dev0 = sw_helper.Install(swtches.Get(0));
    Ptr<P4SwitchNetDevice> sw0 = DynamicCast<P4SwitchNetDevice>(sw_dev0.Get(0));

    NetDeviceContainer devs0_0 = link_helper0.ConnectHost(sw0, gpunodes.Get(0));

    for (int i = 0; i < 3; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(i, devs0_0.Get(0));
            DynamicCast<GPU>(gpunodes.Get(0))->PushRecvPeerDevice(i, devs0_0.Get(0));
        }
    }

    for (int i = 0; i < 3; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(i))->PushPeerAddr(0, devs0_0.Get(0)->GetAddress());
        }
    }

    NetDeviceContainer devs0_1 = link_helper0.ConnectHost(sw0, gpunodes.Get(1));

    for (int i = 0; i < 3; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(i, devs0_1.Get(0));
            DynamicCast<GPU>(gpunodes.Get(1))->PushRecvPeerDevice(i, devs0_1.Get(0));
        }
    }

    for (int i = 0; i < 3; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(i))->PushPeerAddr(1, devs0_1.Get(0)->GetAddress());
        }
    }

    NetDeviceContainer devs0_2 = link_helper0.ConnectHost(sw0, gpunodes.Get(2));

    for (int i = 0; i < 3; ++i){
        if (i != 2){
            DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(i, devs0_2.Get(0));
            DynamicCast<GPU>(gpunodes.Get(2))->PushRecvPeerDevice(i, devs0_2.Get(0));
        }
    }

    for (int i = 0; i < 3; ++i){
        if (i != 2){
            DynamicCast<GPU>(gpunodes.Get(i))->PushPeerAddr(2, devs0_2.Get(0)->GetAddress());
        }
    }


    /*
    */
   		const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_DSL_test.txt";
        std::string XML_ALGO = "/data/scratch/wangyj05/taccl/taccl/custom_examples/Allgather.n3-Custom-N4-.n1-steps1-tacclsol-improve-1781598576_i1_scRemote1_IBContig.sccl.xml";


		constexpr int N_NODES = 3;
        constexpr DataType::Type dtype = DataType::INT32;
        constexpr int N_CHUNKS = 1;
        const uint32_t INPUT_BYTES = inputBytes;
		int CHUNK_SIZE = (INPUT_BYTES / N_CHUNKS) / DataType::GetSizeBytes(dtype);
        // in elements, so total bytes is CHUNK_SIZE * N_CHUNKS * sizeof(datatype)
        bool CORRECTNESS_CHECK = true;



		PacketSocketHelper packetSocket;
		packetSocket.Install(gpunodes);

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

		// flow Ids
		std::map<std::pair<int, int>, uint32_t> flowIds;
		uint32_t c = 0;
		for (int i = 0; i < N_NODES; ++i){
			for (int j = 0; j < N_NODES; ++j){
				if (i != j)flowIds[std::make_pair(i, j)] = c;
				++c;
			}
		}
		for (uint32_t i = 0; i < apps.GetN(); ++i){
			DynamicCast<CollectivesApplication>(apps.Get(i))->StoreFlowIdTable(&flowIds);
		}

		for (auto cur = flowIds.begin(); cur != flowIds.end(); ++cur){
			int dst = cur->first.second;
			uint32_t flow = cur->second;
			sw0->GetCustomImpl()->AddForwardingRule(flow, dst);
		}


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

    Simulator::Run();
    Simulator::Destroy();
    #endif
    return 0;
}
