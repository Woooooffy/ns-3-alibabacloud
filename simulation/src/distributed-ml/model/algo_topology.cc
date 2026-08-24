#include "algo_topology.h"
#ifdef HAVE_LIBXML2
#include <libxml/parser.h>
#include <libxml/tree.h>
#endif
#include "json.hpp"
#include "ns3/switch-node.h"
#include "ns3/qbb-channel.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <utility>

namespace ns3
{
	NS_LOG_COMPONENT_DEFINE("AlgoTopo");
	#ifdef HAVE_LIBXML2
	#define XML_GET_PROP_STR(node, prop, out)                    \
  do {                                                       \
    xmlChar* v = xmlGetProp(node, BAD_CAST prop);            \
    if (!v) return AlgoParseResult::XML_PARSE_ERROR;                         \
    out = (const char*)v;                                    \
  } while (0)

	#define XML_GET_PROP_INT(node, prop, out)                    \
  do {                                                       \
    xmlChar* v = xmlGetProp(node, BAD_CAST prop);            \
    if (!v) return AlgoParseResult::XML_PARSE_ERROR;                         \
    out = atoi((const char*)v);                              \
    xmlFree(v);                                              \
  } while (0)
	#else

	#define XML_GET_PROP_STR(node, prop, out) \
	do { \
		NS_FATAL_ERROR("XML support not enabled"); \
	} while (0)

	#define XML_GET_PROP_INT(node, prop, out) \
	do { \
		NS_FATAL_ERROR("XML support not enabled"); \
	} while (0)
	#endif

	AlgoTopology::AlgoTopology(){}

	AlgoTopology::AlgoTopology(NodeContainer& gpuNodes): m_gpuNodes(gpuNodes){}

	AlgoTopology::AlgoTopology(NodeContainer& gpuNodes, NodeContainer& switchNodes): m_gpuNodes(gpuNodes), m_switchNodes(switchNodes){}

	AlgoTopology::~AlgoTopology(){}

	int AlgoTopology::GetNGpuNodes(){
		return m_gpuNodes.GetN();
	}

	Ptr<Node> AlgoTopology::GetGpuNode(int i){
		// ns3 native .Geti() does not perform bound check
		if (i < 0 || (uint32_t) i >= m_gpuNodes.GetN()){
			NS_LOG_ERROR("Accessing out-of-range index " << i << " on AlgoTopology with " << m_gpuNodes.GetN() << " gpu nodes.");
			return nullptr;
		}
		return m_gpuNodes.Get(i);
	}

	int AlgoTopology::GetNSwitchNodes(){
		return m_switchNodes.GetN();
	}

	Ptr<Node> AlgoTopology::GetSwitchNode(int i){
		if (i < 0 || (uint32_t) i >= m_switchNodes.GetN()){
			NS_LOG_ERROR("Accessing out-of-range index " << i << " on AlgoTopology with " << m_switchNodes.GetN() << " switch nodes.");
			return nullptr;
		}
		return m_switchNodes.Get(i);
	}

	AlgoParseResult mscclGetBufferType(const char* str, uint8_t* output){
  	if (strcmp(str, "i") == 0){
    	*output = MSCCL_INPUT_BUFFER;
  	} else if (strcmp(str, "o") == 0) {
    	*output = MSCCL_OUTPUT_BUFFER;
  	} else if (strcmp(str, "s") == 0) {
    	*output = MSCCL_SCRATCH_BUFFER;
  	} else {
    NS_LOG_WARN("type of buffer is not supported: " << str);
    	return AlgoParseResult::INVALID_USE_ERROR;
  	}
  	return AlgoParseResult::ALGO_PARSE_SUCCESS;
	}

	AlgoParseResult mscclCheckBufferBounds(int bufferType, int offset, int nInputChunks, int nOutputChunks, int nScratchChunks){
  	if (bufferType == MSCCL_INPUT_BUFFER){
    	if (offset < -1 || offset >= nInputChunks){
      	NS_LOG_WARN("Incorrect offset set for input buffer: offset: " << offset << ", maximum allowed: " << nInputChunks);
      return AlgoParseResult::INVALID_USE_ERROR;
    }
  	} else if (bufferType == MSCCL_OUTPUT_BUFFER){
    	if (offset < -1 || offset >= nOutputChunks){
      	NS_LOG_WARN("Incorrect offset set for output buffer: offset: " << offset << ", maximum allowed: " << nOutputChunks);
      	return AlgoParseResult::INVALID_USE_ERROR;
   		}
  	} else if (bufferType == MSCCL_SCRATCH_BUFFER){
    	if (offset < -1 || offset >= nScratchChunks){
      	NS_LOG_WARN("Incorrect offset set for scratch buffer: offset: " << offset << ", maximum allowed: " << nScratchChunks);
      	return AlgoParseResult::INVALID_USE_ERROR;
    	}
  	}
  	return AlgoParseResult::ALGO_PARSE_SUCCESS;
	}

