#ifndef TOPO_H
#define TOPO_H

#include "gpu.h"
#include "msccl.h"
#include <vector>
#include "ns3/core-module.h"
#include "ns3/network-module.h"

namespace ns3
{
	enum AlgoParseResult {
		ALGO_PARSE_SUCCESS,
		FILE_READ_ERROR,
		XML_PARSE_ERROR,
		INVALID_USE_ERROR
	};


	class TopoNodeSet {

		public:
		TopoNodeSet();
		TopoNodeSet(NodeContainer& nodes);
		~TopoNodeSet();
		int GetNNodes();
		Ptr<Node> GetNode(int i);
		private:

		int m_nNodes;
		NodeContainer m_nodes;
	};

	AlgoParseResult mscclGetBufferType(const char* str, uint8_t* output);
	AlgoParseResult mscclCheckBufferBounds(int bufferType, int offset, int nInputChunks, int nOutputChunks, int nScratchChunks);
	AlgoParseResult ParseAlgoFromXml(const char* file_path, TopoNodeSet nodes); 
}
#endif
