/*
 * Copyright (c) 2025 TU Dresden
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Mingyu Ma <mingyu.ma@tu-dresden.de>
 */

#include "ns3/switch-net-device.h"

#include "ns3/boolean.h"
#include "ns3/ethernet-header.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/switched-ethernet-channel.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("P4SwitchNetDevice");
NS_OBJECT_ENSURE_REGISTERED(P4SwitchNetDevice);

// ---------------------------------------------------------------------------
// TypeId
// ---------------------------------------------------------------------------

TypeId
P4SwitchNetDevice::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::P4SwitchNetDevice")
            .SetParent<NetDevice>()
            .SetGroupName("P4sim")
            .AddConstructor<P4SwitchNetDevice>()

            .AddAttribute("EnableTracing",
                          "Enable per-packet tracing inside the P4 pipeline.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&P4SwitchNetDevice::m_enableTracing),
                          MakeBooleanChecker())

            .AddAttribute("EnableSwap",
                          "Enable live P4-program hot-swap.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&P4SwitchNetDevice::m_enableSwap),
                          MakeBooleanChecker())
						.AddAttribute("EnableCustomImpl",
												 	"Enable custom switch impl.",
													BooleanValue(false),
													MakeBooleanAccessor(&P4SwitchNetDevice::m_hasImpl),
													MakeBooleanChecker())

            .AddAttribute("InputBufferSizeLow",
                          "Normal-priority input buffer depth (packets).",
                          UintegerValue(128),
                          MakeUintegerAccessor(&P4SwitchNetDevice::m_InputBufferSizeLow),
                          MakeUintegerChecker<size_t>())

            .AddAttribute("InputBufferSizeHigh",
                          "High-priority input buffer depth (packets).",
                          UintegerValue(128),
                          MakeUintegerAccessor(&P4SwitchNetDevice::m_InputBufferSizeHigh),
                          MakeUintegerChecker<size_t>())

            .AddAttribute("QueueBufferSize",
                          "Output queue buffer depth (packets).",
                          UintegerValue(128),
                          MakeUintegerAccessor(&P4SwitchNetDevice::m_queueBufferSize),
                          MakeUintegerChecker<size_t>())

            .AddAttribute("SwitchRate",
                          "Packet processing rate inside the switch (pps).",
                          UintegerValue(1000),
                          MakeUintegerAccessor(&P4SwitchNetDevice::m_switchRate),
                          MakeUintegerChecker<uint64_t>())

            .AddAttribute(
                "Mtu",
                "Maximum Transmission Unit.",
                UintegerValue(1500),
                MakeUintegerAccessor(&P4SwitchNetDevice::SetMtu, &P4SwitchNetDevice::GetMtu),
                MakeUintegerChecker<uint16_t>())

						.AddAttribute("QueueTypeId",
              "TypeId of the queue type in use",
              StringValue("ns3::DropTailQueue<Packet>"),
              MakeStringAccessor(&P4SwitchNetDevice::m_queueTypeId),
              MakeStringChecker())

            .AddAttribute("QueueMaxSize",
                          "Maximum number of packets held in each per-port queue.",
                          UintegerValue(1000),
                          MakeUintegerAccessor(&P4SwitchNetDevice::m_queueMaxSize),
                          MakeUintegerChecker<uint32_t>(1))

            .AddTraceSource("SwitchEvent",
                            "Fired when the P4 pipeline emits a switch event.",
                            MakeTraceSourceAccessor(&P4SwitchNetDevice::m_switchEvent),
                            "ns3::TracedCallback::Uint32String")

            .AddTraceSource(
                "MacTx",
                "Trace source indicating a packet has arrived for transmission by this device",
                MakeTraceSourceAccessor(&P4SwitchNetDevice::m_macTxTrace),
                "ns3::Packet::TracedCallback")

            .AddTraceSource("MacTxDrop",
                            "Trace source indicating a packet has been dropped by the device "
                            "before transmission",
                            MakeTraceSourceAccessor(&P4SwitchNetDevice::m_macTxDropTrace),
                            "ns3::Packet::TracedCallback")

            .AddTraceSource("MacRx",
                            "A packet has been received by this device, has been passed up from "
                            "the physical layer "
                            "and is being forwarded up the local protocol stack.  This is a "
                            "non-promiscuous trace,",
                            MakeTraceSourceAccessor(&P4SwitchNetDevice::m_macRxTrace),
                            "ns3::Packet::TracedCallback")
            //
            // Trace sources designed to simulate a packet sniffer facility (tcpdump).
            //
            .AddTraceSource("Sniffer",
                            "Trace source simulating a non-promiscuous "
                            "packet sniffer attached to the device",
                            MakeTraceSourceAccessor(&P4SwitchNetDevice::m_snifferTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("PromiscSniffer",
                            "Trace source simulating a promiscuous "
                            "packet sniffer attached to the device",
                            MakeTraceSourceAccessor(&P4SwitchNetDevice::m_promiscSnifferTrace),
                            "ns3::Packet::TracedCallback");

    return tid;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

P4SwitchNetDevice::P4SwitchNetDevice()
    : m_enableTracing(false),
      m_enableSwap(false),
			m_hasImpl(false),
      m_InputBufferSizeLow(1024),
      m_InputBufferSizeHigh(1024),
      m_queueBufferSize(1024),
      m_switchRate(1000),
      m_node(nullptr),
      m_ifIndex(0),
      m_mtu(1500),
      m_queueMaxSize(50000)
{
    NS_LOG_FUNCTION_NOARGS();
}

P4SwitchNetDevice::~P4SwitchNetDevice()
{
    NS_LOG_FUNCTION_NOARGS();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void
P4SwitchNetDevice::DoInitialize()
{
    NS_LOG_FUNCTION(this);
    NetDevice::DoInitialize();
}

void
P4SwitchNetDevice::DoDispose()
{
    NS_LOG_FUNCTION_NOARGS();

    m_portChannels.clear();
    m_portDeviceIds.clear();
    m_node = nullptr;

    NetDevice::DoDispose();
}

// ---------------------------------------------------------------------------
// Channel attachment
// ---------------------------------------------------------------------------

void
P4SwitchNetDevice::Attach(Ptr<SwitchedEthernetChannel> channel)
{
    NS_LOG_FUNCTION(this << channel);
    NS_ASSERT_MSG(channel, "Attach: null channel");

    int32_t devId = channel->Attach(this);
    NS_ASSERT_MSG(devId >= 0, "Attach: channel rejected device (already full?)");

    m_portChannels.push_back(channel);
    m_portDeviceIds.push_back(static_cast<uint32_t>(devId));


		if (m_hasImpl){
			m_impl->AddPort();
		}

    NS_LOG_INFO("Attached to channel as port " << (m_portChannels.size() - 1) << " (channel slot "
                                               << devId << ")");
}

// ---------------------------------------------------------------------------
// Ingress: called directly by SwitchedEthernetChannel
// ---------------------------------------------------------------------------

void
P4SwitchNetDevice::Receive(Ptr<Packet> packet, Ptr<NetDevice> sender)
{
    NS_LOG_FUNCTION_NOARGS();
    NS_LOG_DEBUG("UID=" << packet->GetUid());

    // The packet arrives as a full Ethernet frame.  Peek at the header to
    // recover src/dst/protocol for callbacks and P4 metadata.
    EthernetHeader eth;
    packet->PeekHeader(eth);

    Mac48Address src48 = eth.GetSource();
    Mac48Address dst48 = eth.GetDestination();
    uint16_t proto = eth.GetLengthType();

    // Promiscuous sniffer (e.g. pcap): fire for every frame.
    if (!m_promiscRxCallback.IsNull())
    {
        m_promiscRxCallback(this, packet, proto, src48, dst48, PACKET_OTHERHOST);
    }

		PacketType packetType;

    if (dst48.IsBroadcast())
    {
        packetType = PACKET_BROADCAST;
    }
    else if (dst48.IsGroup())
    {
        packetType = PACKET_MULTICAST;
    }
    else if (dst48 == m_address)
    {
        packetType = PACKET_HOST;
    }
    else
    {
        packetType = PACKET_OTHERHOST;
    }

		// pass up stack if no custom switch impl
    if (!m_hasImpl)
    {
        m_promiscSnifferTrace(packet);
        m_snifferTrace(packet);
        m_macRxTrace(packet);

        // Strip the Ethernet header before handing to the IP stack.
        Ptr<Packet> stripped = packet->Copy();
        EthernetHeader hdr;
        stripped->RemoveHeader(hdr);

        if (!m_rxCallback.IsNull())
        {
            m_rxCallback(this, stripped, proto, src48);
        }
        return;
    }

    // Switch mode: find the ingress port from the sender device.
    uint32_t inPort = GetPortNumber(sender);
    if (inPort == UINT32_MAX)
    {
        NS_LOG_WARN("Receive: sender not found in port list: dropping");
        return;
    }

    NS_LOG_DEBUG("Ingress port=" << inPort << " src=" << src48 << " dst=" << dst48 << " proto=0x"
                                 << std::hex << proto << std::dec);

    m_promiscSnifferTrace(packet);
    m_snifferTrace(packet);
    m_macRxTrace(packet);

    // Feed the full Ethernet frame into the P4 pipeline.
    Ptr<Packet> pkt = packet->Copy();
		// TODO forwarding logic
		m_impl->ReceivePacket(pkt, inPort, proto, dst48, packetType);
}

// ---------------------------------------------------------------------------
// Egress: called by P4 pipeline cores
// ---------------------------------------------------------------------------

void
P4SwitchNetDevice::SendPacket(Ptr<Packet> packetOut,
                              int outPort,
                              uint16_t protocol,
                              const Address& destination)
{
    SendNs3Packet(packetOut, outPort/*, protocol, destination*/);
}

void
P4SwitchNetDevice::SendNs3Packet(Ptr<Packet> packetOut,
                                 int outPort/*,
                                 uint16_t protocol,
                                 const Address& destination*/)
{
    NS_LOG_DEBUG("SendNs3Packet: port=" << outPort);

    if (!packetOut)
    {
        NS_LOG_DEBUG("Null packet: dropping");
        return;
    }

    // Port 511 = P4 convention for drop.
    if (outPort == 511)
    {
        NS_LOG_DEBUG("Drop port (511): packet discarded");
        m_macTxDropTrace(packetOut);
        return;
    }

    if (outPort < 0 || static_cast<size_t>(outPort) >= m_portChannels.size())
    {
        NS_LOG_WARN("SendNs3Packet: invalid port " << outPort << " (" << m_portChannels.size()
                                                   << " ports available)");
        m_macTxDropTrace(packetOut);
        return;
    }

    m_macTxTrace(packetOut);
    m_snifferTrace(packetOut);
    m_promiscSnifferTrace(packetOut);

    // The P4 pipeline delivers a full Ethernet frame.
    // Transmit it directly onto the channel.
    Ptr<SwitchedEthernetChannel> ch = m_portChannels[static_cast<size_t>(outPort)];
    uint32_t devId = m_portDeviceIds[static_cast<size_t>(outPort)];
    TransmitOn(ch, devId, packetOut, static_cast<uint32_t>(outPort));
}

void
P4SwitchNetDevice::TransmitOn(Ptr<SwitchedEthernetChannel> channel,
                              uint32_t devId,
                              Ptr<Packet> packet, uint32_t outPort)
{
    if (!channel->TransmitStart(packet, devId))
    {
        NS_LOG_WARN("TransmitOn: channel busy or device inactive: packet dropped");
        m_macTxDropTrace(packet);
        return;
    }

    // Compute serialisation delay from the channel's data rate.
    DataRate bps = channel->GetDataRate();
    Time txTime = bps.CalculateBytesTxTime(packet->GetSize());

    Simulator::Schedule(txTime, &SwitchedEthernetChannel::TransmitEnd, channel, devId);

		// also trigger the port's EgressComplete
		if (m_hasImpl && outPort != UINT32_MAX){
			Simulator::Schedule(txTime, &CustomSwitchImpl::PortEgressComplete, m_impl, outPort);
		}
}

// ---------------------------------------------------------------------------
// Port accessors
// ---------------------------------------------------------------------------

uint32_t
P4SwitchNetDevice::GetNPorts() const
{
    return static_cast<uint32_t>(m_portChannels.size());
}

Ptr<SwitchedEthernetChannel>
P4SwitchNetDevice::GetPortChannel(uint32_t n) const
{
    if (n >= m_portChannels.size())
    {
        return nullptr;
    }
    return m_portChannels[n];
}

uint32_t
P4SwitchNetDevice::GetPortNumber(Ptr<NetDevice> sender) const
{
    // Find the port whose far end is `sender`.
    for (uint32_t i = 0; i < m_portChannels.size(); ++i)
    {
        uint32_t mySlot = m_portDeviceIds[i];
        uint32_t otherSlot = (mySlot == 0) ? 1 : 0;

        if (m_portChannels[i]->GetNDevices() > otherSlot &&
            m_portChannels[i]->GetDevice(otherSlot) == sender)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

// ---------------------------------------------------------------------------
// P4 core accessors / trace helpers
// ---------------------------------------------------------------------------

void
P4SwitchNetDevice::EmitSwitchEvent(uint32_t id, const std::string& msg)
{
    m_switchEvent(id, msg);
}

void
P4SwitchNetDevice::ConnectCoreEvent()
{
    // Intentionally empty: override or call after DoInitialize() to wire
    // pipeline-internal traces to m_switchEvent.
}

bool P4SwitchNetDevice::CustomImplEnabled(){
	return m_hasImpl;
}

void P4SwitchNetDevice::SetCustomImpl(Ptr<CustomSwitchImpl> impl){
	m_impl = impl;
}

Ptr<CustomSwitchImpl> P4SwitchNetDevice::GetCustomImpl(){
	return m_impl;
}

std::string P4SwitchNetDevice::GetQueueTypeId(){
	return m_queueTypeId;
}

uint32_t P4SwitchNetDevice::GetQueueMaxSize(){
	return m_queueMaxSize;
}


// ---------------------------------------------------------------------------
// NetDevice interface
// ---------------------------------------------------------------------------

void
P4SwitchNetDevice::SetIfIndex(const uint32_t index)
{
    m_ifIndex = index;
}

uint32_t
P4SwitchNetDevice::GetIfIndex() const
{
    return m_ifIndex;
}

Ptr<Channel>
P4SwitchNetDevice::GetChannel() const
{
    // Return the first attached channel as the representative channel.
    if (!m_portChannels.empty())
    {
        return m_portChannels[0];
    }
    return nullptr;
}

void
P4SwitchNetDevice::SetAddress(Address address)
{
    m_address = Mac48Address::ConvertFrom(address);
}

Address
P4SwitchNetDevice::GetAddress() const
{
    return m_address;
}

bool
P4SwitchNetDevice::SetMtu(const uint16_t mtu)
{
    m_mtu = mtu;
    return true;
}

uint16_t
P4SwitchNetDevice::GetMtu() const
{
    return m_mtu;
}

bool
P4SwitchNetDevice::IsLinkUp() const
{
    return !m_portChannels.empty();
}

void
P4SwitchNetDevice::AddLinkChangeCallback(Callback<void> /*callback*/)
{
    // Not supported for a multi-port device.
}

bool
P4SwitchNetDevice::IsBroadcast() const
{
    return true;
}

Address
P4SwitchNetDevice::GetBroadcast() const
{
    return Mac48Address::GetBroadcast();
}

bool
P4SwitchNetDevice::IsMulticast() const
{
    return true;
}

Address
P4SwitchNetDevice::GetMulticast(Ipv4Address multicastGroup) const
{
    return Mac48Address::GetMulticast(multicastGroup);
}

Address
P4SwitchNetDevice::GetMulticast(Ipv6Address addr) const
{
    return Mac48Address::GetMulticast(addr);
}

bool
P4SwitchNetDevice::IsPointToPoint() const
{
    return false;
}

bool
P4SwitchNetDevice::IsBridge() const
{
    return false;
}

bool
P4SwitchNetDevice::Send(Ptr<Packet> packet, const Address& dest, uint16_t protocolNumber)
{
    return SendFrom(packet, m_address, dest, protocolNumber);
}

bool
P4SwitchNetDevice::SendFrom(Ptr<Packet> packet,
                            const Address& src,
                            const Address& dest,
                            uint16_t protocolNumber)
{
    NS_LOG_FUNCTION_NOARGS();

    // Build an Ethernet frame and send it out on all attached channels
    // (used for management / control-plane traffic from the node's IP stack).
    EthernetHeader eth;
    eth.SetSource(Mac48Address::ConvertFrom(src));
    eth.SetDestination(Mac48Address::ConvertFrom(dest));
    eth.SetLengthType(protocolNumber);

    m_macTxTrace(packet);

    for (std::size_t i = 0; i < m_portChannels.size(); ++i)
    {
        Ptr<Packet> frame = packet->Copy();
        frame->AddHeader(eth);
        m_promiscSnifferTrace(frame);
        m_snifferTrace(frame);
        TransmitOn(m_portChannels[i], m_portDeviceIds[i], frame);
    }
    return true;
}

Ptr<Node>
P4SwitchNetDevice::GetNode() const
{
    return m_node;
}

void
P4SwitchNetDevice::SetNode(Ptr<Node> n)
{
    m_node = n;
}

bool
P4SwitchNetDevice::NeedsArp() const
{
    return true;
}

void
P4SwitchNetDevice::SetReceiveCallback(NetDevice::ReceiveCallback cb)
{
    m_rxCallback = cb;
}

void
P4SwitchNetDevice::SetPromiscReceiveCallback(NetDevice::PromiscReceiveCallback cb)
{
    m_promiscRxCallback = cb;
}

bool
P4SwitchNetDevice::SupportsSendFrom() const
{
    return true;
}

} // namespace ns3
