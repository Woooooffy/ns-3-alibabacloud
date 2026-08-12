#include "collective-tests.h"
#include "algo_topology.h"
#include <algorithm>
namespace ns3{

	CollectiveTester::CollectiveTester(ApplicationContainer& apps, bool verbose, std::ostream& log,
	                                   const std::vector<int>& passiveGpus)
		: m_apps(apps), m_mode(verbose ? CollectiveLogMode::VERBOSE : CollectiveLogMode::SILENT),
		  m_n_apps(apps.GetN()), m_log(log){
		SetPassiveGpus(passiveGpus);
	}

	CollectiveTester::~CollectiveTester(){}

	bool CollectiveTester::NoteMismatch(int node, size_t index, int32_t expected, int32_t got){
		if (m_mode == CollectiveLogMode::VERBOSE ||
		    (m_mode == CollectiveLogMode::MINIMAL && m_mismatchesLogged < m_maxMismatches)){
			m_log << "Incorrect result on node " << node << " at output " << index
			      << ": expected " << expected << ", got " << got << std::endl;
		}
		++m_mismatchesLogged;
		if (m_mode == CollectiveLogMode::VERBOSE) return true;
		if (m_mode == CollectiveLogMode::MINIMAL && m_mismatchesLogged >= m_maxMismatches){
			m_log << "Stopping verification after " << m_maxMismatches
			      << " mismatches (minimal log mode)." << std::endl;
		}
		// SILENT stops on the first mismatch; MINIMAL once its quota is used up.
		return m_mode == CollectiveLogMode::MINIMAL && m_mismatchesLogged < m_maxMismatches;
	}

	void CollectiveTester::DumpIfVerbose(Ptr<CollectivesApplication> app, DataBuffer* buf){
		if (m_mode == CollectiveLogMode::VERBOSE) app->DumpBuffer(buf, m_log);
	}

	void CollectiveTester::SetPassiveGpus(const std::vector<int>& passiveGpus){
		m_passive.clear();
		for (int idx : passiveGpus){
			NS_ASSERT_MSG(idx >= 0 && idx < m_n_apps, "Passive GPU index " << idx << " out of range [0, " << m_n_apps << ").");
			m_passive.insert(idx);
		}
		ComputeParticipants();
	}

	void CollectiveTester::ComputeParticipants(){
		m_participants.clear();
		for (int i = 0; i < m_n_apps; ++i){
			if (m_passive.find(i) == m_passive.end()) m_participants.push_back(i);
		}
		m_nParticipants = (int) m_participants.size();
	}

	void CollectiveTester::SetupAllgather(size_t input_elts, int n_chunks, int scratch_chunks){
		NS_ASSERT_MSG((input_elts % n_chunks) == 0, "Input element count not multiple of number of chunks.");
		int n_per_chunk = input_elts / n_chunks;
		int P = m_nParticipants;
		size_t output_elts = input_elts * P;
		// see the note in SetupAlltoall: s_chunks, when known, is the authority on scratch size
		size_t scratch_elts = output_elts;
		if (scratch_chunks > 0) scratch_elts = std::max(scratch_elts, (size_t) scratch_chunks * n_per_chunk);
		// r is the participant rank (0..P-1); the raw app index is m_participants[r].
		// Data is keyed by rank so it lands in the algorithm's rank-indexed output slice.
		for (int r = 0; r < P; ++r){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(m_apps.Get(m_participants[r]));
			app->AllocBuffer(input_elts, app->GetSrcBuffer());
			app->AllocBuffer(output_elts, app->GetDstBuffer());
			memset(app->GetDstBuffer()->dataBuffer, 0, output_elts * sizeof(int32_t));
			app->AllocBuffer(scratch_elts, app->GetScratchBuffer());
			int* ptr = (int*) app->GetSrcBuffer()->dataBuffer;
			int* outptr = (int*) app->GetDstBuffer()->dataBuffer;
			for (int c = 0; c < n_chunks; ++c){
				int chunk = c * n_per_chunk;
				int val = r * 16 * 16 * 16 * 16 + c * 16 * 16 * 16;
				for (int j = 0; j < n_per_chunk; ++j){
					ptr[chunk + j] = val + j;
					// TODO properly handle copy
					// self slice lives at output offset r * input_elts (== r * n_chunks * n_per_chunk)
					outptr[r * n_chunks * n_per_chunk + chunk + j] = val + j;
				}
			}
			DumpIfVerbose(app, app->GetSrcBuffer());
		}
	}

