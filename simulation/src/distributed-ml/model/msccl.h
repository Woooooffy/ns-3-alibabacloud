// File copied from msccl/src/include/msccl.h
#ifndef MSCCL_H_
#define MSCCL_H_

#include <stdint.h>
#include <ostream>
#include <cstring>
namespace ns3 {
// TODO: double check predefined params for topology specificity
#define MSCCL_MAX_NUM_STEPS 256
#define MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL 32
#define MSCCL_MAX_NUM_THREAD_BLOCKS (108*2) // set this to 108 which is the number of SMs on A100
#define MSCCL_MAX_NUM_ALGOS 4
#define MSCCL_MAX_COUNT 72 // make sure this does not overflow wrt the size of mscclWorkElem::mscclMaxAllowCount
#define MSCCL_MAX_REDUCE_FUSION 16

#define MSCCL_SLICESTEPS (NCCL_STEPS/4)
#define MSCCL_CHUNKSTEPS (NCCL_STEPS/2)

#define MAXCHANNELS 32

static_assert(MAXCHANNELS*MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL >= MSCCL_MAX_NUM_THREAD_BLOCKS);
static_assert(MSCCL_MAX_NUM_STEPS <= 256, "MSCCL interpreter doesn't allow for more than nthreads dependences");

#define MSCCL_INPUT_BUFFER 0
#define MSCCL_OUTPUT_BUFFER 1
#define MSCCL_SCRATCH_BUFFER 2

#define MSCCL_SEND 0
#define MSCCL_RECV 1
#define MSCCL_RECV_COPY_SEND 2
#define MSCCL_RECV_REDUCE_SEND 3
#define MSCCL_RECV_REDUCE_COPY 4
#define MSCCL_RECV_REDUCE_COPY_SEND 5
#define MSCCL_LOCAL_COPY 6
#define MSCCL_REDUCE 7
#define MSCCL_RES_ADD 8

enum CollectiveType {
	ALLREDUCE,
	ALLGATHER,
	REDUCE,
	BROADCAST,
	ALLTOALL,
	REDUCESCATTER,
	CUSTOM
};

static_assert(UINT8_MAX > MSCCL_MAX_COUNT, "mscclMaxAllowedCount datatype must be smaller than the allowed datatype");
struct mscclWorkElem {
  uint8_t mscclMaxAllowedCount; // this is used in mscclAlgorithm to find the maximum number of counts that can be sent at the same time.
  int8_t mscclAlgoIndex; // identifies which msccl algorithm to use
  uint32_t workIndex;
};

struct mscclWorkInfo {
  int8_t mscclAlgoIndex; // identifies which msccl algorithm to use
};

// TODO: compress this by a lot!
struct mscclTransfer {
  int16_t srcoffset;
  int16_t dstoffset;
  uint8_t srcbuffer; // follow MSCCL_THIS_INPUT/MSCCL_THIS_OUTPUT macros
  uint8_t dstbuffer; // follow MSCCL_THIS_INPUT/MSCCL_THIS_OUTPUT macros
  int16_t depencePointer; // index to the first dependence
  int16_t numDependences; // depencePointer+numDependences indicate the last dependence
  int8_t has_dependence;
  int16_t numReductions; // number of reductions with the same dst
  int16_t reductionPointer; // where the reduction starts
  uint8_t type;
  uint8_t count;
};

// print method
inline std::ostream& operator<<(std::ostream& os, const mscclTransfer& t) {
  os << "mscclTransfer { "
     << "srcoffset=" << t.srcoffset
     << ", dstoffset=" << t.dstoffset
     << ", srcbuffer=" << static_cast<int>(t.srcbuffer)
     << ", dstbuffer=" << static_cast<int>(t.dstbuffer)
     << ", has_dependence=" << static_cast<int>(t.has_dependence);

  if (t.has_dependence) {
    os << ", dependences=["
       << t.depencePointer << ".."
       << (t.depencePointer + t.numDependences - 1)
       << "]";
  }

  os << ", numReductions=" << t.numReductions;
  if (t.numReductions > 0) {
    os << ", reductionRange=["
       << t.reductionPointer << ".."
       << (t.reductionPointer + t.numReductions - 1)
       << "]";
  }

  os << ", type=" << static_cast<int>(t.type)
     << ", count=" << static_cast<int>(t.count)
     << " }";

  return os;
}

struct mscclThreadBlock {
  int16_t sendpeer;
  int16_t recvpeer;
  uint16_t nsteps;
  int8_t channelId; // associated channel. -1 indicates a threadblock with only local copies
  // step is used to index into this array. transfers[step] is the addr to transfer.
  int8_t dependentBid[MSCCL_MAX_NUM_STEPS]; // -1 if not dependent on any threadblock
  int16_t dependentStep[MSCCL_MAX_NUM_STEPS];
  int16_t reductionSrcOffsets[MSCCL_MAX_NUM_STEPS]; // in case there are multiple reductions with the same dstwewqwqew
  struct mscclTransfer transfers[MSCCL_MAX_NUM_STEPS];
  int64_t pad;
};

// print method
inline std::ostream& operator<<(std::ostream& os, const mscclThreadBlock& tb) {
  os << "mscclThreadBlock { "
     << "sendpeer=" << tb.sendpeer
     << ", recvpeer=" << tb.recvpeer
     << ", nsteps=" << tb.nsteps
     << ", channelId=" << static_cast<int>(tb.channelId)
     << " }\n";

  for (uint16_t step = 0; step < tb.nsteps; ++step) {
    os << "  [" << step << "] "
      << ", redSrcOff=" << tb.reductionSrcOffsets[step]
       << "\n";
    os << "    transfer: " << tb.transfers[step] << "\n";
  }
  return os;
}

struct mscclChannelPeerInfo {
  // nchunksForPeer[i][j] represents the number of times chunks are sent in counts of j-1 for threadblock i. we do not keep counts of 0.
  int peer;
  int nchunksForPeer[MSCCL_MAX_COUNT];
  int nCountExists;
  int counts[MSCCL_MAX_COUNT];
};

// print
inline std::ostream& operator<<(std::ostream& os, const mscclChannelPeerInfo& info) {
  os << "mscclChannelPeerInfo {\n";
  os << "  peer = " << info.peer << "\n";
  os << "  nCountExists = " << info.nCountExists << "\n";

  for (int i = 0; i < info.nCountExists; ++i) {
    int count = info.counts[i];  // j-1 semantics
    os << "  count = " << count << " (chunk size = " << (count + 1) << "): ";

    os << "[";
    bool first = true;
    for (int tb = 0; tb < MSCCL_MAX_COUNT; ++tb) {
      int n = info.nchunksForPeer[tb];
      if (n > 0) {
        if (!first) os << ", ";
        os << "tb" << tb << "=" << n;
        first = false;
      }
    }
    os << "]\n";
  }

  os << "}";

  return os;
}

struct mscclChannelInfo {
  struct mscclChannelPeerInfo sendPeerInfo[MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL];
  int nSendPeers;
  struct mscclChannelPeerInfo recvPeerInfo[MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL];
  int nRecvPeers;
};

// print
inline std::ostream& operator<<(std::ostream& os, const mscclChannelInfo& info) {
  os << "mscclChannelInfo {\n";

  os << "  SendPeers (" << info.nSendPeers << "):\n";
  for (int i = 0; i < info.nSendPeers; ++i) {
    os << "    [" << i << "] ";
    os << info.sendPeerInfo[i] << "\n";
  }

  os << "  RecvPeers (" << info.nRecvPeers << "):\n";
  for (int i = 0; i < info.nRecvPeers; ++i) {
    os << "    [" << i << "] ";
    os << info.recvPeerInfo[i] << "\n";
  }

  os << "}";

  return os;
}

struct mscclFlag {
  uint64_t flag;
  uint64_t align[3]; // To avoid false sharing
};

// gpuId is the one that is in comm->rank
struct mscclAlgorithm {
#define MSCCL_MAX_ALGO_NAME 63
  // name of the algorithm in the XML
  char name[MSCCL_MAX_ALGO_NAME+1];
  // a flag to specify if the MSCCL algorithm is a valid one
  bool isValid;
  // the type of collective this algorithm is
  CollectiveType collectiveType;
  // inPlace collective
  int inPlace;
  // number of gpus in the group
  int ngpus;
  // max(#chunks in input, #chunks in output)
  int nchunksPerLoop;
  // the protocol that the algorithm needs to use
  int protocol;
  // the range of size in which this algorithm is performant
  int64_t minBytes; int64_t maxBytes;
  // bid is used as an index into this array
  struct mscclThreadBlock mscclTBs[MAXCHANNELS*MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL];
  // number of channels needed by MSCCL algorithm
  int nChannels;
  // number of necessary threadblock
  int nBlocks;
  // number of threads in a threadblock
  int nThreads;
  // the arrays in this struct can be inferred from mscclTB. they are created to use NCCL API easily
  struct mscclChannelInfo mscclChannels[MAXCHANNELS];
  // number of scratch chunks that MSCCL will use
  int nScratchChunks;
  int8_t needsProxy;
};

// print

inline std::ostream& operator<<(std::ostream& os, const mscclAlgorithm& algo) {
  os << "mscclAlgorithm {\n";

  os << "  name           = \"" << algo.name << "\"\n";
  os << "  isValid        = " << algo.isValid << "\n";
  os << "  collectiveType = " << static_cast<int>(algo.collectiveType) << "\n";
  os << "  inPlace        = " << algo.inPlace << "\n";
  os << "  ngpus          = " << algo.ngpus << "\n";
  os << "  nchunksPerLoop = " << algo.nchunksPerLoop << "\n";
  os << "  protocol       = " << algo.protocol << "\n";
  os << "  minBytes       = " << algo.minBytes << "\n";
  os << "  maxBytes       = " << algo.maxBytes << "\n";
  os << "  nThreads       = " << algo.nThreads << "\n";
  os << "  nScratchChunks = " << algo.nScratchChunks << "\n";
  os << "  needsProxy     = " << static_cast<int>(algo.needsProxy) << "\n";

  os << "\n  ThreadBlocks (" << algo.nBlocks << "):\n";
  for (int bid = 0; bid < algo.nBlocks; ++bid) {
    os << "    [bid " << bid << "]\n";
    os << algo.mscclTBs[bid] << "\n";
  }

  os << "\n  Channels (" << algo.nChannels << "):\n";
  for (int ch = 0; ch < algo.nChannels; ++ch) {
    os << "    [channel " << ch << "]\n";
    os << algo.mscclChannels[ch] << "\n";
  }

  os << "}";

  return os;
}


// Only related MSCCL algorithm elements necessary for a threadblock
struct mscclSharedMemoryInfo {
  struct mscclThreadBlock mscclTB;
  volatile struct mscclFlag* flags;
  void* scratchBuffer;
  int nchunksPerLoop;
  int8_t needsFence;
  uint32_t workIndex;
};

// All MSCCL algorithm info that will be in ncclDevComm
struct mscclDevCommInfo {
  struct mscclAlgorithm mscclAlgos[MSCCL_MAX_NUM_ALGOS];
  int8_t needsFence; // a global flag to indicate whether we need to have a fence for any of the MSCCL algos
  // allocate enough MSCCL flags (MSCCL_MAX_NUM_THREAD_BLOCKS_PER_CHANNEL * MAXCHANNELS) to synchronize across thread blocks
  struct mscclFlag* flags;
  // declaration for scratchBuffer. This is only to be accessed by the host
  void* scratchBuffer;
};

struct mscclRegistration {
  int algoIndex;
  int64_t minBytes;
  int64_t maxBytes;
  int protocol;
};

// All MSCCL algorithm info that will be in ncclComm
struct mscclHostCommInfo {
  int numberOfMSCCLAlgorithms;
  mscclDevCommInfo mscclDevComm;
  size_t scratchBufferSize;

  // registered algorithms
  struct mscclRegistration *mscclRegistrations;
  int nMscclRegistrations;

  int inMSCCLConnectionSetupPhase;
  uint32_t workIndex;
  int flagsNeedReset;
};
}
#endif
