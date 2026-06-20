#include "ns3/core-module.h"
#include "ns3/csma-module.h"
// #include "ns3/point-to-point-module.h"
//#include "ns3/internet-module.h"
//#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/tap-bridge-module.h"
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

constexpr int N_START_ELTS = 16;

constexpr int N_CHUNKS = 6;

constexpr int ALGO_CHUNKS = 6;
constexpr int ALGO_CHUNK_SIZE = N_CHUNKS * N_START_ELTS / ALGO_CHUNKS;

/*
constexpr std::array<std::pair<int, int>, 24> edges = {{
    {0, 1}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 4},
    {1, 2}, {1, 3}, {1, 3}, {2, 3}, {2, 3}, {2, 6},
    {2, 6}, {3, 7}, {4, 5}, {4, 5}, {4, 6}, {4, 7},
    {5, 6}, {5, 7}, {5, 7}, {6, 7}, {6, 7}, {1, 5}
}}; // undir graph
*/
// FullyConnected<N_NODES> tmp;
// Ring<N_NODES> tmp;
// Line<N_NODES> tmp;
// auto edges = tmp.GetAdjacency();
auto edges = DGX1.GetAdjacency();

constexpr bool LOG_DETAIL = false;

struct Dev {
	int start_node;
	int dest_node;
};

static std::ofstream logtxt;

// const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_n_8_-DGX1-steps_3_rounds_7_chunks_6";
const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/tmp.txt"; 
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
    NodeContainer nodes;
    nodes.Create<GPU>(N_NODES);	
	
		// topology
		CsmaHelper edge;
		edge.SetDeviceAttribute("Mtu", UintegerValue(9000));
		edge.SetChannelAttribute("DataRate", StringValue("25Gbps")); // 25Gbps usually, 10000bps for lowbw test
		edge.SetChannelAttribute("Delay", TimeValue(NanoSeconds(200)));
		edge.SetDeviceAttribute("UseDefaultInterframeGap", BooleanValue(false));
		edge.SetDeviceAttribute("InterframeGap", TimeValue(NanoSeconds(200)));
		edge.SetQueue("ns3::DropTailQueue",
                "MaxSize", StringValue("500MB"));

		for (int u = 0; u < N_NODES; ++u){
			for (int v = 0; v < N_NODES; ++v){
				for (int _ = 0; _ < edges[u][v]; ++_){
			  	NodeContainer linkNodes(nodes.Get(u), nodes.Get(v));
					NetDeviceContainer devices = edge.Install(linkNodes);
					// TODO: move setup to Topo maybe?
					// required maps for CollectivesApplication
					Ptr<GPU> nodeU = DynamicCast<GPU>(nodes.Get(u));
					Ptr<GPU> nodeV = DynamicCast<GPU>(nodes.Get(v));
					nodeU->PushSendPeerDevice(v, devices.Get(0));
					nodeV->PushRecvPeerDevice(u, devices.Get(1));
					nodeU->PushPeerAddr(v, devices.Get(1)->GetAddress());
					// nodeV->PushPeerAddr(u, devices.Get(0)->GetAddress());	
					// DynamicCast<SimpleNetDevice>(devices.Get(0))->SetMtu(9000);
					// DynamicCast<SimpleNetDevice>(devices.Get(1))->SetMtu(9000);					
					/*
					DynamicCast<CsmaNetDevice>(devices.Get(0))->SetBackoffParams(
							NanoSeconds(100), // slotTime (smaller)
							1,                // minSlots
							2,                // maxSlots
							1000,             // maxRetries
							10                // ceiling
					);
					DynamicCast<CsmaNetDevice>(devices.Get(1))->SetBackoffParams(
							NanoSeconds(100), // slotTime (smaller)
							1,                // minSlots
							2,                // maxSlots
							1000,             // maxRetries
							10                // ceiling
					);
					*/
					// DynamicCast<CsmaNetDevice>(devices.Get(1))->SetInterframeGap(NanoSeconds(200));
					// DynamicCast<CsmaNetDevice>(devices.Get(0))->SetInterframeGap(NanoSeconds(200));
				}
			}
		}
		/*
		for (const auto& [u, v] : edges) {
  	  NodeContainer linkNodes(nodes.Get(u), nodes.Get(v));
	    NetDeviceContainer devices = edge.Install(linkNodes);
			// TODO: move setup to Topo maybe?
			// required maps for CollectivesApplication
			Ptr<GPU> nodeU = DynamicCast<GPU>(nodes.Get(u));
			Ptr<GPU> nodeV = DynamicCast<GPU>(nodes.Get(v));
			nodeU->PushSendPeerDevice(v, devices.Get(0));
			nodeV->PushRecvPeerDevice(u, devices.Get(1));
			nodeU->PushPeerAddr(v, devices.Get(1)->GetAddress());
			// nodeV->PushPeerAddr(u, devices.Get(0)->GetAddress());
		}
		*/

		// Ipv4GlobalRoutingHelper::PopulateRoutingTables();

		PacketSocketHelper packetSocket;
		packetSocket.Install(nodes);

		// parse algo xml
		TopoNodeSet topo(nodes);
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
		ApplicationContainer apps = app_helper.Install<GPU>(nodes);

		for (int i = 0; i < N_NODES; ++i){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(apps.Get(i));
			// TODO fix hardcoded values
			app->AllocBuffer(N_START_ELTS * N_CHUNKS, app->GetSrcBuffer());
			app->AllocBuffer(N_START_ELTS * N_CHUNKS * N_NODES, app->GetDstBuffer());
			app->AllocBuffer(N_START_ELTS * N_CHUNKS * N_NODES, app->GetScratchBuffer());
			int* ptr = (int*) app->GetSrcBuffer()->dataBuffer;
			for (int c = 0; c < N_CHUNKS; ++c){
				int chunk = c * N_START_ELTS;
				int val = i * 16 * 16 + c * 16;
				for (int j = 0; j < N_START_ELTS; ++j){
					ptr[chunk + j] = val + j;
				}
			}
			app->DumpBuffer(app->GetSrcBuffer(), logtxt);
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
					int node_base = n * 16 * 16;
					for (int c = 0; c < N_CHUNKS; ++c){
							int chunk = node + c * N_START_ELTS;
							int chunk_base = node_base + c * 16;
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
