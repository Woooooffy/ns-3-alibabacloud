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
			CollectiveTester(ApplicationContainer& apps, bool verbose=false, std::ostream& log=std::cout,
			                 const std::vector<int>& passiveGpus = {});
			~CollectiveTester();

			// Redefine which app indices are passive (idle) for subsequent Setup/Verify calls,
			// letting one tester object sweep several subsets over the same topology.
			void SetPassiveGpus(const std::vector<int>& passiveGpus);

			void SetupAllgather(size_t input_elts, int n_chunks);
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
			void SetupAlltoall(size_t input_elts, int n_chunks);
			CollectiveTestResult VerifyAlltoall(size_t input_elts, int n_chunks);

			// Topology-driven overloads; see the allgather pair above. For alltoall the parsed
			// i_chunks equals nchunksperloop, so GetNInputChunks() is exactly the per-rank chunk
			// count the setup/verify split into P destination partitions.
			void SetupAlltoall(AlgoTopology& topo, size_t input_elts);
			CollectiveTestResult VerifyAlltoall(AlgoTopology& topo, size_t input_elts);
		private:
			// (re)builds m_participants / m_nParticipants from m_passive; called by ctor and setter
			void ComputeParticipants();

			ApplicationContainer& m_apps;
			bool m_verbose;
			int m_n_apps;
			std::ostream& m_log;
			std::set<int> m_passive;             // app indices sitting idle this run
			std::vector<int> m_participants;     // active app indices, ascending; vector index == participant rank
			int m_nParticipants;
	}; // collective tester
}// namespace ns3
#endif
