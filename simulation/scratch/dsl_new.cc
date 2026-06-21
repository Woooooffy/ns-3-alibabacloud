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
/*
gpu0	gpu1    gpu4	gpu5
	|   |         |    |
	swtch0--------swtch1
  |   |         |    |
gpu2	gpu3    gpu6  gpu7
*/
int main(int argc, char *argv[]) {

		uint32_t inputBytes = (1 << 20);
		CommandLine cmd;
		cmd.AddValue("inputBytes", "Total input size in bytes", inputBytes);
		cmd.Parse(argc, argv);

		#ifdef FLOW_ID_TEST

		NS_LOG_COMPONENT_DEFINE("DSL_TEST");
		LogComponentEnable("CustomSwitchImpl", LOG_LEVEL_INFO);
//		LogComponentEnable("P4SwitchNetDevice", LOG_LEVEL_ALL);
//		LogComponentEnable("SwitchedEthernetHostDevice", LOG_LEVEL_ALL);
//		LogComponentEnable("CollectivesApplication", LOG_LEVEL_ALL);
//		LogComponentEnable("PacketSocket", LOG_LEVEL_ALL);
    NodeContainer gpunodes;
    NodeContainer swtches;
    P4Helper sw_helper;
    sw_helper.SetDeviceAttribute("EnableCustomImpl", BooleanValue(true));

    gpunodes.Create<GPU>(8);
    swtches.Create(2);
    SwitchedEthernetHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(65535));
    link_helper0.SetChannelAttribute("Delay", StringValue("300ns"));
    link_helper0.SetChannelAttribute("DataRate", StringValue("125Gbps"));

    SwitchedEthernetHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(65535));
    link_helper1.SetChannelAttribute("Delay", StringValue("1500ns"));
    link_helper1.SetChannelAttribute("DataRate", StringValue("50Gbps"));

    sw_helper.SetDeviceAttribute("Mtu", UintegerValue(65535));
    NetDeviceContainer sw_dev0 = sw_helper.Install(swtches.Get(0));
    Ptr<P4SwitchNetDevice> sw0 = DynamicCast<P4SwitchNetDevice>(sw_dev0.Get(0));
    NetDeviceContainer sw_dev1 = sw_helper.Install(swtches.Get(1));
    Ptr<P4SwitchNetDevice> sw1 = DynamicCast<P4SwitchNetDevice>(sw_dev1.Get(0));

    NetDeviceContainer devs0_0 = link_helper0.ConnectHost(sw0, gpunodes.Get(0));

    for (int i = 0; i < 8; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(i, devs0_0.Get(0));
            DynamicCast<GPU>(gpunodes.Get(0))->PushRecvPeerDevice(i, devs0_0.Get(0));
        }
    }

    NetDeviceContainer devs0_1 = link_helper0.ConnectHost(sw0, gpunodes.Get(1));

    for (int i = 0; i < 8; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(i, devs0_1.Get(0));
            DynamicCast<GPU>(gpunodes.Get(1))->PushRecvPeerDevice(i, devs0_1.Get(0));
        }
    }

    NetDeviceContainer devs0_2 = link_helper0.ConnectHost(sw0, gpunodes.Get(2));

    for (int i = 0; i < 8; ++i){
        if (i != 2){
            DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(i, devs0_2.Get(0));
            DynamicCast<GPU>(gpunodes.Get(2))->PushRecvPeerDevice(i, devs0_2.Get(0));
        }
    }

    NetDeviceContainer devs0_3 = link_helper0.ConnectHost(sw0, gpunodes.Get(3));

    for (int i = 0; i < 8; ++i){
        if (i != 3){
            DynamicCast<GPU>(gpunodes.Get(3))->PushSendPeerDevice(i, devs0_3.Get(0));
            DynamicCast<GPU>(gpunodes.Get(3))->PushRecvPeerDevice(i, devs0_3.Get(0));
        }
    }

    NetDeviceContainer devs0_4 = link_helper0.ConnectHost(sw1, gpunodes.Get(4));

    for (int i = 0; i < 8; ++i){
        if (i != 4){
            DynamicCast<GPU>(gpunodes.Get(4))->PushSendPeerDevice(i, devs0_4.Get(0));
            DynamicCast<GPU>(gpunodes.Get(4))->PushRecvPeerDevice(i, devs0_4.Get(0));
        }
    }

    NetDeviceContainer devs0_5 = link_helper0.ConnectHost(sw1, gpunodes.Get(5));

    for (int i = 0; i < 8; ++i){
        if (i != 5){
            DynamicCast<GPU>(gpunodes.Get(5))->PushSendPeerDevice(i, devs0_5.Get(0));
            DynamicCast<GPU>(gpunodes.Get(5))->PushRecvPeerDevice(i, devs0_5.Get(0));
        }
    }

    NetDeviceContainer devs0_6 = link_helper0.ConnectHost(sw1, gpunodes.Get(6));

    for (int i = 0; i < 8; ++i){
        if (i != 6){
            DynamicCast<GPU>(gpunodes.Get(6))->PushSendPeerDevice(i, devs0_6.Get(0));
            DynamicCast<GPU>(gpunodes.Get(6))->PushRecvPeerDevice(i, devs0_6.Get(0));
        }
    }

    NetDeviceContainer devs0_7 = link_helper0.ConnectHost(sw1, gpunodes.Get(7));

    for (int i = 0; i < 8; ++i){
        if (i != 7){
            DynamicCast<GPU>(gpunodes.Get(7))->PushSendPeerDevice(i, devs0_7.Get(0));
            DynamicCast<GPU>(gpunodes.Get(7))->PushRecvPeerDevice(i, devs0_7.Get(0));
        }
    }

    link_helper1.ConnectSwitches(sw0, sw1);



    // TODO: either emit the following from DSL, or make helpers
		// for things not yet wanted to be added to DSL (e.g. if it
		// may require changes in the near future)

		const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_DSL_test.txt";