	#ifdef HAVE_LIBXML2
	// Post-parse validation of one GPU's network gates (see mscclTransfer::netGate/netWait).
	// Gates are node-local, so this runs once per <gpu> after all of its threadblocks are read.
	// Also records mscclAlgo->maxNetGate, which is what the runtime sizes its gate vectors from.
	static AlgoParseResult ValidateNetGates(struct mscclAlgorithm* algo, int gpuId){
		algo->maxNetGate = MSCCL_GATE_NONE;
		// gate id -> the (bid, sid) that declares it. A gate has exactly one owner; two owners
		// would make "the gate is open" mean whichever message completed first.
		std::map<int16_t, std::pair<int, int>> owner;
		for (int bid = 0; bid < algo->nBlocks; ++bid){
			struct mscclThreadBlock* tb = &algo->mscclTBs[bid];
			for (uint16_t sid = 0; sid < tb->nsteps; ++sid){
				int16_t gate = tb->transfers[sid].netGate;
				if (gate == MSCCL_GATE_NONE) continue;
				auto existing = owner.find(gate);
				if (existing != owner.end()){
					NS_LOG_WARN("MSCCL: gate " << gate << " on GPU (" << gpuId << ") is declared by two ops: tb "
						<< existing->second.first << " step " << existing->second.second << " and tb " << bid
						<< " step " << sid << ". A gate must have exactly one owner.");
					return AlgoParseResult::INVALID_USE_ERROR;
				}
				owner[gate] = {bid, (int)sid};
				if (gate > algo->maxNetGate) algo->maxNetGate = gate;
			}
		}

		// Every gate waited on must be owned by some op, or the waiter hangs forever.
		for (int bid = 0; bid < algo->nBlocks; ++bid){
			struct mscclThreadBlock* tb = &algo->mscclTBs[bid];
			for (uint16_t sid = 0; sid < tb->nsteps; ++sid){
				int16_t gate = tb->transfers[sid].netWait;
				if (gate == MSCCL_GATE_NONE) continue;
				if (owner.find(gate) == owner.end()){
					NS_LOG_WARN("MSCCL: tb " << bid << " step " << sid << " on GPU (" << gpuId << ") waits on gate "
						<< gate << ", which no op declares via netgate. The threadblock would never run.");
					return AlgoParseResult::INVALID_USE_ERROR;
				}
				if (gate > algo->maxNetGate) algo->maxNetGate = gate;
			}
		}

		if (owner.empty()) return AlgoParseResult::ALGO_PARSE_SUCCESS;

		// The runtime allocates its gate vectors densely up to maxNetGate, so a typo'd id would
		// otherwise cost real memory on every node. Nothing needs ids this large: one gate per
		// wave boundary per GPU is single digits in practice.
		if (algo->maxNetGate > MSCCL_MAX_GATE_ID){
			NS_LOG_WARN("MSCCL: gate id " << algo->maxNetGate << " on GPU (" << gpuId << ") exceeds the largest supported gate id (" << MSCCL_MAX_GATE_ID << ").");
			return AlgoParseResult::INVALID_USE_ERROR;
		}

		// Cycle check over intra-tb step order plus wait->owner edges. Rather than a DFS over
		// the edge set, replay the schedule under gate ordering alone: advance every tb as far
		// as its gates allow, opening a gate once its owning step is passed, until no tb moves.
		// A cycle is exactly the case where some tb is still parked once nothing can progress --
		// and the stall point names the tb and gate, which a DFS would not.
		std::set<int16_t> open;
		std::vector<uint16_t> ptr(algo->nBlocks, 0);
		bool progress = true;
		while (progress){
			progress = false;
			for (int bid = 0; bid < algo->nBlocks; ++bid){
				struct mscclThreadBlock* tb = &algo->mscclTBs[bid];
				while (ptr[bid] < tb->nsteps){
					struct mscclTransfer* tran = &tb->transfers[ptr[bid]];
					if (tran->netWait != MSCCL_GATE_NONE && open.find(tran->netWait) == open.end()) break;
					if (tran->netGate != MSCCL_GATE_NONE) open.insert(tran->netGate);
					ptr[bid]++;
					progress = true;
				}
			}
		}
		for (int bid = 0; bid < algo->nBlocks; ++bid){
			struct mscclThreadBlock* tb = &algo->mscclTBs[bid];
			if (ptr[bid] < tb->nsteps){
				NS_LOG_WARN("MSCCL: gate cycle on GPU (" << gpuId << "): tb " << bid << " is stuck at step "
					<< ptr[bid] << " waiting on gate " << tb->transfers[ptr[bid]].netWait
					<< ", which can never open. The schedule would deadlock silently.");
				return AlgoParseResult::INVALID_USE_ERROR;
			}
		}
		return AlgoParseResult::ALGO_PARSE_SUCCESS;
	}
	#endif

