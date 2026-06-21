#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
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
    #ifdef FLOW_ID_TEST
    NS_LOG_COMPONENT_DEFINE("2GPUS1SW_TEST");
    NodeContainer gpunodes;
    NodeContainer swtches;
    P4Helper sw_helper;
    sw_helper.SetDeviceAttribute("EnableCustomImpl", BooleanValue(true));

    gpunodes.Create<GPU>(2);
    swtches.Create(1);
    SwitchedEthernetHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(1500));
    link_helper0.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper0.SetChannelAttribute("DataRate", StringValue("25GBps"));


    sw_helper.SetDeviceAttribute("Mtu", UintegerValue(1500));
    NetDeviceContainer sw_dev0 = sw_helper.Install(swtches.Get(0));
    Ptr<P4SwitchNetDevice> sw0 = DynamicCast<P4SwitchNetDevice>(sw_dev0.Get(0));

    NetDeviceContainer devs0_0 = link_helper0.ConnectHost(sw0, gpunodes.Get(0));

    for (int i = 0; i < 2; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(i, devs0_0.Get(0));
            DynamicCast<GPU>(gpunodes.Get(0))->PushRecvPeerDevice(i, devs0_0.Get(0));
        }
    }

    for (int i = 0; i < 2; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(i))->PushPeerAddr(0, devs0_0.Get(0)->GetAddress());
        }
    }

    NetDeviceContainer devs0_1 = link_helper0.ConnectHost(sw0, gpunodes.Get(1));

    for (int i = 0; i < 2; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(i, devs0_1.Get(0));
            DynamicCast<GPU>(gpunodes.Get(1))->PushRecvPeerDevice(i, devs0_1.Get(0));
        }
    }

    for (int i = 0; i < 2; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(i))->PushPeerAddr(1, devs0_1.Get(0)->GetAddress());
        }
    }


    /*
    */

    const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_DSL_test.txt";
//		std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/test.xml");
		std::string XML_ALGO = "/data/scratch/wangyj05/msccl_xml/single_instance/Allgather_n_2_-Line_n_2_-steps_1.xml";

		constexpr int N_NODES = 2;
		constexpr int N_CHUNKS = 1;
        constexpr int INPUT_BYTES = 4 * (1 << 20);
		int CHUNK_SIZE = (INPUT_BYTES / N_CHUNKS) / DataType::GetSizeBytes(DataType::INT32);
        constexpr bool CORRECTNESS_CHECK = true;

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
		app_helper.SetAttribute("DataType", EnumValue(DataType::INT32));
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
			//int src = cur->first.first;
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
    #endif
    return 0;
}
