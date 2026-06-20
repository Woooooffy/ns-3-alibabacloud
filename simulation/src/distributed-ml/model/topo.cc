#include "topo.h"
#ifdef HAVE_LIBXML2
#include <libxml/parser.h>
#include <libxml/tree.h>
#endif

namespace ns3
{
	NS_LOG_COMPONENT_DEFINE("DistributedTopo");
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

	TopoNodeSet::TopoNodeSet(){}

	TopoNodeSet::TopoNodeSet(NodeContainer& nodes): m_nNodes(nodes.GetN()), m_nodes(nodes){}

	TopoNodeSet::~TopoNodeSet(){}

	int TopoNodeSet::GetNNodes(){
		return m_nNodes;
	}

	Ptr<Node> TopoNodeSet::GetNode(int i){
		// ns3 native .Geti() does not perform bound check
		if (i < 0 || (uint32_t) i > m_nodes.GetN()){
			NS_LOG_ERROR("Accessing out-of-range index " << i << " on TopoNodeSet with " << m_nodes.GetN() << "nodes.");
			return nullptr;
		}	
		return m_nodes.Get(i);
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

	AlgoParseResult ParseAlgoFromXml(const char* file_path, TopoNodeSet nodes){
		// NOTE: entire method makes use of libxml2-dev being installed
		#ifdef HAVE_LIBXML2
		int nRanks = nodes.GetNNodes();
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

			Ptr<GPU> gpuNode = DynamicCast<GPU, Node>(nodes.GetNode(gpuId));
			if (gpuNode->GetId() != static_cast<uint32_t>(gpuId)) NS_FATAL_ERROR("Node Id mismatch; double check node initialization order.");
			if (!gpuNode) return AlgoParseResult::INVALID_USE_ERROR;			
	
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
				if (channelId < 0 || channelId > MAXCHANNELS){
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

					if (depend_bid >= 0) {
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

			mscclAlgo->isValid = true;	
    } // gpu
		xmlFreeDoc(doc);
		return ALGO_PARSE_SUCCESS;	
	#else
	NS_FATAL_ERROR("libxml2 support not enabled");
	#endif
 	} // ParsAlgoFromXML

} // namespace ns3

