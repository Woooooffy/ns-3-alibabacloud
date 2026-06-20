#include "collective-tests.h"
namespace ns3{

	CollectiveTester::CollectiveTester(ApplicationContainer& apps, bool verbose, std::ostream& log): m_apps(apps), m_verbose(verbose), m_n_apps(apps.GetN()), m_log(log){}

	CollectiveTester::~CollectiveTester(){}

	void CollectiveTester::SetupAllgather(size_t input_elts, int n_chunks){
		NS_ASSERT_MSG((input_elts % n_chunks) == 0, "Input element count not multiple of number of chunks.");
		int n_per_chunk = input_elts / n_chunks;
		size_t output_elts = input_elts * m_n_apps;
		size_t scratch_elts = output_elts;
		for (int i = 0; i < m_n_apps; ++i){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(m_apps.Get(i));
			app->AllocBuffer(input_elts, app->GetSrcBuffer());
			app->AllocBuffer(output_elts, app->GetDstBuffer());
			memset(app->GetDstBuffer()->dataBuffer, 0, output_elts * sizeof(int32_t));
			app->AllocBuffer(scratch_elts, app->GetScratchBuffer());
			int* ptr = (int*) app->GetSrcBuffer()->dataBuffer;
			int* outptr = (int*) app->GetDstBuffer()->dataBuffer;
			for (int c = 0; c < n_chunks; ++c){
				int chunk = c * n_per_chunk;
				int val = i * 16 * 16 * 16 * 16 + c * 16 * 16 * 16;
				for (int j = 0; j < n_per_chunk; ++j){
					ptr[chunk + j] = val + j;
					// TODO properly handle copy
					outptr[i * n_chunks * n_per_chunk + chunk + j] = val + j;
				}
			}
			if (m_verbose) app->DumpBuffer(app->GetSrcBuffer(), m_log);
		} 
	}

	CollectiveTestResult CollectiveTester::VerifyAllgather(size_t input_elts, int n_chunks){
		NS_ASSERT_MSG((input_elts % n_chunks) == 0, "Input element count not multiple of number of chunks.");
		int n_per_chunk = input_elts / n_chunks;

		bool correct = true;

		for (int i = 0; i < m_n_apps; ++i){
			Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication>(m_apps.Get(i));
			DataBuffer* buf = app->GetDstBuffer();
			// correctness check
			int32_t* ptr = (int32_t*) buf->dataBuffer;
			if (buf->len != 1ULL * n_per_chunk * n_chunks * m_n_apps){
				correct = false;
				if (m_verbose) m_log << "Incorrect result on node " << i << ": expected output length " << n_per_chunk * n_chunks * m_n_apps<< ", got " << buf->len << std::endl;
			}
			else{
				for (int n = 0; n < m_n_apps; ++n){
					int node = n * n_chunks * n_per_chunk;
					int node_base = n * 16 * 16 * 16 * 16;
					for (int c = 0; c < n_chunks; ++c){
							int chunk = node + c * n_per_chunk;
							int chunk_base = node_base + c * 16 * 16 * 16;
						for (int j = 0; j < n_per_chunk; ++j){
							if (ptr[chunk + j] != chunk_base + j){
								if (m_verbose) m_log << "Incorrect result on node " << i << " at output " << chunk + j << ": expected " << chunk_base + j << ", got " << ptr[chunk + j] << std::endl;
								correct = false;
							}
						}
					}
				}
			}
			if (m_verbose) app->DumpBuffer(buf, m_log);
		}
		// Unconditional logging
		if (correct){
			m_log << "Allgather result verified." << std::endl;
			return CollectiveTestResult::TEST_OK; 
		}
		else m_log << "Allgather incorrect." << std::endl;
		return CollectiveTestResult::TEST_FAILED;
	}
}
