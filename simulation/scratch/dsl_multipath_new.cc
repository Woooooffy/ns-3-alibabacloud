#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <map>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
#include "ns3/ethernet-switch-module.h"
#include "ns3/distributed-ml-module.h"

using namespace ns3;

int main(int argc, char *argv[]) {
		#ifdef FLOW_ID_TEST
		NS_LOG_COMPONENT_DEFINE("DSL_TEST");
//		LogComponentEnable("CustomSwitchImpl", LOG_LEVEL_ALL);
		LogComponentEnable("DSL_TEST", LOG_LEVEL_ALL);
//		LogComponentEnable("P4SwitchNetDevice", LOG_LEVEL_ALL);
//		LogComponentEnable("SwitchedEthernetHostDevice", LOG_LEVEL_ALL);
//		LogComponentEnable("CollectivesApplication", LOG_LEVEL_ALL);
    NodeContainer gpunodes;
    NodeContainer swtches;
    P4Helper sw_helper;
    sw_helper.SetDeviceAttribute("EnableCustomImpl", BooleanValue(true));

    gpunodes.Create<GPU>(8);
    swtches.Create(6);
    SwitchedEthernetHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper0.SetChannelAttribute("Delay", StringValue("300ns"));
    link_helper0.SetChannelAttribute("DataRate", StringValue("125Gbps"));

    SwitchedEthernetHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper1.SetChannelAttribute("Delay", StringValue("2000ns"));
    link_helper1.SetChannelAttribute("DataRate", StringValue("30Gbps"));

    SwitchedEthernetHelper link_helper2;
    link_helper2.SetDeviceAttribute("Mtu", UintegerValue(9000));
    link_helper2.SetChannelAttribute("Delay", StringValue("2000ns"));
    link_helper2.SetChannelAttribute("DataRate", StringValue("25Gbps"));


    sw_helper.SetDeviceAttribute("Mtu", UintegerValue(9000));
    NetDeviceContainer sw_dev0 = sw_helper.Install(swtches.Get(0));
    Ptr<P4SwitchNetDevice> sw0 = DynamicCast<P4SwitchNetDevice>(sw_dev0.Get(0));
    NetDeviceContainer sw_dev1 = sw_helper.Install(swtches.Get(1));
    Ptr<P4SwitchNetDevice> sw1 = DynamicCast<P4SwitchNetDevice>(sw_dev1.Get(0));
    NetDeviceContainer sw_dev2 = sw_helper.Install(swtches.Get(2));
    Ptr<P4SwitchNetDevice> sw2 = DynamicCast<P4SwitchNetDevice>(sw_dev2.Get(0));
    NetDeviceContainer sw_dev3 = sw_helper.Install(swtches.Get(3));
    Ptr<P4SwitchNetDevice> sw3 = DynamicCast<P4SwitchNetDevice>(sw_dev3.Get(0));
    NetDeviceContainer sw_dev4 = sw_helper.Install(swtches.Get(4));
    Ptr<P4SwitchNetDevice> sw4 = DynamicCast<P4SwitchNetDevice>(sw_dev4.Get(0));
    NetDeviceContainer sw_dev5 = sw_helper.Install(swtches.Get(5));
    Ptr<P4SwitchNetDevice> sw5 = DynamicCast<P4SwitchNetDevice>(sw_dev5.Get(0));

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

    link_helper1.ConnectSwitches(sw0, sw2);

    link_helper1.ConnectSwitches(sw1, sw4);

    link_helper1.ConnectSwitches(sw2, sw3);

    link_helper1.ConnectSwitches(sw3, sw4);

    link_helper2.ConnectSwitches(sw4, sw5);

    link_helper2.ConnectSwitches(sw2, sw5);


    /*
    */
		NS_LOG_INFO("DSL emitted setup completed");

		const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_DSL_test.txt";
// const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_n_8_-DGX1-steps_3_rounds_7_chunks_6.txt";
		std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/test1.xml");

		constexpr int N_NODES = 8;

		constexpr int CHUNK_SIZE = 512;
		constexpr int N_CHUNKS = 2;

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

		// install apps
		CollectivesApplicationHelper app_helper;
		app_helper.SetAttribute("DataType", EnumValue(DataType::INT32));
		app_helper.SetAttribute("ChunkSize", UintegerValue(CHUNK_SIZE));
		ApplicationContainer apps = app_helper.Install<GPU>(gpunodes);

		NS_LOG_INFO("Installed applications");

		// Testing with flow tables
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


		#define SCHEME 2
		std::cout << "Testing scheme " << SCHEME << std::endl;
		std::map<uint32_t, std::vector<int>> shared_trace;


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


		#if SCHEME == 0
		for (auto cur = flowIds.begin(); cur != flowIds.end(); ++cur){
			int src = cur->first.first;
			int dst = cur->first.second;
			uint32_t flow = cur->second;

			if (src < 4 && dst >= 4){
				sw2->GetCustomImpl()->AddForwardingRule(flow, 1);
				sw3->GetCustomImpl()->AddForwardingRule(flow, 1);
				sw4->GetCustomImpl()->AddForwardingRule(flow, 0);
			}
			else if (src >= 4 && dst < 4){
				sw2->GetCustomImpl()->AddForwardingRule(flow, 0);
				sw5->GetCustomImpl()->AddForwardingRule(flow, 1);
				sw4->GetCustomImpl()->AddForwardingRule(flow, 2);
			}
		}
		#elif SCHEME == 1
		for (auto cur = flowIds.begin(); cur != flowIds.end(); ++cur){
			int src = cur->first.first;
			int dst = cur->first.second;
			uint32_t flow = cur->second;

			if (src < 4 && dst >= 4){
				sw2->GetCustomImpl()->AddForwardingRule(flow, 1);
				sw3->GetCustomImpl()->AddForwardingRule(flow, 1);
				sw4->GetCustomImpl()->AddForwardingRule(flow, 0);
			}
			else if (src >= 4 && dst < 4){
				sw2->GetCustomImpl()->AddForwardingRule(flow, 0);
				sw3->GetCustomImpl()->AddForwardingRule(flow, 0);
				sw4->GetCustomImpl()->AddForwardingRule(flow, 1);
			}
		}

		#elif SCHEME == 2
		for (auto cur = flowIds.begin(); cur != flowIds.end(); ++cur){
			int src = cur->first.first;
			int dst = cur->first.second;
			uint32_t flow = cur->second;

			if (src < 4 && dst >= 4){
				if ((src % 2) == 0) {
					sw2->GetCustomImpl()->AddForwardingRule(flow, 1);
					sw3->GetCustomImpl()->AddForwardingRule(flow, 1);
					sw4->GetCustomImpl()->AddForwardingRule(flow, 0);
				}
				else {
					sw2->GetCustomImpl()->AddForwardingRule(flow, 2);
					sw5->GetCustomImpl()->AddForwardingRule(flow, 1);
					sw4->GetCustomImpl()->AddForwardingRule(flow, 0);
				}
			}
			else if (src >= 4 && dst < 4){
				if ((dst % 2) == 0) {
					sw2->GetCustomImpl()->AddForwardingRule(flow, 0);
					sw3->GetCustomImpl()->AddForwardingRule(flow, 0);
					sw4->GetCustomImpl()->AddForwardingRule(flow, 1);
				}
				else{
					sw2->GetCustomImpl()->AddForwardingRule(flow, 0);
					sw5->GetCustomImpl()->AddForwardingRule(flow, 1);
					sw4->GetCustomImpl()->AddForwardingRule(flow, 2);
				}
			}
		}
		#endif

		// Allgather test
		CollectiveTester tester(apps, true, logtxt);
		tester.SetupAllgather(CHUNK_SIZE * N_CHUNKS, N_CHUNKS);
		// Run
		std::cout << "Starting simulation:" << std::endl;
    Simulator::Run();
		// Time and verify
		Time simTime = Simulator::Now();
		std::cout << "Total simulated time: "
          << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;

		CollectiveTestResult allgather_res = tester.VerifyAllgather(CHUNK_SIZE * N_CHUNKS, N_CHUNKS);

		if (allgather_res == CollectiveTestResult::TEST_OK) std::cout << "Allgather verified." << std::endl;
		else std::cout << "Allgather incorrect." << std::endl;



    Simulator::Run();
    Simulator::Destroy();
		#endif
    return 0;
}
