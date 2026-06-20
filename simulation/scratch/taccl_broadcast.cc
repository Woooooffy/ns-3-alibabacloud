#include "ns3/core-module.h"
#include "ns3/csma-module.h"
//#include "ns3/internet-module.h"
//#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/bridge-module.h"
#include "ns3/distributed-training-module.h"

#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <map>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("MSCCL_TEST");

constexpr int N_NODES = 8;

constexpr int N_START_ELTS = 16 * 16;

constexpr int N_CHUNKS = 2;

constexpr int ALGO_CHUNKS = 2;
constexpr int ALGO_CHUNK_SIZE = N_CHUNKS * N_START_ELTS / ALGO_CHUNKS;

/*
constexpr std::array<std::pair<int, int>, 24> edges = {{
    {0, 1}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 4},
    {1, 2}, {1, 3}, {1, 3}, {2, 3}, {2, 3}, {2, 6},
    {2, 6}, {3, 7}, {4, 5}, {4, 5}, {4, 6}, {4, 7},
    {5, 6}, {5, 7}, {5, 7}, {6, 7}, {6, 7}, {1, 5}
}}; // undir graph
*/
HubAndSpoke<4> tmp;
auto edges = tmp.GetAdjacency();

constexpr bool LOG_DETAIL = false;

struct Dev {
	int start_node;
	int dest_node;
};

static std::ofstream logtxt;

const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Broadcast-HubSpoke1-max.txt";
// const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_n_8_-DGX1-steps_3_rounds_7_chunks_6.txt";
std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/test.xml");

/*
std::map<Ptr<PointToPointNetDevice>, int> recv_counts;
std::map<Ptr<PointToPointNetDevice>, int> send_counts;

  
void
TxTrace(Ptr<PointToPointNetDevice> dev, Ptr<const Packet> p){
    Time now = Simulator::Now();

    std::string msg =
        "Device " + std::to_string(dev->GetNode()->GetId()) +
        " started TX at " +
        std::to_string(now.GetNanoSeconds() / 1000000) + " ms";

    // Console
    // std::cout << msg << std::endl;

    // File
		if (LOG_DETAIL) {
				logtxt << msg << std::endl;
				logtxt.flush();
		}
  	send_counts[dev]++;
}

void
RxTrace(Ptr<PointToPointNetDevice> dev, Ptr<const Packet> p){
	  Time now = Simulator::Now();

    std::string msg =
        "Device " + std::to_string(dev->GetNode()->GetId()) +
        " started RX at " +
        std::to_string(now.GetNanoSeconds()) + " ns";

    // Console
    // std::cout << msg << std::endl;

    // File
    if (LOG_DETAIL){
				logtxt << msg << std::endl;
				logtxt.flush();
		}
		recv_counts[dev]++;
}

*/