	AlgoParseResult AlgoTopology::ParseAlgoXml(const char* file_path){
		// NOTE: entire method makes use of libxml2-dev being installed
		#ifdef HAVE_LIBXML2
		// A re-parse replaces the algorithm wholesale, so stale endpoints from a previous XML
		// must not survive to be joined against the next switch JSON.
		m_flowEndpoints.clear();
		int nRanks = GetNGpuNodes();
		xmlDocPtr doc = xmlReadFile(file_path, NULL, 0);
	  if (!doc) return AlgoParseResult::FILE_READ_ERROR;

  	xmlNodePtr root = xmlDocGetRootElement(doc);
  	if (!root || xmlStrcmp(root->name, BAD_CAST "algo") != 0)
    	return AlgoParseResult::XML_PARSE_ERROR;

	  /* ---- Parse shared algo attributes ---- */
  	const char* name;
		//const char* protocol;
  	XML_GET_PROP_STR(root, "name", name);
		//XML_GET_PROP_STR(root, "proto", protocol);
		int ngpus;
  	XML_GET_PROP_INT(root, "ngpus", ngpus);
  	if (ngpus != nRanks) return AlgoParseResult::XML_PARSE_ERROR;
		int nChannels;
		int nChunksPerLoop;
  	XML_GET_PROP_INT(root, "nchannels", nChannels);
  	XML_GET_PROP_INT(root, "nchunksperloop", nChunksPerLoop);
		// Reset per-parse algorithm summary captured for the topo-driven helper/tester paths.
		m_nChannels = nChannels;
		m_nChunksPerLoop = nChunksPerLoop;
		m_nInputChunks = 0;
		m_nScratchChunks = 0;
		m_activeGpuIds.clear();
		const char*  collectiveType;
		CollectiveType collType;
		XML_GET_PROP_STR(root, "coll", collectiveType);
		//int inputNChunksMultiplier = 1;
  	//int outputNChunksMultiplier = 1;
  	if (strcmp(collectiveType, "allreduce") == 0){
    	collType = CollectiveType::ALLREDUCE;
	  } else if (strcmp(collectiveType, "allgather") == 0){
			collType = CollectiveType::ALLGATHER;
	 //   inputNChunksMultiplier = nRanks;
  	} else if (strcmp(collectiveType, "reduce") == 0){
  		collType = CollectiveType::REDUCE;
	  } else if (strcmp(collectiveType, "broadcast") == 0){
			collType = CollectiveType::BROADCAST;
	  } else if (strcmp(collectiveType, "alltoall") == 0){
			collType = CollectiveType::ALLTOALL;
	  } else if (strcmp(collectiveType, "reduce_scatter") == 0){
 			collType = CollectiveType::REDUCESCATTER;
		//	outputNChunksMultiplier = nRanks;
	  } else if (strcmp(collectiveType, "custom") == 0){
			collType = CollectiveType::CUSTOM;
	  } else {
			NS_LOG_WARN("MSCCL: collective type " << collectiveType << " is not supported.");
    	return AlgoParseResult::INVALID_USE_ERROR;
  	}

  	/* ---- Iterate over <gpu> nodes ---- */
  	for (xmlNodePtr gpu = root->children; gpu; gpu = gpu->next) {
    	if (gpu->type != XML_ELEMENT_NODE) continue;
    	if (xmlStrcmp(gpu->name, BAD_CAST "gpu") != 0) continue;

    	int gpuId, iChunks, oChunks, sChunks;
    	XML_GET_PROP_INT(gpu, "id", gpuId);
    	XML_GET_PROP_INT(gpu, "i_chunks", iChunks);
    	XML_GET_PROP_INT(gpu, "o_chunks", oChunks);
    	XML_GET_PROP_INT(gpu, "s_chunks", sChunks);

			Ptr<GPU> gpuNode = DynamicCast<GPU, Node>(GetGpuNode(gpuId));
			if (!gpuNode) return AlgoParseResult::INVALID_USE_ERROR;
			if (gpuNode->GetId() != static_cast<uint32_t>(gpuId)) NS_FATAL_ERROR("Node Id mismatch; double check node initialization order.");

			struct mscclAlgorithm* mscclAlgo = gpuNode->GetAlgo();
			memset(mscclAlgo, 0, sizeof(*mscclAlgo));
  		mscclAlgo->isValid = false;
    	mscclAlgo->nScratchChunks = sChunks;
			mscclAlgo->ngpus=ngpus;
			mscclAlgo->nChannels= nChannels;
			mscclAlgo->nchunksPerLoop=nChunksPerLoop;
	  	strncpy(mscclAlgo->name, name, MSCCL_MAX_ALGO_NAME);
			mscclAlgo->collectiveType = collType;

			int blockExists[MSCCL_MAX_NUM_THREAD_BLOCKS];
      memset(blockExists, 0, sizeof(int[MSCCL_MAX_NUM_THREAD_BLOCKS]));

    	/* ---- Iterate over <tb> ---- */
    	for (xmlNodePtr tb = gpu->children; tb; tb = tb->next) {
      	if (tb->type != XML_ELEMENT_NODE) continue;
      	if (xmlStrcmp(tb->name, BAD_CAST "tb") != 0) continue;
				int bid, recvpeer, sendpeer, channelId;
        XML_GET_PROP_INT(tb, "id", bid);
        XML_GET_PROP_INT(tb, "recv", recvpeer);
        XML_GET_PROP_INT(tb, "send", sendpeer);
        XML_GET_PROP_INT(tb, "chan", channelId);
        if (bid < 0){
          NS_LOG_WARN("MSCCL: bid must be not negative. bid " << bid);
          return AlgoParseResult::INVALID_USE_ERROR;
        }
				if (bid >= MSCCL_MAX_NUM_THREAD_BLOCKS){
					NS_LOG_WARN("MSCCL: too many thread blocks are requested. Max thread blocks: " << MSCCL_MAX_NUM_THREAD_BLOCKS);
					return AlgoParseResult::INVALID_USE_ERROR;
				}
				// MSCCL_MAX_THREAD_BLOCK_ID is MSCCL_MAX_NUM_THREAD_BLOCKS - 1 (see msccl.h), so this
				// is currently redundant with the bound above; kept as a separate check since the two
				// aren't guaranteed to stay numerically identical if either is retuned independently.
				if (bid > MSCCL_MAX_THREAD_BLOCK_ID){
					NS_LOG_WARN("MSCCL: thread block id (" << bid << ") on GPU (" << gpuId << ") exceeds the largest representable id (" << MSCCL_MAX_THREAD_BLOCK_ID << ").");
					return AlgoParseResult::INVALID_USE_ERROR;
				}
				if (blockExists[bid]){
					NS_LOG_WARN("MSCCL: duplicate thread block id " << bid << "for MSCCL.");
					return AlgoParseResult::INVALID_USE_ERROR;
				}
				blockExists[bid] = 1;

				if (recvpeer == gpuId || sendpeer == gpuId){
					NS_LOG_WARN("MSCCL: peer (" << recvpeer << ", " << sendpeer << ") and gpu id (" << gpuId << ") must be different.");
					return INVALID_USE_ERROR;
				}
				struct mscclThreadBlock* sTB = &mscclAlgo->mscclTBs[bid];
				sTB->nsteps = 0;
				if (recvpeer < -1 || sendpeer < -1){
					NS_LOG_WARN("MSCCL: wrong recvpeer (" << recvpeer << ") or sendpeer (" << sendpeer << ") in threadblock (" << bid << ") on gpu (" << gpuId << ").");
					return INVALID_USE_ERROR;
				}

				if (recvpeer == gpuId || sendpeer == gpuId){
					NS_LOG_WARN("MSCCL: recvpeer (%d) or sendpeer (%d) for threadblock %d cannot be gpu (" << gpuId << ").");
					return INVALID_USE_ERROR;
				}

				if (recvpeer >= ngpus || sendpeer >= ngpus) {
					NS_LOG_WARN("MSCCL: recvpeer (" << recvpeer <<  ") or sendpeer (" << sendpeer << ") must be -1 or between 0 and ngpus (" << ngpus << ").");
					return INVALID_USE_ERROR;
				}

				sTB->recvpeer = recvpeer;
				sTB->sendpeer = sendpeer;
				if (channelId < 0 || channelId >= MAXCHANNELS){
					NS_LOG_WARN("MSCCL: threadblock (" << bid << ") on GPU (" << gpuId << ") has an invalid channel (" << channelId << ").");
					return INVALID_USE_ERROR;
				}
				sTB->channelId = channelId;

				// setting the summary of the msccl aglorithm in msccl channels
				mscclChannelInfo* mscclChannel = &mscclAlgo->mscclChannels[sTB->channelId];

				int numDependences = 0;
				int oldDependencePointer = 0; // inidcator of where the dependences started for nop

				int oldReductionDstBuffer = -1; // Indicator of last reduction buffer name; -1 means that last one wasn't a compatible reduction
				int oldReductionDstOffset = -1; // Indicator of last reduction buffer index
				int oldReductionSrcBuffer = -1; //
				int numReductions = 0;

				int numTransfers = 0;

      		/* ---- Iterate over <step> ---- */
				for (xmlNodePtr stepNode = tb->children; stepNode; stepNode = stepNode->next) {
					if (stepNode->type != XML_ELEMENT_NODE) continue;
					if (xmlStrcmp(stepNode->name, BAD_CAST "step") != 0) continue;


					int s, srcoffset, dstoffset, depend_bid, depend_step, has_dependence, count;
					const char* srcbuffer, * dstbuffer, * type;
					XML_GET_PROP_INT(stepNode, "s", s);

					XML_GET_PROP_INT(stepNode, "srcoff", srcoffset);
					XML_GET_PROP_STR(stepNode, "srcbuf", srcbuffer);
					XML_GET_PROP_INT(stepNode, "dstoff", dstoffset);
					XML_GET_PROP_STR(stepNode, "dstbuf", dstbuffer);

					XML_GET_PROP_INT(stepNode, "cnt", count);
					XML_GET_PROP_STR(stepNode, "type", type);
					XML_GET_PROP_INT(stepNode, "depid", depend_bid);
					XML_GET_PROP_INT(stepNode, "deps", depend_step);
					XML_GET_PROP_INT(stepNode, "hasdep", has_dependence);

					// optional: not every step carries a flow id (e.g. recv steps in
					// the example XML), so this attribute is read leniently instead
					// of via XML_GET_PROP_INT, which fails the whole parse if missing
					uint32_t mscclFlowId = MSCCL_FLOW_ID_NONE;
					{
						xmlChar* flowIdProp = xmlGetProp(stepNode, BAD_CAST "mscclflowid");
						if (flowIdProp) {
							mscclFlowId = (uint32_t) atoi((const char*)flowIdProp);
							xmlFree(flowIdProp);
						}
					}

					// optional host-side pacing rate in GB/s (see mscclTransfer::rate);
					// read leniently like mscclflowid since not every step carries one
					double rate = 0.0;
					{
						xmlChar* rateProp = xmlGetProp(stepNode, BAD_CAST "rate");
						if (rateProp) {
							rate = atof((const char*)rateProp);
							xmlFree(rateProp);
						}
					}

					// optional network gates (see mscclTransfer::netGate/netWait), read leniently
					// like mscclflowid/rate so every existing XML parses unchanged with both -1
					int16_t netGate = MSCCL_GATE_NONE;
					int16_t netWait = MSCCL_GATE_NONE;
					{
						xmlChar* gateProp = xmlGetProp(stepNode, BAD_CAST "netgate");
						if (gateProp) {
							netGate = (int16_t) atoi((const char*)gateProp);
							xmlFree(gateProp);
						}
						xmlChar* waitProp = xmlGetProp(stepNode, BAD_CAST "netwait");
						if (waitProp) {
							netWait = (int16_t) atoi((const char*)waitProp);
							xmlFree(waitProp);
						}
					}
					if (netGate < MSCCL_GATE_NONE || netWait < MSCCL_GATE_NONE){
						NS_LOG_WARN("MSCCL: netgate/netwait must be >= -1, but step " << s << " of threadblock (" << bid << ") on GPU (" << gpuId << ") has netgate=" << netGate << " netwait=" << netWait << ".");
						return AlgoParseResult::INVALID_USE_ERROR;
					}
					// Self-deadlock: the op would be waiting on the gate only it can open.
					if (netGate != MSCCL_GATE_NONE && netGate == netWait){
						NS_LOG_WARN("MSCCL: step " << s << " of threadblock (" << bid << ") on GPU (" << gpuId << ") both opens and waits on gate " << netGate << " -- self-deadlock.");
						return AlgoParseResult::INVALID_USE_ERROR;
					}

					if (s >= MSCCL_MAX_NUM_STEPS){
						NS_LOG_WARN("MSCCL: too many steps are requested. Max number of steps: " << MSCCL_MAX_NUM_STEPS << ", requested: " << s+1  << ". ");
						return AlgoParseResult::INVALID_USE_ERROR;
					}
					if (s < 0){
						NS_LOG_WARN("MSCCL: step must be positive: step " << s);
						return AlgoParseResult::INVALID_USE_ERROR;
					}

					int hasSend = 0;
					int hasRecv = 0;
					int checkSrc = 0;
					int checkDst = 0;
					int transferType = -1; // -1 indicate a nop
					if (strcmp(type, "s") == 0){
						transferType = MSCCL_SEND;
						hasSend = 1;
						checkSrc = 1;
					} else if (strcmp(type, "r") == 0) {
						transferType = MSCCL_RECV;
						hasRecv = 1;
						checkDst = 1;
					} else if (strcmp(type, "rcs") == 0) {
						transferType = MSCCL_RECV_COPY_SEND;
						hasSend = 1;
						hasRecv = 1;
						checkDst = 1;
					} else if (strcmp(type, "rrs") == 0) {
						transferType = MSCCL_RECV_REDUCE_SEND;
						hasSend = 1;
						hasRecv = 1;
						checkSrc = 1;
					} else if (strcmp(type, "rrc") == 0) {
						transferType = MSCCL_RECV_REDUCE_COPY;
						hasRecv = 1;
					} else if (strcmp(type, "rrcs") == 0) {
						transferType = MSCCL_RECV_REDUCE_COPY_SEND;
						hasRecv = 1;
						hasSend = 1;
						checkSrc = 1;
						checkDst = 1;
					} else if (strcmp(type, "cpy") == 0) {
						transferType = MSCCL_LOCAL_COPY;
						checkSrc = 1;
						checkDst = 1;
					} else if (strcmp(type, "re") == 0) {
						transferType = MSCCL_REDUCE;
						checkSrc = 1;
						checkDst = 1;
					} else if (strcmp(type, "ra") == 0) {
						transferType = MSCCL_RES_ADD;
						checkSrc = 1;
						checkDst = 1;
					} else if (strcmp(type, "nop") == 0) {
						transferType = -1;
					} else {
						NS_LOG_WARN("MSCCL: type of transfer is not supported: " << type);
						return AlgoParseResult::INVALID_USE_ERROR;
					}

					if (netGate != MSCCL_GATE_NONE || netWait != MSCCL_GATE_NONE){
						// nop steps never become transfers (see the `transferType != -1` guard below),
						// so a gate attribute on one would be silently discarded pacing.
						if (transferType == -1){
							NS_LOG_WARN("MSCCL: netgate/netwait on a nop step (step " << s << " of threadblock (" << bid << ") on GPU (" << gpuId << ")). Nops carry no transfer, so the gate would be discarded.");
							return AlgoParseResult::INVALID_USE_ERROR;
						}
						// Gates model the network, and only the RDMA path opens them: a gate on a
						// non-sending op could never open (or never be observed), hanging the run.
						if (!hasSend){
							NS_LOG_WARN("MSCCL: netgate/netwait on a non-sending step (type \"" << type << "\", step " << s << " of threadblock (" << bid << ") on GPU (" << gpuId << ")). Gates are valid only on send-bearing ops.");
							return AlgoParseResult::INVALID_USE_ERROR;
						}
					}

					if (depend_bid >= 0) {
						// Same ceiling as the tb id itself -- this is the field it is stored in.
						if (depend_bid > MSCCL_MAX_THREAD_BLOCK_ID){
							NS_LOG_WARN("MSCCL: depid (" << depend_bid << ") at step " << s << " of threadblock (" << bid << ") on GPU (" << gpuId << ") exceeds the largest representable thread block id (" << MSCCL_MAX_THREAD_BLOCK_ID << ").");
							return AlgoParseResult::INVALID_USE_ERROR;
						}
						// numDependences counts every step carrying a depid, including the nops the
						// emitter inserts to express multi-dependency ops, so it is bounded by the step
						// count only when the s values are distinct -- which the parser never checks.
						if (numDependences >= MSCCL_MAX_NUM_STEPS){
							NS_LOG_WARN("MSCCL: too many dependences in threadblock (" << bid << ") on GPU (" << gpuId << "). Max: " << MSCCL_MAX_NUM_STEPS);
							return AlgoParseResult::INVALID_USE_ERROR;
						}
						sTB->dependentBid[numDependences] = depend_bid;
						sTB->dependentStep[numDependences] = depend_step;
						numDependences++;
					}

					uint8_t srcbufferInt = 0;
					uint8_t dstbufferInt = 0;
					AlgoParseResult res;
					res = mscclGetBufferType(srcbuffer, &srcbufferInt);
					if (res != AlgoParseResult::ALGO_PARSE_SUCCESS) return res;
					res = mscclGetBufferType(dstbuffer, &dstbufferInt);
					if (res != AlgoParseResult::ALGO_PARSE_SUCCESS) return res;

					int continuationOfReductions = 0;
					// Analyze to see if this is in the same list of reductions for them to be chained
					if (transferType == MSCCL_REDUCE) {
						if (oldReductionDstBuffer == dstbufferInt && oldReductionDstOffset == dstoffset && oldReductionSrcBuffer == srcbufferInt && depend_bid == -1){
							numTransfers--; // reuse the same transfer
							continuationOfReductions = 1;
						} else {
							oldReductionDstBuffer = -1;
							oldReductionDstOffset = -1;
						}
					}


					if (transferType != -1) {
						struct mscclTransfer* msccltran = &sTB->transfers[numTransfers];
						msccltran->type = transferType;
						msccltran->srcoffset = srcoffset;
						msccltran->srcbuffer = srcbufferInt;
						msccltran->srcoffset = srcoffset;
						msccltran->dstbuffer = dstbufferInt;
						msccltran->dstoffset = dstoffset;
						msccltran->mscclFlowId = mscclFlowId;
						msccltran->rate = rate;
						// mscclAlgo is memset to 0 at parse start, so these must be written on every
						// transfer -- otherwise an absent attribute would read as gate 0.
						msccltran->netGate = netGate;
						msccltran->netWait = netWait;

						if (count < 0 || count >= MSCCL_MAX_COUNT){
							NS_LOG_WARN("MSCCL: count (" << count << ") must be positive and less than " << MSCCL_MAX_COUNT);
								return AlgoParseResult::INVALID_USE_ERROR;
						}
						msccltran->count = count;

						if (hasSend){
							if (sendpeer < 0){
								NS_LOG_WARN("MSCCL: there is a send in threadblock (" << bid << ") on GPU (" << gpuId << ") without a sendpeer.");
								return AlgoParseResult::INVALID_USE_ERROR;
							}
							// Record the flow's endpoints for ParseSwitchJson's benefit. Only the send
							// side is recorded: the peer's matching recv step repeats this same flow
							// id, and taking it from there would name the receiver as the sender.
							if (mscclFlowId != MSCCL_FLOW_ID_NONE){
								m_flowEndpoints[mscclFlowId] = FlowEndpoints{
									(uint32_t) gpuId, (int16_t) sendpeer, channelId};
							}
							if (mscclChannel->nSendPeers >= MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL){
								NS_LOG_WARN("MSCCL: too many sends per channel. Max allowed " << MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL);
								return AlgoParseResult::INVALID_USE_ERROR;
							}

							struct mscclChannelPeerInfo* sendPeerInfo = &mscclChannel->sendPeerInfo[mscclChannel->nSendPeers];
							sendPeerInfo->nchunksForPeer[count-1]++;
							// mscclChannel->nchunksForSendPeer[mscclChannel->nsendPeers][count-1]++;
						}
						if (hasRecv){
							if (recvpeer < 0){
								NS_LOG_WARN("MSCCL: there is a recv in threadblock (" << bid << ") on GPU (" << gpuId <<") without a recvpeer.");
								return AlgoParseResult::INVALID_USE_ERROR;
							}
							if (mscclChannel->nRecvPeers >= MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL){
								NS_LOG_WARN("MSCCL: too many recvs per channel. Max allowed "<< MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL);
								return AlgoParseResult::INVALID_USE_ERROR;
							}
							struct mscclChannelPeerInfo* recvPeerInfo = &mscclChannel->recvPeerInfo[mscclChannel->nRecvPeers];
							recvPeerInfo->nchunksForPeer[count-1]++;
							// mscclChannel->nchunksForRecvPeer[mscclChannel->nrecvPeers][count-1]++;
						}

						if (checkSrc) {
							res = mscclCheckBufferBounds(msccltran->srcbuffer, msccltran->srcoffset, iChunks, oChunks, sChunks);
							if (res != AlgoParseResult::ALGO_PARSE_SUCCESS) return res;
						}
						if (checkDst) {
							res = mscclCheckBufferBounds(msccltran->dstbuffer, msccltran->dstoffset, iChunks, oChunks, sChunks);
							if (res != AlgoParseResult::ALGO_PARSE_SUCCESS) return res;
						}

						if (!continuationOfReductions){
							msccltran->depencePointer = oldDependencePointer;
							msccltran->numDependences = numDependences - oldDependencePointer;
							if (msccltran->numDependences > 0 && depend_bid < 0){
								NS_LOG_WARN("MSCCL: when there is a chain of dependences, the last reduction must be a part of the first immediate instruction. Detected for GPU " << gpuId << ", threadblock " << bid << ", and step " << s << ". XML will be ignored.");
								return AlgoParseResult::INVALID_USE_ERROR;
							}
							oldDependencePointer = numDependences;
						}

						// reduction related pointers
						if (transferType != MSCCL_REDUCE){
							oldReductionDstBuffer = -1;
							oldReductionDstOffset = -1;
							oldReductionSrcBuffer = -1;
						} else {
							if (oldReductionDstBuffer == -1) { // if this is the first reduction
								msccltran->reductionPointer = numReductions;
							}
							sTB->reductionSrcOffsets[numReductions] = msccltran->srcoffset;
							numReductions++;
							msccltran->numReductions = numReductions - msccltran->reductionPointer;

							if (has_dependence || numReductions == MSCCL_MAX_REDUCE_FUSION){
								oldReductionDstBuffer = -1;
								oldReductionDstOffset = -1;
							} else {
								oldReductionDstBuffer = msccltran->dstbuffer;
								oldReductionDstOffset = msccltran->dstoffset;
								oldReductionSrcBuffer = msccltran->srcbuffer;
							}
						}


						if (has_dependence != 0 && has_dependence != 1){
							NS_LOG_WARN("MSCCL: has_dependence needs to be 0 or 1, but it was " << has_dependence);
							return AlgoParseResult::INVALID_USE_ERROR;
						}
						msccltran->has_dependence = has_dependence;

						numTransfers++;
						sTB->nsteps = numTransfers;
					}
				} // stepNode
				// channel info calculation
				for (int c = 0; c < MSCCL_MAX_COUNT; c++){
					struct mscclChannelPeerInfo* sendPeer = &mscclChannel->sendPeerInfo[mscclChannel->nSendPeers];
					if (sendPeer->nchunksForPeer[c] > 0){
						sendPeer->counts[sendPeer->nCountExists] = c;
						sendPeer->nCountExists++;
					}
					struct mscclChannelPeerInfo* recvPeer = &mscclChannel->recvPeerInfo[mscclChannel->nRecvPeers];
					if (recvPeer->nchunksForPeer[c] > 0){
						recvPeer->counts[recvPeer->nCountExists] = c;
						recvPeer->nCountExists++;
					}
				}

				if (sTB->sendpeer >= 0){
					mscclChannel->sendPeerInfo[mscclChannel->nSendPeers].peer = sTB->sendpeer;
					mscclChannel->nSendPeers++;
				}
				if (sTB->recvpeer >= 0){
					mscclChannel->recvPeerInfo[mscclChannel->nRecvPeers].peer = sTB->recvpeer;
					mscclChannel->nRecvPeers++;
				}
			} // threadBlock

			// make sure that threblocks are in order. Something like 0, 2, 3 is not allowed.
			if (blockExists[0] == 1){
				mscclAlgo->nBlocks = 1;
			}
			for (int i = 1; i < MSCCL_MAX_NUM_THREAD_BLOCKS; i++){
				if (blockExists[i] == 1 && blockExists[i-1] == 0){
					NS_LOG_WARN("MSCCL: threadblock " << i << " is missing.");
					return AlgoParseResult::INVALID_USE_ERROR;
				}
				if (blockExists[i] == 1){
					mscclAlgo->nBlocks = i+1;
				}
			}

			{
				AlgoParseResult gateRes = ValidateNetGates(mscclAlgo, gpuId);
				if (gateRes != AlgoParseResult::ALGO_PARSE_SUCCESS) return gateRes;
			}

			mscclAlgo->isValid = true;

			// A GPU that carries at least one threadblock is an active participant. Record it
			// (ascending gpu-id order, enforced below) and its per-GPU input chunk count so the
			// topo-driven helper/tester paths can install and set up without the caller
			// re-specifying either. Symmetric collectives give every active rank the same
			// i_chunks; warn if that assumption is violated rather than silently pick one.
			if (mscclAlgo->nBlocks > 0){
				m_activeGpuIds.push_back(gpuId);
				if (m_nInputChunks != 0 && m_nInputChunks != iChunks){
					NS_LOG_WARN("MSCCL: GPU " << gpuId << " has i_chunks " << iChunks
						<< " differing from earlier active GPUs (" << m_nInputChunks
						<< "); topo-driven tester setup assumes a uniform per-rank input size.");
				}
				m_nInputChunks = iChunks;
				// Scratch, unlike i_chunks, legitimately varies per rank (a rank that relays more
				// traffic stages more chunks), so take the maximum: the tester allocates one
				// uniform scratch buffer and it must cover the hungriest rank's offsets.
				if (sChunks > m_nScratchChunks) m_nScratchChunks = sChunks;
			}
    } // gpu
		std::sort(m_activeGpuIds.begin(), m_activeGpuIds.end());
		xmlFreeDoc(doc);
		return ALGO_PARSE_SUCCESS;
	#else
	NS_FATAL_ERROR("libxml2 support not enabled");
	#endif
 	} // ParseAlgoXml

