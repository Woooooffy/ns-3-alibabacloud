#include "ns3/core-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/tap-bridge-module.h"
#include "ns3/csma-module.h"

#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <map>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("RingExample");

constexpr int N_NODES = 16;

/*
constexpr std::array<std::pair<int, int>, 16> edges = {{
    {0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6},
    {6, 7}, {7, 8}, {8, 9}, {9, 10}, {10, 11}, {11, 12},
    {12, 13}, {13, 14}, {14, 15}, {15, 0}
}}; // undir graph
*/

constexpr std::array<std::pair<int, int>, 16> edges = {{
    {0, 3}, {3, 6}, {6, 9}, {9, 12}, {12, 15}, {15, 2},
    {2, 5}, {5, 8}, {8, 11}, {11, 14}, {14, 1}, {1, 4},
    {4, 7}, {7, 10}, {10, 13}, {13, 0}
}}; // undir graph


struct Dev {
	int start_node;
	int dest_node;
};

const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/ring2-alltoall.txt";

constexpr bool LOG_DETAIL = false;

static std::ofstream logtxt;

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
		if (LOG_DETAIL) logtxt << msg << std::endl;
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
    if (LOG_DETAIL) logtxt << msg << std::endl;
		recv_counts[dev]++;
}

int
main(int argc, char* argv[])
{
	  std::string mode = "UseLocal";
    /*std::string tapName0 = "tap0_host";
		std::string tapName1 = "tap1_host";
    std::string tapName2 = "tap2_host";
		std::string tapName3 = "tap3_host";
    std::string tapName4 = "tap4_host";
		std::string tapName5 = "tap5_host";
    std::string tapName6 = "tap6_host";
		std::string tapName7 = "tap7_host"; */
		//LogComponentEnable("TapBridge", LOG_LEVEL_INFO);
    //CommandLine cmd(__FILE__);
    //cmd.AddValue("mode", "Mode setting of TapBridge", mode);
    //cmd.Parse(argc, argv);

    GlobalValue::Bind("SimulatorImplementationType", StringValue("ns3::RealtimeSimulatorImpl"));
    GlobalValue::Bind("ChecksumEnabled", BooleanValue(true));

    NodeContainer nodes;
    nodes.Create(N_NODES);
		NodeContainer ghost_nodes;
		ghost_nodes.Create(N_NODES);
		// TAP
		CsmaHelper ghost_edge; // eth conn
		ghost_edge.SetChannelAttribute("DataRate", StringValue("1000000000Gbps"));
		ghost_edge.SetChannelAttribute("Delay", TimeValue(NanoSeconds(0)));
    TapBridgeHelper tapBridge; // tap
    tapBridge.SetAttribute("Mode", StringValue(mode));
    InternetStackHelper stack; // ip
    stack.Install(nodes);
		stack.Install(ghost_nodes);
    Ipv4AddressHelper addresses;
    addresses.SetBase("13.0.0.0", "255.255.255.0", "0.0.0.10");
    for (int i = 0; i < N_NODES; ++i) {
			// connect ghost node and node
			NodeContainer node_pair(ghost_nodes.Get(i), nodes.Get(i));
			NetDeviceContainer devices = ghost_edge.Install(node_pair);	
			addresses.Assign(devices);
			addresses.NewNetwork();
			// tap on ghost node
			std::ostringstream oss;
			oss << "tap" << i << "_host";
			std::string name = oss.str();
			tapBridge.SetAttribute("DeviceName", StringValue(name));
  	  tapBridge.Install(ghost_nodes.Get(i), devices.Get(0));	
		}
	
		// topology
		PointToPointHelper edge;
		edge.SetDeviceAttribute("DataRate", StringValue("25Gbps"));
		edge.SetChannelAttribute("Delay", TimeValue(NanoSeconds(200)));
		edge.SetQueue("ns3::DropTailQueue",
                "MaxSize", StringValue("1MB"));
		addresses.SetBase("10.0.1.0", "255.255.255.252");
		for (const auto& [u, v] : edges) {
  	  NodeContainer linkNodes(nodes.Get(u), nodes.Get(v));
	    NetDeviceContainer devices = edge.Install(linkNodes);
			addresses.Assign(devices);
			addresses.NewNetwork();
			for (int i = 0; i < 2; ++i) {
				Ptr<PointToPointNetDevice> dev = DynamicCast<PointToPointNetDevice, NetDevice>(devices.Get(i));
				// initialize counters
				recv_counts[dev] = 0;
				send_counts[dev] = 0;
				// trace sources
				dev->TraceConnectWithoutContext(
            "PhyTxBegin",
            MakeBoundCallback(&TxTrace, dev));

      	dev->TraceConnectWithoutContext(
            "PhyRxEnd",
            MakeBoundCallback(&RxTrace, dev));
			}
		}

		Ipv4GlobalRoutingHelper::PopulateRoutingTables();

		// log file
		logtxt.open(LOG_FILE, std::ios::app);
		if (!logtxt.is_open()){
    	NS_FATAL_ERROR("Failed to open log file");
		}
		chmod(LOG_FILE.c_str(), 0666);

    Simulator::Stop(Seconds(180.));
		NS_LOG_UNCOND("Starting simulation");
    Simulator::Run();
		// print stats
		std::cout << "\n=== Aggregate Stats ===\n";

		for (const auto& [dev, tx] : send_counts){
	    uint32_t nodeId = dev->GetNode()->GetId();

  	  int rx = 0;
    	auto it = recv_counts.find(dev);
    	if (it != recv_counts.end()){
        rx = it->second;
    	}

			std::ostringstream oss;
    	oss << "Node " << nodeId
        << " DeviceAddr " << dev->GetAddress()
        << " TX=" << tx
        << " RX=" << rx;

    	std::string msg = oss.str();

   	 	std::cout << msg << std::endl;
    	logtxt << msg << std::endl;	
			logtxt.flush();
		}
    Simulator::Destroy();
		NS_LOG_UNCOND("Done simulation");

    return 0;
}
