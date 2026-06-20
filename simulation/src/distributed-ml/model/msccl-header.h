#ifndef MSCCL_HEADER_H
#define MSCCL_HEADER_H

#include "ns3/network-module.h"
#include "ns3/core-module.h"

#include <utility>

namespace ns3 {
	class MscclHeader : public Header {

		public:
			static TypeId GetTypeId();
		  virtual TypeId GetInstanceTypeId() const override;
			MscclHeader();
			MscclHeader(uint16_t src, uint16_t dst, uint16_t chan, uint16_t dstBuf, uint16_t dstOff, uint32_t bytes, uint32_t flowId=0, uint32_t fragByteOffset=0);
			void Serialize(Buffer::Iterator i) const override;
			uint32_t Deserialize(Buffer::Iterator i) override;
			uint32_t GetSerializedSize() const override;
			void Print(std::ostream &os) const override;
			uint16_t GetSrcGpu();
			uint16_t GetDstGpu();
			uint16_t GetChannel();
			uint16_t GetDstBuf();
			uint16_t GetDstOff();
			uint32_t GetFlowId();
			uint32_t GetBytes();
			uint32_t GetFragByteOffset();

		private:
		  uint16_t m_srcGpu;
			uint16_t m_dstGpu;
			uint16_t m_channel;
			uint16_t m_dstBuf;
			uint16_t m_dstOff;
			uint32_t m_flowId;
			uint32_t m_bytes;
			uint32_t m_fragByteOffset;
	};
} // namespace ns3
#endif
