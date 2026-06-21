/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "msccl-flow-id-tag.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (MscclFlowIdTag);

TypeId
MscclFlowIdTag::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MscclFlowIdTag")
    .SetParent<Tag> ()
    .SetGroupName ("Network")
    .AddConstructor<MscclFlowIdTag> ()
  ;
  return tid;
}
TypeId
MscclFlowIdTag::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}
uint32_t
MscclFlowIdTag::GetSerializedSize (void) const
{
  return 4;
}
void
MscclFlowIdTag::Serialize (TagBuffer buf) const
{
  buf.WriteU32 (m_flowId);
}
void
MscclFlowIdTag::Deserialize (TagBuffer buf)
{
  m_flowId = buf.ReadU32 ();
}
void
MscclFlowIdTag::Print (std::ostream &os) const
{
  os << "MscclFlowId=" << m_flowId;
}
MscclFlowIdTag::MscclFlowIdTag ()
  : Tag (),
    m_flowId (0)
{
}

MscclFlowIdTag::MscclFlowIdTag (uint32_t id)
  : Tag (),
    m_flowId (id)
{
}

void
MscclFlowIdTag::SetFlowId (uint32_t id)
{
  m_flowId = id;
}
uint32_t
MscclFlowIdTag::GetFlowId (void) const
{
  return m_flowId;
}

} // namespace ns3
