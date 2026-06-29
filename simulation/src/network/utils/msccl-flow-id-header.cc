/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "msccl-flow-id-header.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (MscclFlowIdHeader);

TypeId
MscclFlowIdHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MscclFlowIdHeader")
    .SetParent<Header> ()
    .SetGroupName ("Network")
    .AddConstructor<MscclFlowIdHeader> ()
  ;
  return tid;
}
TypeId
MscclFlowIdHeader::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}
uint32_t
MscclFlowIdHeader::GetSerializedSize (void) const
{
  return 4;
}
void
MscclFlowIdHeader::Serialize (Buffer::Iterator start) const
{
  start.WriteHtonU32 (m_flowId);
}
uint32_t
MscclFlowIdHeader::Deserialize (Buffer::Iterator start)
{
  m_flowId = start.ReadNtohU32 ();
  return GetSerializedSize ();
}
void
MscclFlowIdHeader::Print (std::ostream &os) const
{
  os << "MscclFlowId=" << m_flowId;
}
MscclFlowIdHeader::MscclFlowIdHeader ()
  : m_flowId (NO_FLOW_ID)
{
}

MscclFlowIdHeader::MscclFlowIdHeader (uint32_t flowId)
  : m_flowId (flowId)
{
}

void
MscclFlowIdHeader::SetFlowId (uint32_t flowId)
{
  m_flowId = flowId;
}
uint32_t
MscclFlowIdHeader::GetFlowId (void) const
{
  return m_flowId;
}

} // namespace ns3
