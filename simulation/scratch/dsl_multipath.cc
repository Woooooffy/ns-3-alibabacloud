#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
#include "ns3/bridge-module.h"
#include "ns3/distributed-training-module.h"

#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <map>



/*
gpu0	gpu1	sw3---|		 gpu4	gpu5
	|   |      |    |			 |    |
	swtch0----sw2  sw4-----swtch1
  |   |      | 		|			 |    |
gpu2	gpu3   |---sw5	 gpu6  gpu7
*/

// TODO check backoff
// also check self backoff
// check packet retransmission
// check queue dropping and channel contention
// Try diamond in middle
// try two host two switch setup

using namespace ns3;


std::map<uint32_t, uint32_t> pkt_queue_drops;
std::map<uint32_t, uint32_t> pkt_backoffs;

void MacTxDropTrace(Ptr<const Packet> p){
	pkt_queue_drops[p->GetUid()]++;
}

void MacTxBackoffTrace(Ptr<const Packet> p){
	pkt_backoffs[p->GetUid()]++;
}

int main(int argc, char *argv[]) {
		#ifdef FLOW_ID_TEST

		NS_LOG_COMPONENT_DEFINE("DSL_TEST");

		LogComponentEnable("CollectivesApplication", LOG_LEVEL_ALL);
    NodeContainer gpunodes;
    NodeContainer swtches;

    gpunodes.Create<GPU>(8);
    swtches.Create(6);
    CsmaHelper csma0;
    csma0.SetDeviceAttribute("Mtu", UintegerValue(9000));
    csma0.SetChannelAttribute("Delay", StringValue("300ns"));
    csma0.SetChannelAttribute("DataRate", StringValue("125Gbps"));
		csma0.SetDeviceAttribute("UseDefaultInterframeGap", BooleanValue(false));
		csma0.SetDeviceAttribute("InterframeGap", TimeValue(NanoSeconds(300)));

    CsmaHelper csma1;
    csma1.SetDeviceAttribute("Mtu", UintegerValue(9000));
    csma1.SetChannelAttribute("Delay", StringValue("2000ns"));
    csma1.SetChannelAttribute("DataRate", StringValue("100Gbps"));
		csma1.SetDeviceAttribute("UseDefaultInterframeGap", BooleanValue(false));
		csma1.SetDeviceAttribute("InterframeGap", TimeValue(NanoSeconds(2000)));

    CsmaHelper csma2;
    csma2.SetDeviceAttribute("Mtu", UintegerValue(9000));
    csma2.SetChannelAttribute("Delay", StringValue("2000ns"));
    csma2.SetChannelAttribute("DataRate", StringValue("100Gbps"));
		csma2.SetDeviceAttribute("UseDefaultInterframeGap", BooleanValue(false));
		csma2.SetDeviceAttribute("InterframeGap", TimeValue(NanoSeconds(2000)));

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

		Ptr<CsmaNetDevice> toGpu0 = DynamicCast<CsmaNetDevice>(devs0_1.Get(0));

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
		Ptr<CsmaNetDevice> toGpu1 = DynamicCast<CsmaNetDevice>(devs0_3.Get(0));

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

		Ptr<CsmaNetDevice> toGpu2 = DynamicCast<CsmaNetDevice>(devs0_5.Get(0));

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
		Ptr<CsmaNetDevice> toGpu3 = DynamicCast<CsmaNetDevice>(devs0_7.Get(0));

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

		Ptr<CsmaNetDevice> toGpu4 = DynamicCast<CsmaNetDevice>(devs0_9.Get(0));

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
		Ptr<CsmaNetDevice> toGpu5 = DynamicCast<CsmaNetDevice>(devs0_11.Get(0));

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

		Ptr<CsmaNetDevice> toGpu6 = DynamicCast<CsmaNetDevice>(devs0_13.Get(0));
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

		Ptr<CsmaNetDevice> toGpu7 = DynamicCast<CsmaNetDevice>(devs0_15.Get(0));

    NodeContainer nc1_16;
    nc1_16.Add(swtches.Get(0));
    nc1_16.Add(swtches.Get(2));
    NetDeviceContainer devs1_16 = csma1.Install(nc1_16);

    NodeContainer nc1_17;
    nc1_17.Add(swtches.Get(1));
    nc1_17.Add(swtches.Get(4));
    NetDeviceContainer devs1_17 = csma1.Install(nc1_17);

    NodeContainer nc1_18;
    nc1_18.Add(swtches.Get(2));
    nc1_18.Add(swtches.Get(5));//
    NetDeviceContainer devs1_18 = csma2.Install(nc1_18);

    NodeContainer nc1_19;
    nc1_19.Add(swtches.Get(2));
    nc1_19.Add(swtches.Get(3));
    NetDeviceContainer devs1_19 = csma1.Install(nc1_19);

    NodeContainer nc1_20;
    nc1_20.Add(swtches.Get(3));
    nc1_20.Add(swtches.Get(4));
    NetDeviceContainer devs1_20 = csma1.Install(nc1_20);

    NodeContainer nc1_21;
    nc1_21.Add(swtches.Get(4));
    nc1_21.Add(swtches.Get(5)); //
    NetDeviceContainer devs1_21 = csma2.Install(nc1_21);


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
    bridgePorts_c2_sw.Add(devs1_17.Get(0));
    SmartSwitchHelper bridge_c2_sw;
    bridge_c2_sw.Install(swtches.Get(1), bridgePorts_c2_sw);

    NetDeviceContainer bridgePorts_sw1;
    bridgePorts_sw1.Add(devs1_16.Get(1));
    bridgePorts_sw1.Add(devs1_18.Get(0));
    bridgePorts_sw1.Add(devs1_19.Get(0));
    SmartSwitchHelper bridge_sw1;
    bridge_sw1.Install(swtches.Get(2), bridgePorts_sw1);

    NetDeviceContainer bridgePorts_sw3;
    bridgePorts_sw3.Add(devs1_17.Get(1));
    bridgePorts_sw3.Add(devs1_20.Get(1));
    bridgePorts_sw3.Add(devs1_21.Get(0));
    SmartSwitchHelper bridge_sw3;
    bridge_sw3.Install(swtches.Get(4), bridgePorts_sw3);

    NetDeviceContainer bridgePorts_sw4;
    bridgePorts_sw4.Add(devs1_18.Get(1));
    bridgePorts_sw4.Add(devs1_21.Get(1));
    SmartSwitchHelper bridge_sw4;
    bridge_sw4.Install(swtches.Get(5), bridgePorts_sw4);

    NetDeviceContainer bridgePorts_sw2;
    bridgePorts_sw2.Add(devs1_19.Get(1));
    bridgePorts_sw2.Add(devs1_20.Get(0));
    SmartSwitchHelper bridge_sw2;
    bridge_sw2.Install(swtches.Get(3), bridgePorts_sw2);


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
        c1_sw -> sw1: devs1_16
        c2_sw -> sw3: devs1_17
        sw1 -> sw4: devs1_18
        sw1 -> sw2: devs1_19
        sw2 -> sw3: devs1_20
        sw3 -> sw4: devs1_21
    */
		Config::ConnectWithoutContext(
			"/NodeList/*/DeviceList/*/$ns3::CsmaNetDevice/MacTxBackoff",
			MakeCallback(&MacTxBackoffTrace));

		Config::ConnectWithoutContext(
			"/NodeList/*/DeviceList/*/$ns3::CsmaNetDevice/MacTxDrop",
			MakeCallback(&MacTxDropTrace));


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

		// install apps
		CollectivesApplicationHelper app_helper;
		app_helper.SetAttribute("DataType", EnumValue(DataType::INT32));
		app_helper.SetAttribute("ChunkSize", UintegerValue(CHUNK_SIZE));
		ApplicationContainer apps = app_helper.Install<GPU>(gpunodes);

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


		// switches flow
/*
        c1_sw -> sw1: devs1_16
        c2_sw -> sw3: devs1_17
        sw1 -> sw4: devs1_18
        sw1 -> sw2: devs1_19
        sw2 -> sw3: devs1_20
        sw3 -> sw4: devs1_21
*/
		#define SCHEME 1
		std::cout << "Testing scheme " << SCHEME << std::endl;
		std::map<uint32_t, std::vector<int>> shared_trace;


		Ptr<Node> c1_sw_node = swtches.Get(0);
		Ptr<SmartSwitch> c1_sw = DynamicCast<SmartSwitch>(
				c1_sw_node->GetDevice(c1_sw_node->GetNDevices() - 1));
		c1_sw->DisableLearning();
		c1_sw->SetSharedTraceMap(&shared_trace);

		Ptr<Node> c2_sw_node = swtches.Get(1);
		Ptr<SmartSwitch> c2_sw = DynamicCast<SmartSwitch>(
				c2_sw_node->GetDevice(c2_sw_node->GetNDevices() - 1));
		c2_sw->DisableLearning();
		c2_sw->SetSharedTraceMap(&shared_trace);

		for (auto cur = flowIds.begin(); cur != flowIds.end(); ++cur){
			int dst = cur->first.second;
			uint32_t flow = cur->second;
			switch (dst){
				case 0:
					c1_sw->AddForwardingRule(flow, toGpu0);
					c2_sw->AddForwardingRule(flow, devs1_17.Get(0));
					break;
				case 1:
					c1_sw->AddForwardingRule(flow, toGpu1);
					c2_sw->AddForwardingRule(flow, devs1_17.Get(0));
					break;
				case 2:
					c1_sw->AddForwardingRule(flow, toGpu2);
					c2_sw->AddForwardingRule(flow, devs1_17.Get(0));
					break;
				case 3:
					c1_sw->AddForwardingRule(flow, toGpu3);
					c2_sw->AddForwardingRule(flow, devs1_17.Get(0));
					break;
				case 4:
					c2_sw->AddForwardingRule(flow, toGpu4);
					c1_sw->AddForwardingRule(flow, devs1_16.Get(0));
					break;
				case 5:
					c2_sw->AddForwardingRule(flow, toGpu5);
					c1_sw->AddForwardingRule(flow, devs1_16.Get(0));
					break;
				case 6:
					c2_sw->AddForwardingRule(flow, toGpu6);
					c1_sw->AddForwardingRule(flow, devs1_16.Get(0));
					break;
				case 7:
					c2_sw->AddForwardingRule(flow, toGpu7);
					c1_sw->AddForwardingRule(flow, devs1_16.Get(0));
					break;
			}
		}

		Ptr<Node> sw1_node = swtches.Get(2);
		Ptr<SmartSwitch> sw1 = DynamicCast<SmartSwitch>(
				sw1_node->GetDevice(sw1_node->GetNDevices() - 1));
		sw1->DisableLearning();
		sw1->SetSharedTraceMap(&shared_trace);

		Ptr<Node> sw2_node = swtches.Get(3);
		Ptr<SmartSwitch> sw2 = DynamicCast<SmartSwitch>(
				sw2_node->GetDevice(sw2_node->GetNDevices() - 1));
		sw2->DisableLearning();
		sw2->SetSharedTraceMap(&shared_trace);

		Ptr<Node> sw3_node = swtches.Get(4);
		Ptr<SmartSwitch> sw3 = DynamicCast<SmartSwitch>(
				sw3_node->GetDevice(sw3_node->GetNDevices() - 1));
		sw3->DisableLearning();
		sw3->SetSharedTraceMap(&shared_trace);

		Ptr<Node> sw4_node = swtches.Get(5);
		Ptr<SmartSwitch> sw4 = DynamicCast<SmartSwitch>(
				sw4_node->GetDevice(sw4_node->GetNDevices() - 1));
		sw4->DisableLearning();
		sw4->SetSharedTraceMap(&shared_trace);

		#if SCHEME == 0
		for (auto cur = flowIds.begin(); cur != flowIds.end(); ++cur){
			int src = cur->first.first;
			int dst = cur->first.second;
			uint32_t flow = cur->second;

			if (src < 4 && dst >= 4){
				sw1->AddForwardingRule(flow, devs1_19.Get(0));
				sw2->AddForwardingRule(flow, devs1_20.Get(0));
				sw3->AddForwardingRule(flow, devs1_17.Get(1));
			}
			else if (src >= 4 && dst < 4){
				sw1->AddForwardingRule(flow, devs1_16.Get(1));
				// sw2->AddForwardingRule(flow, devs1_19.Get(1));
				sw3->AddForwardingRule(flow, devs1_21.Get(0));
				sw4->AddForwardingRule(flow, devs1_18.Get(1));
			}
		}
		#elif SCHEME == 1
		for (auto cur = flowIds.begin(); cur != flowIds.end(); ++cur){
			int src = cur->first.first;
			int dst = cur->first.second;
			uint32_t flow = cur->second;

			if (src < 4 && dst >= 4){
				sw1->AddForwardingRule(flow, devs1_19.Get(0));
				sw2->AddForwardingRule(flow, devs1_20.Get(0));
				sw3->AddForwardingRule(flow, devs1_17.Get(1));
			}
			else if (src >= 4 && dst < 4){
				sw1->AddForwardingRule(flow, devs1_16.Get(1));
				sw2->AddForwardingRule(flow, devs1_19.Get(1));
				sw3->AddForwardingRule(flow, devs1_20.Get(1));
				// sw4->AddForwardingRule(flow, devs1_18.Get(1));
			}
		}

		#elif SCHEME == 2
		for (auto cur = flowIds.begin(); cur != flowIds.end(); ++cur){
			int src = cur->first.first;
			int dst = cur->first.second;
			uint32_t flow = cur->second;

			if (src < 4 && dst >= 4){
				sw3->AddForwardingRule(flow, devs1_17.Get(1));
				if ((src % 2) == 0) {
					sw1->AddForwardingRule(flow, devs1_19.Get(0));
					sw2->AddForwardingRule(flow, devs1_20.Get(0));
				}
				else {
					sw1->AddForwardingRule(flow, devs1_18.Get(0));
					sw4->AddForwardingRule(flow, devs1_21.Get(1));
				}
			}
			else if (src >= 4 && dst < 4){
				sw1->AddForwardingRule(flow, devs1_16.Get(1));
				if ((dst % 2) == 0) {
					sw2->AddForwardingRule(flow, devs1_19.Get(1));
					sw3->AddForwardingRule(flow, devs1_20.Get(1));
				}
				else{
					sw3->AddForwardingRule(flow, devs1_21.Get(0));
					sw4->AddForwardingRule(flow, devs1_18.Get(1));
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

/*
		for (const auto& [key, vec] : shared_trace) {
        std::cout << key << ": [";
        for (size_t i = 0; i < vec.size(); ++i) {
            std::cout << vec[i];
            if (i + 1 < vec.size()) {
                std::cout << ", ";
            }
        }
        std::cout << "]\n";
    }
*/


    Simulator::Destroy();

		std::cout << "\n=== Per-packet Backoff Counts ===\n";
		for (const auto &kv : pkt_backoffs)
		{
				std::cout << "Packet " << kv.first
									<< " backoffs: " << kv.second << "\n";
		}

		std::cout << "\n=== Per-packet Drop Counts ===\n";
		for (const auto &kv : pkt_queue_drops)
		{
				std::cout << "Packet " << kv.first
									<< " drops: " << kv.second << "\n";
		}
    return 0;
		#endif
}