int
main(int argc, char* argv[])
{
		LogComponentEnable("DistributedTopo", LOG_LEVEL_ALL);
    NodeContainer gpunodes;
		NodeContainer swtches;
    gpunodes.Create<GPU>(N_NODES);	
		swtches.Create(3);
		// Hosts
		NodeContainer host1;
		NodeContainer host2;
		host1.Add(swtches.Get(0));
		for (int i = 0; i < 4; ++i){
			host1.Add(gpunodes.Get(i));
		}
		host2.Add(swtches.Get(1));
		for (int i = 0; i < 4; ++i){
			host2.Add(gpunodes.Get(i+4));
		}

		// Switch helper
		BridgeHelper bridge;
		bridge.SetDeviceAttribute("EnableLearning", BooleanValue(false));
	
		// topology
		CsmaHelper edge;
		edge.SetDeviceAttribute("Mtu", UintegerValue(9000));
		edge.SetChannelAttribute("DataRate", StringValue("125Gbps")); // 25Gbps usually, 10000bps for lowbw test
		edge.SetChannelAttribute("Delay", TimeValue(NanoSeconds(300)));
		edge.SetQueue("ns3::DropTailQueue",
                "MaxSize", StringValue("500MB"));
		// host1
		NetDeviceContainer host1_swtch_devs;
		NetDeviceContainer host1_terminal_devs;
		// recv devices on gpus from switch
		for (int v = 0; v < 4; ++v){
			for (int _ = 0; _ < edges[4][v]; ++_){
				NodeContainer linkNodes(host1.Get(0), host1.Get(v + 1));
				NetDeviceContainer devices = edge.Install(linkNodes);
				// all links should be between swtch and a gpu
				// required maps for CollectivesApplication
				host1_swtch_devs.Add(devices.Get(0));
				host1_terminal_devs.Add(devices.Get(1));
				Ptr<GPU> nodeV = DynamicCast<GPU>(gpunodes.Get(v));
				// nodeU->PushSendPeerDevice(v, devices.Get(0));
				for (int p = 0; p < 8; ++p){
					if (p!=v) nodeV->PushRecvPeerDevice(p, devices.Get(1));
				}
			}
		}

		// host2
		NetDeviceContainer host2_swtch_devs;
		NetDeviceContainer host2_terminal_devs;
		// recv devices on gpus from switch
		for (int v = 0; v < 4; ++v){
			for (int _ = 0; _ < edges[4][v]; ++_){
				NodeContainer linkNodes(host2.Get(0), host2.Get(v + 1));
				NetDeviceContainer devices = edge.Install(linkNodes);
				// all links should be between swtch and a gpu
				// required maps for CollectivesApplication
				host2_swtch_devs.Add(devices.Get(0));
				host2_terminal_devs.Add(devices.Get(1));
				Ptr<GPU> nodeV = DynamicCast<GPU>(gpunodes.Get(v + 4));
				for (int p = 0; p < 8; ++p){
					if (p!=(v+4)) nodeV->PushRecvPeerDevice(p, devices.Get(1));
				}
			}
		}
		

		// host1
		// send devices on gpus to switch
		for (int u = 0; u < 4; ++u){
			for (int _ = 0; _ < edges[u][4]; ++_){
				NodeContainer linkNodes(host1.Get(u + 1), host1.Get(0));
				NetDeviceContainer devices = edge.Install(linkNodes);
				// all links should be between swtch and a gpu
				// required maps for CollectivesApplication
				host1_swtch_devs.Add(devices.Get(1));
				Ptr<GPU> nodeU = DynamicCast<GPU>(gpunodes.Get(u));
				for (int p = 0; p < 4; ++p){
					if (p != u) {
						nodeU->PushSendPeerDevice(p, devices.Get(0));	
						nodeU->PushPeerAddr(p, host1_terminal_devs.Get(p)->GetAddress());
					}
					nodeU->PushSendPeerDevice(p + 4, devices.Get(0));
					nodeU->PushPeerAddr(p + 4, host2_terminal_devs.Get(p)->GetAddress());
				}	
			}
		}
		Ptr<BridgeNetDevice> bridge_dev = DynamicCast<BridgeNetDevice>(bridge.Install(host1.Get(0), host1_swtch_devs).Get(0));	

		// host2
		// send devices on gpus to switch
		for (int u = 0; u < 4; ++u){
			for (int _ = 0; _ < edges[u][4]; ++_){
				NodeContainer linkNodes(host2.Get(u + 1), host2.Get(0));
				NetDeviceContainer devices = edge.Install(linkNodes);
				// all links should be between swtch and a gpu
				// required maps for CollectivesApplication
				host2_swtch_devs.Add(devices.Get(1));
				Ptr<GPU> nodeU = DynamicCast<GPU>(gpunodes.Get(u + 4));
				for (int p = 0; p < 4; ++p){
					if (p + 4 != u) {
						nodeU->PushSendPeerDevice(p + 4, devices.Get(0));	
						nodeU->PushPeerAddr(p + 4, host2_terminal_devs.Get(p)->GetAddress());
					}
					nodeU->PushSendPeerDevice(p, devices.Get(0));
					nodeU->PushPeerAddr(p, host1_terminal_devs.Get(p)->GetAddress());
				}	
			}
		}
		Ptr<BridgeNetDevice> bridge_dev2 = DynamicCast<BridgeNetDevice>(bridge.Install(host2.Get(0), host2_swtch_devs).Get(0));

		// set up two swtches to central swtch
		CsmaHelper remote_edge;
		remote_edge.SetDeviceAttribute("Mtu", UintegerValue(9000));
		remote_edge.SetChannelAttribute("DataRate", StringValue("9Gbps")); // 25Gbps usually, 10000bps for lowbw test
		remote_edge.SetChannelAttribute("Delay", TimeValue(NanoSeconds(1500)));
		remote_edge.SetQueue("ns3::DropTailQueue",
                "MaxSize", StringValue("500MB"));		
		NetDeviceContainer central_swtch_devs;
		NetDeviceContainer tmp = remote_edge.Install(NodeContainer(swtches.Get(0), swtches.Get(2)));
		bridge_dev->AddBridgePort(tmp.Get(0));
		central_swtch_devs.Add(tmp.Get(1));
		tmp = remote_edge.Install(NodeContainer(swtches.Get(1), swtches.Get(2)));
		bridge_dev2->AddBridgePort(tmp.Get(0));
		central_swtch_devs.Add(tmp.Get(1));
		Ptr<BridgeNetDevice> bridge_central = DynamicCast<BridgeNetDevice>(bridge.Install(swtches.Get(2), central_swtch_devs).Get(0));
		for (uint32_t i = 0; i < host1_terminal_devs.GetN(); ++i){
			Mac48Address addr = Mac48Address::ConvertFrom(host1_terminal_devs.Get(i)->GetAddress());
			bridge_dev->AddEntry(addr, host1_swtch_devs.Get(i));
			bridge_dev2->AddEntry(addr, bridge_dev2->GetBridgePort(bridge_dev2->GetNBridgePorts()-1));
			bridge_central->AddEntry(addr, bridge_central->GetBridgePort(0));
		}
		for (uint32_t i = 0; i < host2_terminal_devs.GetN(); ++i){
			Mac48Address addr = Mac48Address::ConvertFrom(host2_terminal_devs.Get(i)->GetAddress());
			bridge_dev2->AddEntry(addr, host2_swtch_devs.Get(i));
			bridge_dev->AddEntry(addr, bridge_dev->GetBridgePort(bridge_dev->GetNBridgePorts()-1));
			bridge_central->AddEntry(addr, bridge_central->GetBridgePort(1));
		}

		PacketSocketHelper packetSocket;
		packetSocket.Install(gpunodes);

		// parse algo xml
		TopoNodeSet topo(gpunodes);
		AlgoParseResult result = ParseAlgoFromXml(XML_ALGO.c_str(), topo);
		if (result != AlgoParseResult::ALGO_PARSE_SUCCESS) NS_LOG_ERROR("Encountered issue in parsing XML algorithm, error code " << result);
		
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
		app_helper.SetAttribute("ChunkSize", UintegerValue(ALGO_CHUNK_SIZE));
		ApplicationContainer apps = app_helper.Install<GPU>(gpunodes);

		for (int i = 0; i < N_NODES; ++i){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(apps.Get(i));
			// TODO fix hardcoded values
			app->AllocBuffer(N_START_ELTS * N_CHUNKS * N_NODES, app->GetSrcBuffer());
			app->AllocBuffer(N_START_ELTS * N_CHUNKS * N_NODES, app->GetDstBuffer());
			app->AllocBuffer(N_START_ELTS * N_CHUNKS * N_NODES, app->GetScratchBuffer());
			if (i == 0){
				int* ptr = (int*) app->GetSrcBuffer()->dataBuffer;
				int* outptr = (int*) app->GetDstBuffer()->dataBuffer;
				for (int n = 0; n < N_NODES; ++n){
					for (int c = 0; c < N_CHUNKS; ++c){
						int chunk = c * N_START_ELTS;
						int val = n * 16 * 16 * 16 + c * 16 * 16;
						for (int j = 0; j < N_START_ELTS; ++j){
							ptr[n * N_CHUNKS * N_START_ELTS + chunk + j] = val + j;
							outptr[n * N_CHUNKS * N_START_ELTS + chunk + j] = val + j;
						}
					}
				}
				app->DumpBuffer(app->GetSrcBuffer(), logtxt);
			}
		}

    // Simulator::Stop(Seconds(120));
		NS_LOG_UNCOND("Starting simulation");
    Simulator::Run();

		Time simTime = Simulator::Now();
		std::cout << "Total simulated time: "
          << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;

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
		
    Simulator::Destroy();
		NS_LOG_UNCOND("Done simulation");

    return 0;
}
