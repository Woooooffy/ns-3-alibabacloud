#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
#include "ns3/bridge-module.h"
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

		#ifdef FLOW_ID_TEST

		NS_LOG_COMPONENT_DEFINE("DSL_TEST");
		LogComponentEnable("CollectivesApplication", LOG_LEVEL_ALL);
//		LogComponentEnable("PacketSocket", LOG_LEVEL_ALL);
    NodeContainer gpunodes;
    NodeContainer swtches;
    
    gpunodes.Create<GPU>(8);
    swtches.Create(2);
    CsmaHelper csma0;
    csma0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    csma0.SetChannelAttribute("Delay", StringValue("300ns"));
    csma0.SetChannelAttribute("DataRate", StringValue("125Gbps"));
    
    CsmaHelper csma1;
    csma1.SetDeviceAttribute("Mtu", UintegerValue(9000));
    csma1.SetChannelAttribute("Delay", StringValue("1500ns"));
    csma1.SetChannelAttribute("DataRate", StringValue("9Gbps"));
    
    NodeContainer nc0_0;
    nc0_0.Add(gpunodes.Get(0));
    nc0_0.Add(swtches.Get(0));
    NetDeviceContainer devs0_0 = csma0.Install(nc0_0);
    
    for (int i = 0; i < 8; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(i, devs0_0.Get(0));
        }
    }
    NodeContainer nc0_1;
    nc0_1.Add(swtches.Get(0));
    nc0_1.Add(gpunodes.Get(0));
    NetDeviceContainer devs0_1 = csma0.Install(nc0_1);
    
    for (int i = 0; i < 8; ++i){
        if (i != 0){
            DynamicCast<GPU>(gpunodes.Get(0))->PushRecvPeerDevice(i, devs0_1.Get(1));
        }
    }
    NodeContainer nc0_2;
    nc0_2.Add(gpunodes.Get(1));
    nc0_2.Add(swtches.Get(0));
    NetDeviceContainer devs0_2 = csma0.Install(nc0_2);
    
    for (int i = 0; i < 8; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(i, devs0_2.Get(0));
        }
    }
    NodeContainer nc0_3;
    nc0_3.Add(swtches.Get(0));
    nc0_3.Add(gpunodes.Get(1));
    NetDeviceContainer devs0_3 = csma0.Install(nc0_3);
    
    for (int i = 0; i < 8; ++i){
        if (i != 1){
            DynamicCast<GPU>(gpunodes.Get(1))->PushRecvPeerDevice(i, devs0_3.Get(1));
        }
    }
    NodeContainer nc0_4;
    nc0_4.Add(gpunodes.Get(2));
    nc0_4.Add(swtches.Get(0));
    NetDeviceContainer devs0_4 = csma0.Install(nc0_4);
    
    for (int i = 0; i < 8; ++i){
        if (i != 2){
            DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(i, devs0_4.Get(0));
        }
    }
    NodeContainer nc0_5;
    nc0_5.Add(swtches.Get(0));
    nc0_5.Add(gpunodes.Get(2));
    NetDeviceContainer devs0_5 = csma0.Install(nc0_5);
    
    for (int i = 0; i < 8; ++i){
        if (i != 2){
            DynamicCast<GPU>(gpunodes.Get(2))->PushRecvPeerDevice(i, devs0_5.Get(1));
        }
    }
    NodeContainer nc0_6;
    nc0_6.Add(gpunodes.Get(3));
    nc0_6.Add(swtches.Get(0));
    NetDeviceContainer devs0_6 = csma0.Install(nc0_6);
    
    for (int i = 0; i < 8; ++i){
        if (i != 3){
            DynamicCast<GPU>(gpunodes.Get(3))->PushSendPeerDevice(i, devs0_6.Get(0));
        }
    }
    NodeContainer nc0_7;
    nc0_7.Add(swtches.Get(0));
    nc0_7.Add(gpunodes.Get(3));
    NetDeviceContainer devs0_7 = csma0.Install(nc0_7);
    
    for (int i = 0; i < 8; ++i){
        if (i != 3){
            DynamicCast<GPU>(gpunodes.Get(3))->PushRecvPeerDevice(i, devs0_7.Get(1));
        }
    }
    NodeContainer nc0_8;
    nc0_8.Add(gpunodes.Get(4));
    nc0_8.Add(swtches.Get(1));
    NetDeviceContainer devs0_8 = csma0.Install(nc0_8);
    
    for (int i = 0; i < 8; ++i){
        if (i != 4){
            DynamicCast<GPU>(gpunodes.Get(4))->PushSendPeerDevice(i, devs0_8.Get(0));
        }
    }
    NodeContainer nc0_9;
    nc0_9.Add(swtches.Get(1));
    nc0_9.Add(gpunodes.Get(4));
    NetDeviceContainer devs0_9 = csma0.Install(nc0_9);
    
    for (int i = 0; i < 8; ++i){
        if (i != 4){
            DynamicCast<GPU>(gpunodes.Get(4))->PushRecvPeerDevice(i, devs0_9.Get(1));
        }
    }
    NodeContainer nc0_10;
    nc0_10.Add(gpunodes.Get(5));
    nc0_10.Add(swtches.Get(1));
    NetDeviceContainer devs0_10 = csma0.Install(nc0_10);
    
    for (int i = 0; i < 8; ++i){
        if (i != 5){
            DynamicCast<GPU>(gpunodes.Get(5))->PushSendPeerDevice(i, devs0_10.Get(0));
        }
    }
    NodeContainer nc0_11;
    nc0_11.Add(swtches.Get(1));
    nc0_11.Add(gpunodes.Get(5));
    NetDeviceContainer devs0_11 = csma0.Install(nc0_11);
    
    for (int i = 0; i < 8; ++i){
        if (i != 5){
            DynamicCast<GPU>(gpunodes.Get(5))->PushRecvPeerDevice(i, devs0_11.Get(1));
        }
    }
    NodeContainer nc0_12;
    nc0_12.Add(gpunodes.Get(6));
    nc0_12.Add(swtches.Get(1));
    NetDeviceContainer devs0_12 = csma0.Install(nc0_12);
    
    for (int i = 0; i < 8; ++i){
        if (i != 6){
            DynamicCast<GPU>(gpunodes.Get(6))->PushSendPeerDevice(i, devs0_12.Get(0));
        }
    }
    NodeContainer nc0_13;
    nc0_13.Add(swtches.Get(1));
    nc0_13.Add(gpunodes.Get(6));
    NetDeviceContainer devs0_13 = csma0.Install(nc0_13);
    
    for (int i = 0; i < 8; ++i){
        if (i != 6){
            DynamicCast<GPU>(gpunodes.Get(6))->PushRecvPeerDevice(i, devs0_13.Get(1));
        }
    }
    NodeContainer nc0_14;
    nc0_14.Add(gpunodes.Get(7));
    nc0_14.Add(swtches.Get(1));
    NetDeviceContainer devs0_14 = csma0.Install(nc0_14);
    
    for (int i = 0; i < 8; ++i){
        if (i != 7){
            DynamicCast<GPU>(gpunodes.Get(7))->PushSendPeerDevice(i, devs0_14.Get(0));
        }
    }
    NodeContainer nc0_15;
    nc0_15.Add(swtches.Get(1));
    nc0_15.Add(gpunodes.Get(7));
    NetDeviceContainer devs0_15 = csma0.Install(nc0_15);
    
    for (int i = 0; i < 8; ++i){
        if (i != 7){
            DynamicCast<GPU>(gpunodes.Get(7))->PushRecvPeerDevice(i, devs0_15.Get(1));
        }
    }
    NodeContainer nc1_16;
    nc1_16.Add(swtches.Get(0));
    nc1_16.Add(swtches.Get(1));
    NetDeviceContainer devs1_16 = csma1.Install(nc1_16);
    
    
    // Bridge setup for switches
    NetDeviceContainer bridgePorts_c1_sw;
    bridgePorts_c1_sw.Add(devs0_0.Get(1));
    bridgePorts_c1_sw.Add(devs0_1.Get(0));
    bridgePorts_c1_sw.Add(devs0_2.Get(1));
    bridgePorts_c1_sw.Add(devs0_3.Get(0));
    bridgePorts_c1_sw.Add(devs0_4.Get(1));
    bridgePorts_c1_sw.Add(devs0_5.Get(0));
    bridgePorts_c1_sw.Add(devs0_6.Get(1));
    bridgePorts_c1_sw.Add(devs0_7.Get(0));
    bridgePorts_c1_sw.Add(devs1_16.Get(0));
    SmartSwitchHelper bridge_c1_sw;
    bridge_c1_sw.Install(swtches.Get(0), bridgePorts_c1_sw);
    
    NetDeviceContainer bridgePorts_c2_sw;
    bridgePorts_c2_sw.Add(devs0_8.Get(1));
    bridgePorts_c2_sw.Add(devs0_9.Get(0));
    bridgePorts_c2_sw.Add(devs0_10.Get(1));
    bridgePorts_c2_sw.Add(devs0_11.Get(0));
    bridgePorts_c2_sw.Add(devs0_12.Get(1));
    bridgePorts_c2_sw.Add(devs0_13.Get(0));
    bridgePorts_c2_sw.Add(devs0_14.Get(1));
    bridgePorts_c2_sw.Add(devs0_15.Get(0));
    bridgePorts_c2_sw.Add(devs1_16.Get(1));
    SmartSwitchHelper bridge_c2_sw;
    bridge_c2_sw.Install(swtches.Get(1), bridgePorts_c2_sw);
    
    
    /*
        c1_n1 -> c1_sw: devs0_0
        c1_sw -> c1_n1: devs0_1
        c1_n2 -> c1_sw: devs0_2
        c1_sw -> c1_n2: devs0_3
        c1_n3 -> c1_sw: devs0_4
        c1_sw -> c1_n3: devs0_5
        c1_n4 -> c1_sw: devs0_6
        c1_sw -> c1_n4: devs0_7
        c2_n1 -> c2_sw: devs0_8
        c2_sw -> c2_n1: devs0_9
        c2_n2 -> c2_sw: devs0_10
        c2_sw -> c2_n2: devs0_11
        c2_n3 -> c2_sw: devs0_12
        c2_sw -> c2_n3: devs0_13
        c2_n4 -> c2_sw: devs0_14
        c2_sw -> c2_n4: devs0_15
        c1_sw -> c2_sw: devs1_16
    */

		// TODO: either emit the following from DSL, or make helpers
		// for things not yet wanted to be added to DSL (e.g. if it
		// may require changes in the near future)
		
		const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_DSL_test.txt";