	// Collects every port on `sw` that reaches `target`, so a switch JSON rule -- which names a
	// next-hop *node*, not a link -- can be validated and installed against all of them. Returns
	// false only in the case actually worth warning about: the algorithm dictates a next hop that
	// this switch has no link to at all. Multiple ports toward one neighbor is not an error but
	// the normal fat-tree case (parallel leaf uplinks to the same spine); all of them are handed
	// back and SwitchNode picks per flow, since any of them realizes the dictated path.
	// Lazily maps out every qbb port of `node` by the neighbor it reaches, on first touch.
	// Called for switches (to validate/resolve a rule's out port) and for GPUs (to find which
	// of a multi-homed sender's NICs faces a given leaf) -- the walk is identical for both.
	std::map<uint32_t, std::vector<uint32_t>>& AlgoTopology::SwitchPortCache(Ptr<Node> node){
		uint32_t swId = node->GetId();
		auto cached = m_switchPortCache.find(swId);
		if (cached != m_switchPortCache.end()) return cached->second;

		std::map<uint32_t, std::vector<uint32_t>>& neighbors = m_switchPortCache[swId];
		for (uint32_t i = 0; i < node->GetNDevices(); ++i){
			Ptr<NetDevice> dev = node->GetDevice(i);
			Ptr<QbbChannel> channel = DynamicCast<QbbChannel>(dev->GetChannel());
			if (!channel) continue;
			Ptr<NetDevice> other = (channel->GetDevice(0) == dev) ? channel->GetDevice(1) : channel->GetDevice(0);
			neighbors[other->GetNode()->GetId()].push_back(dev->GetIfIndex());
		}
		return neighbors;
	}

