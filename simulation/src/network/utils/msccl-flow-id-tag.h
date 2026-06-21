/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef MSCCL_FLOW_ID_TAG_H
#define MSCCL_FLOW_ID_TAG_H

#include "ns3/tag.h"

namespace ns3 {

// Carries an application-level (msccl) flow id alongside RDMA packets, distinct
// from FlowIdTag (which qbb-net-device/switch-node already use for the ingress
// port index in MMU bookkeeping). Set by RdmaHw::GetNxtPacket from the sending
// queue pair's tag, read by SwitchNode's optional custom flow-forwarding table.
class MscclFlowIdTag : public Tag
{
public:
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual uint32_t GetSerializedSize (void) const;
  virtual void Serialize (TagBuffer buf) const;
  virtual void Deserialize (TagBuffer buf);
  virtual void Print (std::ostream &os) const;
  MscclFlowIdTag ();
  MscclFlowIdTag (uint32_t flowId);
  void SetFlowId (uint32_t flowId);
  uint32_t GetFlowId (void) const;
private:
  uint32_t m_flowId;
};

} // namespace ns3

#endif /* MSCCL_FLOW_ID_TAG_H */