// const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_n_8_-DGX1-steps_3_rounds_7_chunks_6.txt";
		std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/test1.xml");

		constexpr int N_NODES = 8;

		constexpr int CHUNK_SIZE = 512;
		constexpr int N_CHUNKS = 2;

		// TODO: unsafe stuff
		std::array<NetDeviceContainer*, N_NODES> recv_addr = {&devs0_1, &devs0_3, &devs0_5, &devs0_7, &devs0_9, &devs0_11, &devs0_13, &devs0_15};
		for (int i = 0; i < N_NODES; ++i){
			for (int j = 0; j < N_NODES; ++j){
				if (i != j) DynamicCast<GPU>(gpunodes.Get(i))->PushPeerAddr(j, recv_addr[j]->Get(1)->GetAddress());
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
		for (int i = 0; i < topo.GetNNodes(); ++i){
			Ptr<GPU> gpu = DynamicCast<GPU, Node>(topo.GetNode(i));
			gpu->DumpAlgo(logtxt);
			logtxt << std::endl;
			logtxt.flush();
		}

		// install apps
		CollectivesApplicationHelper app_helper;
		app_helper.SetAttribute("DataType", EnumValue(DataType::INT32));
		app_helper.SetAttribute("ChunkSize", UintegerValue(CHUNK_SIZE));
		ApplicationContainer apps = app_helper.Install<GPU>(gpunodes);


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
/*
		for (int i = 0; i < N_NODES; ++i){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(apps.Get(i));
			app->AllocBuffer(N_START_ELTS * N_CHUNKS, app->GetSrcBuffer());
			app->AllocBuffer(N_START_ELTS * N_CHUNKS * N_NODES, app->GetDstBuffer());
			app->AllocBuffer(N_START_ELTS * N_CHUNKS * N_NODES, app->GetScratchBuffer());
			int* ptr = (int*) app->GetSrcBuffer()->dataBuffer;
			int* outptr = (int*) app->GetDstBuffer()->dataBuffer;
			for (int c = 0; c < N_CHUNKS; ++c){
				int chunk = c * N_START_ELTS;
				int val = i * 16 * 16 * 16 + c * 16 * 16;
				for (int j = 0; j < N_START_ELTS; ++j){
					ptr[chunk + j] = val + j;
					outptr[i * N_CHUNKS * N_START_ELTS + chunk + j] = val + j;
				}
			}
			app->DumpBuffer(app->GetSrcBuffer(), logtxt);
		}    
*/
  
		CollectiveTester tester(apps, true, logtxt);
		tester.SetupAllgather(CHUNK_SIZE * N_CHUNKS, N_CHUNKS);
    Simulator::Run();
		Time simTime = Simulator::Now();
		std::cout << "Total simulated time: "
          << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;

		CollectiveTestResult allgather_res = tester.VerifyAllgather(CHUNK_SIZE * N_CHUNKS, N_CHUNKS);
		
		if (allgather_res == CollectiveTestResult::TEST_OK) std::cout << "Allgather verified." << std::endl;
		else std::cout << "Allgather incorrect." << std::endl;

/*
		// dst buffer dump & correctness check
		bool correct = true;

		for (int i = 0; i < N_NODES; ++i){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(apps.Get(i));
			DataBuffer* buf = app->GetDstBuffer();
			// correctness check
			int32_t* ptr = (int32_t*) buf->dataBuffer;
			if (buf->len != N_START_ELTS * N_CHUNKS * N_NODES){
				correct = false;
				std::cout << "Incorrect result on node " << i << ": expected output length " << N_START_ELTS * N_CHUNKS * N_NODES << ", got " << buf->len << std::endl;
			}
			else{
				for (int n = 0; n < N_NODES; ++n){
					int node = n * N_CHUNKS * N_START_ELTS;
					int node_base = n * 16 * 16 * 16;
					for (int c = 0; c < N_CHUNKS; ++c){
							int chunk = node + c * N_START_ELTS;
							int chunk_base = node_base + c * 16 * 16;
						for (int j = 0; j < N_START_ELTS; ++j){
							if (ptr[chunk + j] != chunk_base + j){
								std::cout << "Incorrect result on node " << i << " at output " << chunk + j << ": expected " << chunk_base + j << ", got " << ptr[chunk + j] << std::endl;
								correct = false;
							}
						}
					}
				}
			}
			app->DumpBuffer(buf, logtxt);
		}
		if (correct){
			std::cout << "Allgather result verified." << std::endl;
		}
*/	
		
    Simulator::Destroy();
		NS_LOG_UNCOND("Done simulation");


		#endif
    return 0;
}

