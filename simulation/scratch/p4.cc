
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/ethernet-switch-module.h"
#include "ns3/internet-module.h"
#include "ns3/internet-apps-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("P4Scratch");


//	n1---sw1---sw2---n2


int main(int argc, char* argv[]){
	
	LogComponentEnable("P4Scratch", LOG_LEVEL_INFO);
	LogComponentEnable("P4SwitchNetDevice", LOG_LEVEL_ALL);
	// LogComponentEnable("P4Helper", LOG_LEVEL_ALL);
	LogComponentEnable("CustomSwitchImpl", LOG_LEVEL_ALL);
	// LogComponentEnable("Ping", LOG_LEVEL_DEBUG);
	LogComponentEnable("SwitchedEthernetHostDevice", LOG_LEVEL_ALL);

	
	NS_LOG_INFO("Running simulation..."); 
	NodeContainer hosts(2);
	NodeContainer swtchs(2);
	
	P4Helper sw_helper;
	sw_helper.SetDeviceAttribute("EnableCustomImpl", BooleanValue(true));
	SwitchedEthernetHelper eth_helper;

	NetDeviceContainer sw_dev1 = sw_helper.Install(swtchs.Get(0));
	NetDeviceContainer sw_dev2 = sw_helper.Install(swtchs.Get(1));

	NS_LOG_INFO("Installed switches.");

	Ptr<P4SwitchNetDevice> sw1 = DynamicCast<P4SwitchNetDevice>(sw_dev1.Get(0));
	Ptr<P4SwitchNetDevice> sw2 = DynamicCast<P4SwitchNetDevice>(sw_dev2.Get(0));


	NetDeviceContainer host_dev1 = eth_helper.Install(sw1, hosts.Get(0));
	NetDeviceContainer host_dev2 = eth_helper.Install(sw2, hosts.Get(1));
	NetDeviceContainer host_devs;
	host_devs.Add(host_dev1);
	host_devs.Add(host_dev2);

	eth_helper.ConnectSwitches(sw1, sw2);

	sw1->GetCustomImpl()->AddAddrForwarding(host_dev1.Get(0)->GetAddress(), 0);
	sw1->GetCustomImpl()->AddAddrForwarding(host_dev2.Get(0)->GetAddress(), 1);

	sw2->GetCustomImpl()->AddAddrForwarding(host_dev1.Get(0)->GetAddress(), 1);
	sw2->GetCustomImpl()->AddAddrForwarding(host_dev2.Get(0)->GetAddress(), 0);

	NS_LOG_INFO("Installed links & devs to hosts.");
	// Internet
	InternetStackHelper internet;
  internet.Install(hosts);

	Ipv4AddressHelper ipv4;
	ipv4.SetBase("10.0.0.0", "255.255.255.0");

	Ipv4InterfaceContainer ifaces = ipv4.Assign(host_devs);
	
	// Ping
	PingHelper ping(ifaces.GetAddress(1));

	ApplicationContainer apps =
			ping.Install(hosts.Get(0));

	apps.Start(Seconds(1));
	apps.Stop(Seconds(5));	

	NS_LOG_INFO("Set up ping app.");

	Simulator::Run();
	Simulator::Destroy();
	NS_LOG_INFO("Simulation done."); 
	return 0;
}
