#ifndef COLL_TEST_H
#define COLL_TEST_H
#include "collectives.h"
#include <iostream>
#include <fstream>
#include <set>
#include <vector>

namespace ns3{
	class AlgoTopology; // full definition pulled into collective-tests.cc

	enum CollectiveTestResult{
		TEST_OK,
		TEST_FAILED
	};

	// How much a tester writes to its log stream.
	//
	// VERBOSE dumps every source/destination buffer and one line per mismatching element.
	// At large scale (many participants x large per-rank buffers) that is gigabytes of text:
	// it dominates runtime and can exhaust memory in the stream/file layer, so it is only
	// usable for small debug topologies.
	//
	// MINIMAL dumps no buffers and prints at most maxMismatches mismatch lines, then stops
	// scanning (the verdict is already decided) -- enough to see where the first divergence is.
	// SILENT prints nothing but the final one-line verdict.
	enum class CollectiveLogMode{
		SILENT,
		MINIMAL,
		VERBOSE
	};

	// Drives correctness setup/verification for a collective running on top of an
	// ApplicationContainer of CollectivesApplications.
	//
	// Subsets / passive GPUs:
	//   A topology may hold more GPUs than a given collective actually uses. Pass the
	//   node/app indices that sit idle for this run as `passiveGpus`; the tester then
	//   drives only the remaining (active) apps, so a single large topology setup can be
	//   re-used to test many different subsets of participants and algorithms without
	//   rebuilding the network. The passive apps are left untouched (no buffers allocated,
	//   never verified) -- their algorithm XML is simply expected to contain no threadblocks.
	//
	//   Active apps are ordered by ascending app/container index and assigned a contiguous
	//   participant rank 0..P-1. That rank -- not the raw app index -- is what the value
	//   encoding and output-slice ordering key on, matching the convention that an algorithm
	//   XML generated for P participants numbers its buffer slices 0..P-1 in ascending
	//   gpu-id order. With no passive GPUs the rank equals the app index and behavior is
	//   identical to the whole-topology case.
	class CollectiveTester {
		public:
			// verbose=true maps to CollectiveLogMode::VERBOSE, false to SILENT; use SetLogMode
			// for MINIMAL (or to change the mode later).
			CollectiveTester(ApplicationContainer& apps, bool verbose=false, std::ostream& log=std::cout,
			                 const std::vector<int>& passiveGpus = {});
			~CollectiveTester();

			// Redefine which app indices are passive (idle) for subsequent Setup/Verify calls,
			// letting one tester object sweep several subsets over the same topology.
			void SetPassiveGpus(const std::vector<int>& passiveGpus);

			void SetLogMode(CollectiveLogMode mode){ m_mode = mode; }
			CollectiveLogMode GetLogMode() const { return m_mode; }
			// Mismatch lines printed in MINIMAL mode before verification gives up and returns.
			void SetMaxMismatches(size_t n){ m_maxMismatches = n; }

			// scratch_chunks is the algorithm's declared per-rank s_chunks; <= 0 keeps the legacy
			// behaviour of sizing scratch like the output buffer. Pass it whenever it is known:
			// relay-heavy schedules stage into scratch offsets far beyond the output size.
			void SetupAllgather(size_t input_elts, int n_chunks, int scratch_chunks = 0);
			CollectiveTestResult VerifyAllgather(size_t input_elts, int n_chunks);

			// Topology-driven overloads: n_chunks is read straight from the parsed algorithm
			// (AlgoTopology::GetNInputChunks(), i.e. the per-rank i_chunks) so the chunk count
			// stays a single source of truth shared with the app's ChunkSize configuration and
			// can never drift from the XML. input_elts is still the per-rank input element count.
			void SetupAllgather(AlgoTopology& topo, size_t input_elts);
			CollectiveTestResult VerifyAllgather(AlgoTopology& topo, size_t input_elts);

			// Alltoall: each participant's input is split into P contiguous partitions (one per
			// participant rank); partition d is destined for rank d. After the collective, output
			// partition s holds the data rank s sent to this rank. input_elts is the per-rank input
			// size and equals the per-rank output size. Requires n_chunks % P == 0 so the chunks
			// divide evenly among the P destination partitions.
			// scratch_chunks as in SetupAllgather above.
			void SetupAlltoall(size_t input_elts, int n_chunks, int scratch_chunks = 0);
			CollectiveTestResult VerifyAlltoall(size_t input_elts, int n_chunks);

			// Topology-driven overloads; see the allgather pair above. For alltoall the parsed
			// i_chunks equals nchunksperloop, so GetNInputChunks() is exactly the per-rank chunk
			// count the setup/verify split into P destination partitions.
			void SetupAlltoall(AlgoTopology& topo, size_t input_elts);
			CollectiveTestResult VerifyAlltoall(AlgoTopology& topo, size_t input_elts);
		private:
			// (re)builds m_participants / m_nParticipants from m_passive; called by ctor and setter
			void ComputeParticipants();

			// Records one mismatching element, logging it if the current mode still wants output.
			// Returns false when the scan should stop early: the run is already known to be
			// incorrect, so outside VERBOSE there is nothing to gain from checking the rest.
			bool NoteMismatch(int node, size_t index, int32_t expected, int32_t got);
			// Dumps a buffer only in VERBOSE mode (a full dump is what makes large runs unusable).
			void DumpIfVerbose(Ptr<CollectivesApplication> app, DataBuffer* buf);

			ApplicationContainer& m_apps;
			CollectiveLogMode m_mode;
			size_t m_maxMismatches = 10;
			size_t m_mismatchesLogged = 0;       // reset at the start of each Verify* call
			int m_n_apps;
			std::ostream& m_log;
			std::set<int> m_passive;             // app indices sitting idle this run
			std::vector<int> m_participants;     // active app indices, ascending; vector index == participant rank
			int m_nParticipants;
	}; // collective tester
}// namespace ns3
#endif
