/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

// Network topology
//
// Packets sent to the device "thetap" on the Linux host will be sent to the
// tap bridge on node zero and then emitted onto the ns-3 simulated CSMA
// network.  ARP will be used on the CSMA network to resolve MAC addresses.
// Packets destined for the CSMA device on node zero will be sent to the
// device "thetap" on the linux Host.
//
//  +----------+
//  | external |
//  |  Linux   |
//  |   Host   |
//  |          |
//  | "thetap" |
//  +----------+
//       |           n0            n1            n2            n3
//       |       +--------+    +--------+    +--------+    +--------+
//       +-------|  tap   |    |        |    |        |    |        |
//               | bridge |    |        |    |        |    |        |
//               +--------+    +--------+    +--------+    +--------+
//               |  CSMA  |    |  CSMA  |    |  CSMA  |    |  CSMA  |
//               +--------+    +--------+    +--------+    +--------+
//                   |             |             |             |
//                   |             |             |             |
//                   |             |             |             |
//                   ===========================================
//                                 CSMA LAN 10.1.1
//
// The CSMA device on node zero is:  10.1.1.1
// The CSMA device on node one is:   10.1.1.2
// The CSMA device on node two is:   10.1.1.3
// The CSMA device on node three is: 10.1.1.4
//
// Some simple things to do:
//
// 1) Ping one of the simulated nodes
//
//    ./ns3 run tap-csma&
//    ping 10.1.1.2
//
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/tap-bridge-module.h"
#include "ns3/wifi-module.h"
#include "ns3/point-to-point-module.h"

#include <fstream>
#include <iostream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TapCsmaExample");

std::vector<int> recv_counts;
std::vector<int> send_counts;
/*  
void PrintRxTrace(std::string context, Ptr<const Packet> p,
                  const Address &from, const Address &to)
{
    double now = Simulator::Now().GetSeconds();
    InetSocketAddress src = InetSocketAddress::ConvertFrom(from);
    InetSocketAddress dst = InetSocketAddress::ConvertFrom(to);

    // record total bytes
    bytes_transferred[{from, to}] += p->GetSize();

    NS_LOG_UNCOND("Time " << now
                   << "s: Packet of size " << p->GetSize()
                   << " bytes; from " << src.GetIpv4() << ":" << src.GetPort()
                   << " -> to " << dst.GetIpv4() << ":" << dst.GetPort());
} */



int
main(int argc, char* argv[])
{
	  std::string mode = "UseLocal";
    std::string tapName0 = "tap0_host";
		std::string tapName1 = "tap1_host";

		//LogComponentEnable("TapBridge", LOG_LEVEL_INFO);
    //CommandLine cmd(__FILE__);
    //cmd.AddValue("mode", "Mode setting of TapBridge", mode);
    //cmd.Parse(argc, argv);

    GlobalValue::Bind("SimulatorImplementationType", StringValue("ns3::RealtimeSimulatorImpl"));
    GlobalValue::Bind("ChecksumEnabled", BooleanValue(true));

    NodeContainer nodes;
    nodes.Create(2);

		NodeContainer nodes_ns3;
		nodes_ns3.Create(2);

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", DataRateValue(50000000000));
    csma.SetChannelAttribute("Delay", TimeValue(MilliSeconds(0)));

    NetDeviceContainer devices0 = csma.Install(NodeContainer(nodes.Get(0), nodes_ns3.Get(0)));
		NetDeviceContainer devices1 = csma.Install(NodeContainer(nodes.Get(1), nodes_ns3.Get(1)));

    InternetStackHelper stack;
    stack.Install(nodes);
		stack.Install(nodes_ns3);
		for (uint32_t i = 0; i < nodes_ns3.GetN(); i++) {
  		Ptr<Ipv4> ipv4 = nodes_ns3.Get(i)->GetObject<Ipv4>();
  		ipv4->SetAttribute("IpForward", BooleanValue(true));
		}


    Ipv4AddressHelper addresses;
    addresses.SetBase("13.0.0.0", "255.255.255.0", "0.0.0.10");
		addresses.Assign(devices0);
		addresses.NewNetwork();
		addresses.Assign(devices1);
    TapBridgeHelper tapBridge;
    tapBridge.SetAttribute("Mode", StringValue(mode));
    tapBridge.SetAttribute("DeviceName", StringValue(tapName0));
    tapBridge.Install(nodes.Get(0), devices0.Get(0));
		tapBridge.SetAttribute("DeviceName", StringValue(tapName1));
		tapBridge.Install(nodes.Get(1), devices1.Get(0));

		addresses.SetBase("10.0.0.0", "255.255.255.252");
		PointToPointHelper link;
	  link.SetDeviceAttribute("DataRate", DataRateValue(500000));
    link.SetChannelAttribute("Delay", TimeValue(MilliSeconds(2)));	
		NetDeviceContainer ns3_devs = link.Install(nodes_ns3);
		addresses.Assign(ns3_devs);

		Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Simulator::Stop(Seconds(600.));
		NS_LOG_UNCOND("Starting simulation");
    Simulator::Run();
    Simulator::Destroy();
		NS_LOG_UNCOND("Done simulation");

    return 0;
}