	bool AlgoTopology::ResolveOutPorts(Ptr<SwitchNode> sw, Ptr<Node> target, std::vector<uint32_t>& outIfIndices){
		std::map<uint32_t, std::vector<uint32_t>>& neighbors = SwitchPortCache(sw);
		auto portIt = neighbors.find(target->GetId());
		if (portIt == neighbors.end() || portIt->second.empty()) return false;
		outIfIndices = portIt->second;
		return true;
	}

	// Reverse of ResolveOutPorts: which neighbor does this switch's port `ifIndex` reach?
	// Doubles as the existence check for a JSON-supplied port -- a false return means the
	// index names no qbb port on this switch, so nothing could be forwarded out of it.
	bool AlgoTopology::ResolvePortNeighbor(Ptr<SwitchNode> sw, uint32_t ifIndex, uint32_t& neighborNodeId){
		for (const auto& [neighbor, ports] : SwitchPortCache(sw)){
			if (std::find(ports.begin(), ports.end(), ifIndex) != ports.end()){
				neighborNodeId = neighbor;
				return true;
			}
		}
		return false;
	}

	// The switch JSON pins each flow to a path but says nothing about which NIC the *sender*
	// should inject on, and on a multi-homed fabric (e.g. dual-plane, one NIC per plane) that
	// choice decides the whole path: inject on the wrong plane and every switch along it misses
	// the flow's rules entirely and falls back to ECMP, so the schedule is silently ignored.
	// The intended NIC is recoverable, though -- it is the sender's port facing this flow's
	// *ingress* switch, i.e. whichever switch in the flow's rule set neighbors the sender. This
	// runs per rule and simply notices when `sw` is that switch.
	void AlgoTopology::ResolveScheduledNics(Ptr<SwitchNode> sw, uint32_t flowId){
		auto ep = m_flowEndpoints.find(flowId);
		if (ep == m_flowEndpoints.end()) return; // rule for a flow the XML never sends

		Ptr<Node> srcGpu = GetGpuNode((int) ep->second.srcGpu);
		if (!srcGpu) return;
		std::map<uint32_t, std::vector<uint32_t>>& gpuPorts = SwitchPortCache(srcGpu);
		auto facing = gpuPorts.find(sw->GetId());
		if (facing == gpuPorts.end() || facing->second.empty()) return; // not the ingress hop

		// Multiple NICs from one GPU to one leaf would make "the" scheduled NIC ambiguous; no
		// topology built so far does that, and taking the first keeps the flow on the dictated
		// path either way (both ports reach the same switch).
		uint32_t nic = facing->second.front();
		ConnectionKey key{ep->second.srcGpu, ep->second.peer, ep->second.chan};
		auto [it, inserted] = m_scheduledNic.emplace(key, nic);
		if (!inserted && it->second != nic && m_scheduledNicConflicts.insert(key).second){
			// RdmaHw binds a qp to one NIC for its lifetime, and MSCCL opens exactly one qp per
			// (peer, channel), so a schedule that spreads one connection's flows across NICs
			// cannot be realized. Report it and leave the connection unpinned (see
			// PublishScheduledNics) rather than honoring whichever flow happened to parse first.
			NS_LOG_WARN("Switch JSON: gpu " << ep->second.srcGpu << " -> peer " << ep->second.peer
				<< " on channel " << ep->second.chan << " has flows entering the fabric on two "
				<< "different NICs (ifIndex " << it->second << " and " << nic << "). One RDMA "
				<< "connection can only use one NIC, so this connection will keep RdmaHw's ECMP "
				<< "hashing instead. Re-solve with the plane held constant per (src, dst, channel).");
		}
	}

