/*
 * SPDX-License-Identifier: GPL-2.0-only
 */
 
// Network topology
//
//        n0     n1
//        |      |
//       ----------
//       | Switch |
//       ----------
//        |      |
//        n2     n3
//
//
// - CBR/UDP flows from n0 to n1 and from n3 to n0
// - DropTail queues
// - Tracing of queues and packet receptions to file "csma-bridge.tr"
 
#include "ns3/applications-module.h"
#include "ns3/bridge-module.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-apps-module.h"
 
#include <fstream>
#include <iostream>
 
/**
 * @file
 * @ingroup bridge
 * Bridge example connecting four nodes,
 * with the bridge acting as a simple switch.
 */
 
using namespace ns3;
 
NS_LOG_COMPONENT_DEFINE("BridgeExample");
 
int
main(int argc, char* argv[])
{
    //
    // Users may find it convenient to turn on explicit debugging
    // for selected modules; the below lines suggest how to do this
    //
#if 0
  LogComponentEnable ("CsmaBridgeExample", LOG_LEVEL_INFO);
#endif
 
    //
    // Allow the user to override any of the defaults and the above Bind() at
    // run-time, via command-line arguments
    //
    CommandLine cmd(__FILE__);
    cmd.Parse(argc, argv);
 
    //
    // Explicitly create the nodes required by the topology (shown above).
    //
    NS_LOG_INFO("Create nodes.");
    NodeContainer terminals;
    terminals.Create(4);
 
    NodeContainer swtches;
    swtches.Create(1);
 
    NS_LOG_INFO("Build Topology");
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", DataRateValue(500000000000000));
    csma.SetChannelAttribute("Delay", TimeValue(MilliSeconds(1)));
 
    // Create the csma links, from each terminal to the switch
 
    NetDeviceContainer terminalDevices;
    NetDeviceContainer switchDevices;
 
    for (int i = 0; i < 4; ++i)
    {
        NetDeviceContainer link = csma.Install(NodeContainer(terminals.Get(i), swtches));
        terminalDevices.Add(link.Get(0));
        switchDevices.Add(link.Get(1));
    }
 
    // Create the bridge netdevice, which will do the packet switching
    Ptr<Node> switchNode = swtches.Get(0);
    BridgeHelper bridge;
		bridge.Install(switchNode, switchDevices);

    // Add internet stack to the terminals
    InternetStackHelper internet;
    internet.Install(terminals);
 
    // We've got the "hardware" in place.  Now we need to add IP addresses.
    //
    NS_LOG_INFO("Assign IP Addresses.");
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    ipv4.Assign(terminalDevices);
 
		// ping for connection verification
		for (uint32_t i = 1; i < terminals.GetN(); ++i){
			PingHelper ping(
      	terminals.Get(i)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal());

			ping.SetAttribute("Interval", TimeValue(Seconds(1.0)));
			ping.SetAttribute("Size", UintegerValue(64));
			ping.SetAttribute("Count", UintegerValue(5));

    	ApplicationContainer apps = ping.Install(terminals.Get(0));
    	apps.Start(Seconds(1));
    	apps.Stop(Seconds(20));
  }
    //
    // Now, do the actual simulation.
    //
		Simulator::Stop(Seconds(21));
    NS_LOG_INFO("Run Simulation.");
    Simulator::Run();
    Simulator::Destroy();
    NS_LOG_INFO("Done.");
 
    return 0;
}
