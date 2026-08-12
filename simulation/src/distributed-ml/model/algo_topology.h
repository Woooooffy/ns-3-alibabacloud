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

		// Algorithm-level parameters captured from the parsed XML. Valid only after a
		// successful ParseAlgoXml (0 / empty before then). Exposing them here makes the parsed
		// algorithm the single source of truth for the chunk count, so the app's ChunkSize and
		// the tester's setup/verify no longer have to be hand-kept in sync with the XML.
		//
		// GetNInputChunks() is the per-GPU input chunk count (the <gpu> i_chunks attribute) and
		// is the value the CollectiveTester wants: it equals the number of chunks each rank
		// contributes. For alltoall this coincides with nchunksperloop; for allgather it does
		// not (there nchunksperloop == ngpus * i_chunks), so use this rather than
		// GetNChunksPerLoop() to drive the tester.
		int GetNInputChunks() const { return m_nInputChunks; }
		// Largest per-GPU s_chunks over the active GPUs. The scratch buffer the tester allocates
		// must hold this many chunks, otherwise steps staging into high scratch offsets trip the
		// bounds check in CollectivesApplication::GetBufferPtrRawBytes.
		int GetNScratchChunks() const { return m_nScratchChunks; }
		int GetNChunksPerLoop() const { return m_nChunksPerLoop; }
		int GetNChannels() const { return m_nChannels; }
		// GPU ids carrying a non-empty algorithm (>=1 threadblock), ascending. These are the
		// active participants; any other GPU in the container is passive for this algorithm.
		const std::vector<int>& GetActiveGpuIds() const { return m_activeGpuIds; }

		AlgoParseResult ParseAlgoXml(const char* xmlFilePath);
		AlgoParseResult ParseSwitchJson(const char* jsonFilePath);

		private:

		NodeContainer m_gpuNodes;
		NodeContainer m_switchNodes;
		int m_nInputChunks = 0;              // per-GPU input chunk count (i_chunks); tester n_chunks
		int m_nScratchChunks = 0;            // max per-GPU scratch chunk count (s_chunks) over active GPUs
		int m_nChunksPerLoop = 0;            // nchunksperloop from the <algo> root
		int m_nChannels = 0;                 // nchannels from the <algo> root
		std::vector<int> m_activeGpuIds;     // gpu ids with a non-empty algorithm, ascending
		// switch node id -> (neighbor node id -> outgoing ifIndex), built lazily
		// the first time a given switch is touched by ParseSwitchJson
		std::map<uint32_t, std::map<uint32_t, uint32_t>> m_switchPortCache;

		bool ResolveOutPort(Ptr<SwitchNode> sw, Ptr<Node> target, uint32_t& outIfIndex);
	};

	AlgoParseResult mscclGetBufferType(const char* str, uint8_t* output);
	AlgoParseResult mscclCheckBufferBounds(int bufferType, int offset, int nInputChunks, int nOutputChunks, int nScratchChunks);
}
#endif