// const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_n_8_-DGX1-steps_3_rounds_7_chunks_6.txt";
		std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/test1.xml");

		constexpr int N_NODES = 8;
        constexpr DataType::Type dtype = DataType::INT32;
        constexpr int N_CHUNKS = 2;
        const uint32_t INPUT_BYTES = inputBytes;
		int CHUNK_SIZE = (INPUT_BYTES / N_CHUNKS) / DataType::GetSizeBytes(dtype);
        // in elements, so total bytes is CHUNK_SIZE * N_CHUNKS * sizeof(datatype)
        bool CORRECTNESS_CHECK = true;

		// TODO: unsafe stuff
		std::array<NetDeviceContainer*, N_NODES> recv_addr = {&devs0_0, &devs0_1, &devs0_2, &devs0_3, &devs0_4, &devs0_5, &devs0_6, &devs0_7};
		for (int i = 0; i < N_NODES; ++i){
			for (int j = 0; j < N_NODES; ++j){
				if (i != j) DynamicCast<GPU>(gpunodes.Get(i))->PushPeerAddr(j, recv_addr[j]->Get(0)->GetAddress());
			}
		}


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

		// debug dump
		/*
		for (int i = 0; i < topo.GetNNodes(); ++i){
			Ptr<GPU> gpu = DynamicCast<GPU, Node>(topo.GetNode(i));
			gpu->DumpAlgo(logtxt);
			logtxt << std::endl;
			logtxt.flush();
		}
		*/

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
			int src = cur->first.first;
			int dst = cur->first.second;
			uint32_t flow = cur->second;

			if (src / 4 != dst / 4){
				if (dst < 4) sw1->GetCustomImpl()->AddForwardingRule(flow, 4);
				if (dst >= 4) sw0->GetCustomImpl()->AddForwardingRule(flow, 4);
			}
			if (dst < 4){
				sw0->GetCustomImpl()->AddForwardingRule(flow, dst);
			}
			else {
				sw1->GetCustomImpl()->AddForwardingRule(flow, dst % 4);
			}
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
