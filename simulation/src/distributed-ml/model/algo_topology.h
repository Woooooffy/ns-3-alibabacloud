#ifndef ALGO_TOPOLOGY_H
#define ALGO_TOPOLOGY_H

#include "gpu.h"
#include "msccl.h"
#include <map>
#include <vector>
#include "ns3/core-module.h"
#include "ns3/network-module.h"

namespace ns3
{
	enum AlgoParseResult {
		ALGO_PARSE_SUCCESS,
		FILE_READ_ERROR,
		XML_PARSE_ERROR,
		JSON_PARSE_ERROR,
		INVALID_USE_ERROR
	};

	class SwitchNode;

	// Represents, parses, and sets up the algorithm-level details of one example:
	// the GPU-side MSCCL algorithm (XML) and, optionally, the switch-side custom
	// flow-forwarding routing (JSON).
	class AlgoTopology {
		public:
		AlgoTopology();
		explicit AlgoTopology(NodeContainer& gpuNodes);
		AlgoTopology(NodeContainer& gpuNodes, NodeContainer& switchNodes);
		~AlgoTopology();

		int GetNGpuNodes();
		Ptr<Node> GetGpuNode(int i);
		int GetNSwitchNodes();
		Ptr<Node> GetSwitchNode(int i);

		AlgoParseResult ParseAlgoXml(const char* xmlFilePath);
		AlgoParseResult ParseSwitchJson(const char* jsonFilePath);

		private:

		NodeContainer m_gpuNodes;
		NodeContainer m_switchNodes;
		// switch node id -> (neighbor node id -> outgoing ifIndex), built lazily
		// the first time a given switch is touched by ParseSwitchJson
		std::map<uint32_t, std::map<uint32_t, uint32_t>> m_switchPortCache;

		bool ResolveOutPort(Ptr<SwitchNode> sw, Ptr<Node> target, uint32_t& outIfIndex);
	};

	AlgoParseResult mscclGetBufferType(const char* str, uint8_t* output);
	AlgoParseResult mscclCheckBufferBounds(int bufferType, int offset, int nInputChunks, int nOutputChunks, int nScratchChunks);
}
#endif
