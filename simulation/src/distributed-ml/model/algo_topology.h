#ifndef ALGO_TOPOLOGY_H
#define ALGO_TOPOLOGY_H

#include "gpu.h"
#include "msccl.h"
#include <map>
#include <set>
#include <tuple>
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

		// One send step's endpoints, recorded by ParseAlgoXml and consumed by ParseSwitchJson.
		// The XML knows flowId -> (sender, peer, channel) but not which link the flow takes;
		// the switch JSON knows flowId -> (switch, out port) but names no channel. Joining the
		// two on the flow id is the only way to recover which of a multi-homed GPU's NICs the
		// schedule intended a given connection to leave by (see ResolveScheduledNics).
		struct FlowEndpoints {
			uint32_t srcGpu;
			int16_t peer;
			int chan;
		};
		// (sender gpu id, peer, channel) -- the identity of one persistent RDMA connection,
		// which is the granularity at which a NIC can be pinned (RdmaHw binds a qp to one NIC
		// for its lifetime). A schedule that sent one such connection's flows out different
		// NICs could not be honored; ResolveScheduledNics detects and reports that rather than
		// silently following whichever flow it saw first.
		struct ConnectionKey {
			uint32_t srcGpu;
			int16_t peer;
			int chan;
			bool operator<(const ConnectionKey& o) const {
				return std::tie(srcGpu, peer, chan) < std::tie(o.srcGpu, o.peer, o.chan);
			}
		};

		NodeContainer m_gpuNodes;
		NodeContainer m_switchNodes;
		int m_nNetDeps = 0;                  // netdepid/netdeps pairs honored, for the parse summary
		int m_nSendSteps = 0;                // send-bearing steps seen, the summary's denominator
		int m_nInputChunks = 0;              // per-GPU input chunk count (i_chunks); tester n_chunks
		int m_nScratchChunks = 0;            // max per-GPU scratch chunk count (s_chunks) over active GPUs
		int m_nChunksPerLoop = 0;            // nchunksperloop from the <algo> root
		int m_nChannels = 0;                 // nchannels from the <algo> root
		std::vector<int> m_activeGpuIds;     // gpu ids with a non-empty algorithm, ascending
		// switch node id -> (neighbor node id -> every outgoing ifIndex reaching that
		// neighbor), built lazily the first time a given switch is touched by
		// ParseSwitchJson. The value is a vector, not a single port, because a realistic
		// fat tree runs several parallel links between the same switch pair (e.g. each
		// leaf's multiple uplinks to one spine) -- keying one port per neighbor would let
		// the last-installed device silently overwrite its siblings.
		// Despite the name this caches GPU ports too: resolving a flow's ingress hop needs the
		// sending GPU's own port map (which of its NICs faces the leaf named by the rule), and
		// the lookup is identical -- walk the node's qbb channels and group ports by neighbor.
		std::map<uint32_t, std::map<uint32_t, std::vector<uint32_t>>> m_switchPortCache;
		// flow id -> the send step's endpoints, filled by ParseAlgoXml. Only send-bearing steps
		// carrying a real flow id are recorded; the matching recv step repeats the same id but
		// would name the receiver as "src".
		std::map<uint32_t, FlowEndpoints> m_flowEndpoints;
		// connection -> the sending GPU's ifIndex the schedule routes it out of, derived by
		// ResolveScheduledNics and published onto the GPU nodes at the end of ParseSwitchJson.
		std::map<ConnectionKey, uint32_t> m_scheduledNic;
		// connections whose flows disagreed about the egress NIC (see ConnectionKey); left
		// unpinned so they keep RdmaHw's ECMP hashing rather than honoring half a schedule.
		std::set<ConnectionKey> m_scheduledNicConflicts;

		std::map<uint32_t, std::vector<uint32_t>>& SwitchPortCache(Ptr<Node> node);
		bool ResolveOutPorts(Ptr<SwitchNode> sw, Ptr<Node> target, std::vector<uint32_t>& outIfIndices);
		bool ResolvePortNeighbor(Ptr<SwitchNode> sw, uint32_t ifIndex, uint32_t& neighborNodeId);
		// If `sw` is the ingress hop of `flowId` (i.e. it neighbors that flow's sending GPU),
		// record the GPU-side NIC facing it as that connection's scheduled egress NIC.
		void ResolveScheduledNics(Ptr<SwitchNode> sw, uint32_t flowId);
		// Push m_scheduledNic onto the GPU nodes and log a one-line summary.
		void PublishScheduledNics();
	};

	AlgoParseResult mscclGetBufferType(const char* str, uint8_t* output);
	AlgoParseResult mscclCheckBufferBounds(int bufferType, int offset, int nInputChunks, int nOutputChunks, int nScratchChunks);
}
#endif
