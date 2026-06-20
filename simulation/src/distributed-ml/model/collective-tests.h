#ifndef COLL_TEST_H
#define COLL_TEST_H
#include "collectives.h"
#include <iostream>
#include <fstream>

namespace ns3{
	enum CollectiveTestResult{
		TEST_OK,
		TEST_FAILED
	};
	class CollectiveTester {
		public:
			CollectiveTester(ApplicationContainer& apps, bool verbose=false, std::ostream& log=std::cout);
			~CollectiveTester();
			void SetupAllgather(size_t input_bytes, int n_chunks);
			CollectiveTestResult VerifyAllgather(size_t input_bytes, int n_chunks);
		private:
			ApplicationContainer& m_apps;
			bool m_verbose;
			int m_n_apps;
			std::ostream& m_log;
	}; // collective tester
}// namespace ns3
#endif