	CollectiveTestResult CollectiveTester::VerifyAllgather(size_t input_elts, int n_chunks){
		NS_ASSERT_MSG((input_elts % n_chunks) == 0, "Input element count not multiple of number of chunks.");
		int n_per_chunk = input_elts / n_chunks;
		int P = m_nParticipants;

		bool correct = true;
		bool keepScanning = true;
		m_mismatchesLogged = 0;

		for (int r = 0; r < P && keepScanning; ++r){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(m_apps.Get(m_participants[r]));
			DataBuffer* buf = app->GetDstBuffer();
			// correctness check
			int32_t* ptr = (int32_t*) buf->dataBuffer;
			if (buf->len != 1ULL * n_per_chunk * n_chunks * P){
				correct = false;
				if (m_mode != CollectiveLogMode::SILENT) m_log << "Incorrect result on node " << m_participants[r] << ": expected output length " << n_per_chunk * n_chunks * P << ", got " << buf->len << std::endl;
			}
			else{
				for (int n = 0; n < P && keepScanning; ++n){
					int node = n * n_chunks * n_per_chunk;
					int node_base = n * 16 * 16 * 16 * 16;
					for (int c = 0; c < n_chunks && keepScanning; ++c){
							int chunk = node + c * n_per_chunk;
							int chunk_base = node_base + c * 16 * 16 * 16;
						for (int j = 0; j < n_per_chunk; ++j){
							if (ptr[chunk + j] != chunk_base + j){
								correct = false;
								if (!NoteMismatch(m_participants[r], chunk + j, chunk_base + j, ptr[chunk + j])){
									keepScanning = false;
									break;
								}
							}
						}
					}
				}
			}
			DumpIfVerbose(app, buf);
		}
		// Unconditional logging
		if (correct){
			m_log << "Allgather result verified." << std::endl;
			return CollectiveTestResult::TEST_OK;
		}
		else m_log << "Allgather incorrect." << std::endl;
		return CollectiveTestResult::TEST_FAILED;
	}

	void CollectiveTester::SetupAllgather(AlgoTopology& topo, size_t input_elts){
		SetupAllgather(input_elts, topo.GetNInputChunks(), topo.GetNScratchChunks());
	}

	CollectiveTestResult CollectiveTester::VerifyAllgather(AlgoTopology& topo, size_t input_elts){
		return VerifyAllgather(input_elts, topo.GetNInputChunks());
	}

	void CollectiveTester::SetupAlltoall(size_t input_elts, int n_chunks, int scratch_chunks){
		int P = m_nParticipants;
		NS_ASSERT_MSG((input_elts % n_chunks) == 0, "Input element count not multiple of number of chunks.");
		NS_ASSERT_MSG((n_chunks % P) == 0, "Chunk count not multiple of number of participants; cannot split evenly per destination.");
		int n_per_chunk = input_elts / n_chunks;
		int chunks_per_dest = n_chunks / P;         // chunks in each destination partition
		int partition_size = chunks_per_dest * n_per_chunk; // == input_elts / P
		// input == output size for alltoall
		size_t output_elts = input_elts;
		// Scratch is sized from the algorithm's declared s_chunks when the caller supplies it
		// (topo-driven path). Algorithms that relay through scratch — a ring alltoall, say —
		// declare far more scratch chunks than a rank's input holds, and the old input-sized
		// default made those steps write past the end of the buffer.
		size_t scratch_elts = input_elts;
		if (scratch_chunks > 0) scratch_elts = std::max(scratch_elts, (size_t) scratch_chunks * n_per_chunk);
		// src = participant rank of the sender. Its input is split into P partitions; partition
		// d holds the data destined for rank d, encoded (src, d, chunk, elem) so the receiver can
		// verify both who sent it and which partition it belongs to.
		for (int src = 0; src < P; ++src){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(m_apps.Get(m_participants[src]));
			app->AllocBuffer(input_elts, app->GetSrcBuffer());
			app->AllocBuffer(output_elts, app->GetDstBuffer());
			memset(app->GetDstBuffer()->dataBuffer, 0, output_elts * sizeof(int32_t));
			app->AllocBuffer(scratch_elts, app->GetScratchBuffer());
			int* ptr = (int*) app->GetSrcBuffer()->dataBuffer;
			int* outptr = (int*) app->GetDstBuffer()->dataBuffer;
			for (int d = 0; d < P; ++d){
				int part = d * partition_size;
				int part_base = src * 16 * 16 * 16 * 16 + d * 16 * 16 * 16;
				for (int c = 0; c < chunks_per_dest; ++c){
					int chunk = part + c * n_per_chunk;
					int chunk_base = part_base + c * 16 * 16;
					for (int j = 0; j < n_per_chunk; ++j){
						ptr[chunk + j] = chunk_base + j;
						// TODO properly handle copy
						// self partition (d == src) stays local: pre-seed our own output slot src
						if (d == src) outptr[src * partition_size + c * n_per_chunk + j] = chunk_base + j;
					}
				}
			}
			DumpIfVerbose(app, app->GetSrcBuffer());
		}
	}