	void AlgoTopology::PublishScheduledNics(){
		uint32_t pinned = 0;
		for (const auto& [key, nic] : m_scheduledNic){
			if (m_scheduledNicConflicts.count(key)) continue;
			Ptr<GPU> gpu = DynamicCast<GPU, Node>(GetGpuNode((int) key.srcGpu));
			if (!gpu) continue;
			gpu->PushPeerNic(key.peer, key.chan, nic);
			++pinned;
		}
		// Unconditional (not NS_LOG_INFO): silently pinning nothing -- the failure mode when the
		// XML and JSON disagree about flow ids -- is indistinguishable from a working run except
		// by the performance it quietly fails to deliver.
		NS_LOG_UNCOND("Switch JSON: pinned the scheduled egress NIC on " << pinned
			<< " RDMA connection(s); " << m_scheduledNicConflicts.size()
			<< " left unpinned due to conflicting plane assignments.");
	}

	AlgoParseResult AlgoTopology::ParseSwitchJson(const char* file_path){
		std::ifstream in(file_path);
		if (!in.is_open()) return AlgoParseResult::FILE_READ_ERROR;

		// As in ParseAlgoXml, a re-parse replaces the routing wholesale.
		m_scheduledNic.clear();
		m_scheduledNicConflicts.clear();

		nlohmann::json root;
		try {
			in >> root;

			if (!root.contains("switches") || !root["switches"].is_object()){
				NS_LOG_WARN("Switch JSON: missing top-level \"switches\" object in " << file_path);
				return AlgoParseResult::JSON_PARSE_ERROR;
			}

			for (auto& [switchIdStr, flows] : root["switches"].items()){
				int switchId = std::stoi(switchIdStr);
				Ptr<Node> swNodeBase = GetSwitchNode(switchId);
				if (!swNodeBase) return AlgoParseResult::INVALID_USE_ERROR;
				Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(swNodeBase);
				if (!sw){
					NS_LOG_WARN("Switch JSON: node " << switchId << " is not a SwitchNode");
					return AlgoParseResult::INVALID_USE_ERROR;
				}
				sw->SetAttribute("CustomFlowForwarding", BooleanValue(true));

				for (auto& [flowIdStr, entry] : flows.items()){
					uint32_t flowId = static_cast<uint32_t>(std::stoul(flowIdStr));
					int srcGpu = entry.value("src_gpu", -1);
					int dstGpu = entry.value("dst_gpu", -1);
					int step = entry.value("step", -1);

					// "port" is the authoritative forwarding field: this switch's own outgoing
					// device index(es). When present it alone decides where the flow goes, and
					// next_hop/next_hop_type become documentation -- cross-checked and warned
					// about below, but never allowed to override the port. Giving an array
					// spreads one flow across several parallel links (SwitchNode then picks per
					// flow), which is how a leaf's multiple uplinks to one spine get used.
					// When "port" is absent -- every JSON written so far -- fall back to
					// resolving next_hop to all ports that reach the named neighbor.
					const bool hasPort = entry.contains("port");

					// Both next_hop fields are optional once "port" carries the decision; a
					// missing or unresolvable next_hop then only weakens the cross-check.
					std::string nextHopType = entry.value("next_hop_type", std::string());
					int nextHop = entry.value("next_hop", -1);
					Ptr<Node> target;
					if (nextHopType == "gpu"){
						target = GetGpuNode(nextHop);
					} else if (nextHopType == "switch"){
						target = GetSwitchNode(nextHop);
					} else if (!nextHopType.empty()){
						NS_LOG_WARN("Switch JSON: unknown next_hop_type \"" << nextHopType << "\" for switch " << switchId << ", flow " << flowId);
						if (!hasPort) return AlgoParseResult::INVALID_USE_ERROR;
					} else if (!hasPort){
						NS_LOG_WARN("Switch JSON: switch " << switchId << ", flow " << flowId << " has neither \"port\" nor \"next_hop_type\" -- no way to decide an out port");
						return AlgoParseResult::INVALID_USE_ERROR;
					}
					if (!target && !nextHopType.empty()){
						NS_LOG_WARN("Switch JSON: switch " << switchId << ", flow " << flowId << ": next_hop " << nextHop << " (" << nextHopType << ") is not a known node");
						if (!hasPort) return AlgoParseResult::INVALID_USE_ERROR;
					}

					std::vector<uint32_t> outIfIndices;
					if (hasPort){
						const nlohmann::json& portField = entry.at("port");
						if (portField.is_array()){
							for (const auto& portVal : portField) outIfIndices.push_back(portVal.get<uint32_t>());
						} else {
							outIfIndices.push_back(portField.get<uint32_t>());
						}
						if (outIfIndices.empty()){
							NS_LOG_WARN("Switch JSON: switch " << switchId << ", flow " << flowId << " has an empty \"port\" list");
							return AlgoParseResult::INVALID_USE_ERROR;
						}
						// A port index that names no qbb port on this switch stays fatal even
						// though next_hop is not: there is no link to forward the flow out of,
						// so unlike a stale next_hop this cannot be downgraded to a warning.
						for (uint32_t ifIndex : outIfIndices){
							uint32_t neighborId = 0;
							if (!ResolvePortNeighbor(sw, ifIndex, neighborId)){
								NS_LOG_WARN("Switch JSON: switch " << switchId << ", flow " << flowId << ": port " << ifIndex << " is not a qbb port on this switch (it has " << sw->GetNDevices() << " device(s)). Check that the generator numbers ports by ns-3 device index.");
								return AlgoParseResult::INVALID_USE_ERROR;
							}
							if (target && neighborId != target->GetId()){
								NS_LOG_WARN("Switch JSON: switch " << switchId << ", flow " << flowId << ": port " << ifIndex << " reaches node " << neighborId << ", not the declared next_hop " << nextHop << " (" << nextHopType << ", node " << target->GetId() << "). Forwarding follows the port; next_hop looks stale.");
							}
						}
					} else if (!ResolveOutPorts(sw, target, outIfIndices)){
						NS_LOG_WARN("Switch JSON: switch " << switchId << " has no link toward next_hop " << nextHop << " (" << nextHopType << ") for flow " << flowId);
						return AlgoParseResult::INVALID_USE_ERROR;
					}

					sw->AddFlowForwardingRule(flowId, outIfIndices);
					ResolveScheduledNics(sw, flowId);
					std::ostringstream portList;
					for (size_t p = 0; p < outIfIndices.size(); ++p){
						if (p) portList << ",";
						portList << outIfIndices[p];
					}
					NS_LOG_INFO("Switch " << switchId << ": flow " << flowId << " (gpu " << srcGpu << " -> gpu " << dstGpu << ", step " << step << ") -> port(s) " << portList.str());
				}
			}
		} catch (const std::exception& e) {
			NS_LOG_WARN("Switch JSON: error parsing " << file_path << ": " << e.what());
			return AlgoParseResult::JSON_PARSE_ERROR;
		}

		// Only after every rule has been seen: a conflicting connection may not reveal itself
		// until its second flow, so publishing incrementally could pin one that later conflicts.
		PublishScheduledNics();
		return AlgoParseResult::ALGO_PARSE_SUCCESS;
	}

} // namespace ns3
