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

NS_LOG_COMPONENT_DEFINE("ScclExample2");

constexpr int N_NODES = 8;

constexpr std::array<std::pair<int, int>, 8> edges0 = {{
    {0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 4}, {1, 5},
    {1, 6}, {1, 7}}}; // undir graph
constexpr std::array<std::pair<int, int>, 8> edges1 = {{
		{0, 3}, {0, 2}, {3, 5}, {2, 5},
		{1, 4}, {1, 7}, {4, 6}, {6, 7}
}};

struct Dev {
	int start_node;
	int dest_node;
};

static std::ofstream logtxt;

std::map<Ptr<PointToPointNetDevice>, int> recv_counts;
std::map<Ptr<PointToPointNetDevice>, int> send_counts;

constexpr bool LOG_DETAIL = false;  

const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/sccl2-broadcast.txt";

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
		if (LOG_DETAIL) {
				logtxt << msg << std::endl;
				logtxt.flush();
		}
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
		NodeContainer intermediates;
		intermediates.Create(2);
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
		stack.Install(intermediates);
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
		edge.SetDeviceAttribute("DataRate", StringValue("27Gbps"));
		edge.SetChannelAttribute("Delay", TimeValue(NanoSeconds(200)));
		edge.SetQueue("ns3::DropTailQueue",
                "MaxSize", StringValue("10MB"));
		addresses.SetBase("10.0.1.0", "255.255.255.252");
		// within processor
		for (const auto& [u, v] : edges0) {
			NodeContainer linkNodes(intermediates.Get(u), nodes.Get(v));
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
		// between intermediates
		NodeContainer linkNodes(intermediates.Get(0), nodes.Get(1));
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
		// interconnect
		edge.SetDeviceAttribute("DataRate", StringValue("33Gbps"));
		for (const auto& [u, v] : edges1) {
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
		chmod(LOG_FILE.c_str(), 0666);
		if (!logtxt.is_open()){
    	NS_FATAL_ERROR("Failed to open log.txt");
		}

    Simulator::Stop(Seconds(200.));
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
