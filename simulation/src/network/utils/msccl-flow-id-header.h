/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef MSCCL_FLOW_ID_HEADER_H
#define MSCCL_FLOW_ID_HEADER_H

#include "ns3/header.h"
#include <cstdint>

namespace ns3 {

// Carries an application-level (msccl) flow id as a real wire header, distinct
// from FlowIdTag (which qbb-net-device/switch-node already use for the ingress
// port index in MMU bookkeeping). Added by RdmaHw::GetNxtPacket from the sending
// queue pair's mscclFlowId, parsed by switches into CustomHeader::udp.mscclFlowId
// for the optional custom flow-forwarding table.
//
// Sits innermost on the wire (right next to the payload, after the SeqTs/INT
// header region), so it doesn't disturb the fixed byte offsets switches use to
// reach the INT header for HPCC telemetry.
class MscclFlowIdHeader : public Header
{
public:
  // sentinel meaning "no msccl flow id assigned"
  static constexpr uint32_t NO_FLOW_ID = 0xFFFFFFFFu;

  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual uint32_t GetSerializedSize (void) const;
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual void Print (std::ostream &os) const;

  MscclFlowIdHeader ();
  MscclFlowIdHeader (uint32_t flowId);
  void SetFlowId (uint32_t flowId);
  uint32_t GetFlowId (void) const;
private:
  uint32_t m_flowId;
};

} // namespace ns3

#endif /* MSCCL_FLOW_ID_HEADER_H */
