#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ethernet-switch-module.h"
#include "ns3/distributed-ml-module.h"

#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <map>

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("EXAMPLE_TEST");
    NodeContainer gpunodes;
    NodeContainer swtches;
    P4Helper sw_helper;
    sw_helper.SetDeviceAttribute("EnableCustomImpl", BooleanValue(true));

    gpunodes.Create<GPU>(3);
    swtches.Create(0);
    PointToPointHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(1000));
    link_helper0.SetChannelAttribute("Delay", StringValue("1400ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("25GBps"));
    link_helper0.SetQueue("ns3::DropTailQueue", "MaxSize", QueueSizeValue(QueueSize("50000p")));

    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), gpunodes.Get(1));

    DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(1, devs0_0.Get(0));
    DynamicCast<GPU>(gpunodes.Get(1))->PushRecvPeerDevice(0, devs0_0.Get(1));
    DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(0, devs0_0.Get(1));
    DynamicCast<GPU>(gpunodes.Get(0))->PushRecvPeerDevice(1, devs0_0.Get(0));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerAddr(1, (devs0_0.Get(1))->GetAddress());
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerAddr(0, (devs0_0.Get(0))->GetAddress());

    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), gpunodes.Get(2));

    DynamicCast<GPU>(gpunodes.Get(1))->PushSendPeerDevice(2, devs0_1.Get(0));
    DynamicCast<GPU>(gpunodes.Get(2))->PushRecvPeerDevice(1, devs0_1.Get(1));
    DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(1, devs0_1.Get(1));
    DynamicCast<GPU>(gpunodes.Get(1))->PushRecvPeerDevice(2, devs0_1.Get(0));
    DynamicCast<GPU>(gpunodes.Get(1))->PushPeerAddr(2, (devs0_1.Get(1))->GetAddress());
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerAddr(1, (devs0_1.Get(0))->GetAddress());

    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(0), gpunodes.Get(2));

    DynamicCast<GPU>(gpunodes.Get(0))->PushSendPeerDevice(2, devs0_2.Get(0));
    DynamicCast<GPU>(gpunodes.Get(2))->PushRecvPeerDevice(0, devs0_2.Get(1));
    DynamicCast<GPU>(gpunodes.Get(2))->PushSendPeerDevice(0, devs0_2.Get(1));
    DynamicCast<GPU>(gpunodes.Get(0))->PushRecvPeerDevice(2, devs0_2.Get(0));
    DynamicCast<GPU>(gpunodes.Get(0))->PushPeerAddr(2, (devs0_2.Get(1))->GetAddress());
    DynamicCast<GPU>(gpunodes.Get(2))->PushPeerAddr(0, (devs0_2.Get(0))->GetAddress());


    /*
        n0 -> n1: devs0_0
        n1 -> n2: devs0_1
        n0 -> n2: devs0_2
    */

const std::string LOG_FILE = "/data/commit/graphit/wangyj05/workspace/gloo-ns3-examples/logs/Allgather_DSL_test.txt";
//		std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/test.xml");
		std::string XML_ALGO = "/data/scratch/wangyj05/msccl_xml/single_instance/Allgather_n_3_-Ring_n_3_-steps_1.xml";

//		constexpr int N_NODES = 8;
		constexpr int N_CHUNKS = 1;
    // in elements, so total bytes is CHUNK_SIZE * N_CHUNKS * sizeof(datatype)
		int INPUT_SIZE = 4 * (1 << 20);
		int CHUNK_SIZE = (INPUT_SIZE / N_CHUNKS) / DataType::GetSizeBytes(DataType::INT32);
    constexpr bool CORRECTNESS_CHECK = false;

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
    app_helper.SetAttribute("CorrectnessCheck", BooleanValue(CORRECTNESS_CHECK));
		ApplicationContainer apps = app_helper.Install<GPU>(gpunodes);

    CollectiveTester tester(apps, true, logtxt);
    if (CORRECTNESS_CHECK) {
		  tester.SetupAllgather(CHUNK_SIZE * N_CHUNKS, N_CHUNKS);
    }
    else{
      NS_LOG_UNCOND("Skipping correctness check.");
    }

    Simulator::Run();
		Time simTime = Simulator::Now();
		std::cout << "Total simulated time: "
          << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;

    if (CORRECTNESS_CHECK) {
      CollectiveTestResult allgather_res = tester.VerifyAllgather(CHUNK_SIZE * N_CHUNKS, N_CHUNKS);
      if (allgather_res == CollectiveTestResult::TEST_OK) std::cout << "Allgather verified." << std::endl;
      else std::cout << "Allgather incorrect." << std::endl;
    }

    Simulator::Destroy();
    NS_LOG_UNCOND("Done simulation");

    return 0;
}