	CollectiveTestResult CollectiveTester::VerifyAlltoall(size_t input_elts, int n_chunks){
		int P = m_nParticipants;
		NS_ASSERT_MSG((input_elts % n_chunks) == 0, "Input element count not multiple of number of chunks.");
		NS_ASSERT_MSG((n_chunks % P) == 0, "Chunk count not multiple of number of participants; cannot split evenly per destination.");
		int n_per_chunk = input_elts / n_chunks;
		int chunks_per_dest = n_chunks / P;
		int partition_size = chunks_per_dest * n_per_chunk;

		bool correct = true;
		bool keepScanning = true;
		m_mismatchesLogged = 0;

		// dst is the participant rank of the receiver being checked. Its output partition s must
		// hold what rank s placed in ITS partition d==dst, i.e. value keyed (src=s, dest=dst, ...).
		for (int dst = 0; dst < P && keepScanning; ++dst){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(m_apps.Get(m_participants[dst]));
			DataBuffer* buf = app->GetDstBuffer();
			int32_t* ptr = (int32_t*) buf->dataBuffer;
			if (buf->len != 1ULL * input_elts){
				correct = false;
				if (m_mode != CollectiveLogMode::SILENT) m_log << "Incorrect result on node " << m_participants[dst] << ": expected output length " << input_elts << ", got " << buf->len << std::endl;
			}
			else{
				for (int s = 0; s < P && keepScanning; ++s){
					int part = s * partition_size;
					int part_base = s * 16 * 16 * 16 * 16 + dst * 16 * 16 * 16;
					for (int c = 0; c < chunks_per_dest && keepScanning; ++c){
						int chunk = part + c * n_per_chunk;
						int chunk_base = part_base + c * 16 * 16;
						for (int j = 0; j < n_per_chunk; ++j){
							if (ptr[chunk + j] != chunk_base + j){
								correct = false;
								if (!NoteMismatch(m_participants[dst], chunk + j, chunk_base + j, ptr[chunk + j])){
									keepScanning = false;
									break;
								}
							}
						}
					}
				}
			}
			DumpIfVerbose(app, buf);
		}
		// Unconditional logging
		if (correct){
			m_log << "Alltoall result verified." << std::endl;
			return CollectiveTestResult::TEST_OK;
		}
		else m_log << "Alltoall incorrect." << std::endl;
		return CollectiveTestResult::TEST_FAILED;
	}

	void CollectiveTester::SetupAlltoall(AlgoTopology& topo, size_t input_elts){
		SetupAlltoall(input_elts, topo.GetNInputChunks(), topo.GetNScratchChunks());
	}

	CollectiveTestResult CollectiveTester::VerifyAlltoall(AlgoTopology& topo, size_t input_elts){
		return VerifyAlltoall(input_elts, topo.GetNInputChunks());
	}
}
