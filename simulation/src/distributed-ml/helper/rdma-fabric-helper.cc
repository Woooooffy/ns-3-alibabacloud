#include "rdma-fabric-helper.h"

#include "ns3/assert.h"
#include "ns3/boolean.h"
#include "ns3/config.h"
#include "ns3/gpu.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4.h"
#include "ns3/log.h"
#include "ns3/nvswitch-node.h"
#include "ns3/qbb-channel.h"
#include "ns3/qbb-net-device.h"
#include "ns3/rdma-driver.h"
#include "ns3/rdma-hw.h"
#include "ns3/switch-mmu.h"
#include "ns3/switch-node.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <sstream>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RdmaFabricHelper");

void RdmaFabricHelper::DiscoverAdjacency(const NodeContainer& gpus, const NodeContainer& switches,
    const NodeContainer& nvswitches) {
    NodeContainer all;
    all.Add(gpus);
    all.Add(switches);
    all.Add(nvswitches);

    for (uint32_t n = 0; n < all.GetN(); ++n) {
        Ptr<Node> node = all.Get(n);
        m_nodeById[node->GetId()] = node;

        for (uint32_t i = 0; i < node->GetNDevices(); ++i) {
            Ptr<NetDevice> dev = node->GetDevice(i);
            Ptr<QbbChannel> channel = DynamicCast<QbbChannel>(dev->GetChannel());
            if (!channel) {
                continue; // not a qbb link (e.g. a gpu<->gpu p2p PointToPointNetDevice)
            }
            Ptr<NetDevice> otherDev = (channel->GetDevice(0) == dev) ? channel->GetDevice(1) : channel->GetDevice(0);
            Ptr<QbbNetDevice> qbbDev = DynamicCast<QbbNetDevice>(dev);
            NS_ASSERT_MSG(qbbDev, "RdmaFabricHelper: device on a QbbChannel is not a QbbNetDevice");

            QbbEdge edge;
            edge.peer = otherDev->GetNode();
            edge.dev = qbbDev;
            edge.ifIndex = qbbDev->GetIfIndex();
            edge.delay = channel->GetDelay();
            edge.rateBps = qbbDev->GetDataRate().GetBitRate();
            edge.mtu = qbbDev->GetMtu();
            m_adj[node->GetId()].push_back(edge);
        }
    }
}

std::unordered_map<uint32_t, RdmaFabricHelper::PathMetrics> RdmaFabricHelper::BfsFromDestination(
    uint32_t dstNodeId) const {
    std::unordered_map<uint32_t, PathMetrics> metrics;
    metrics[dstNodeId] = PathMetrics{0, Time(0), Time(0), std::numeric_limits<uint64_t>::max()};

    std::queue<uint32_t> q;
    q.push(dstNodeId);
    while (!q.empty()) {
        uint32_t cur = q.front();
        q.pop();
        PathMetrics curMetrics = metrics[cur];
        auto it = m_adj.find(cur);
        if (it == m_adj.end()) {
            continue;
        }
        for (const QbbEdge& edge : it->second) {
            uint32_t peerId = edge.peer->GetId();
            if (metrics.count(peerId)) {
                continue;
            }
            Time txDelay = Seconds(edge.mtu * 8.0 / edge.rateBps);
            metrics[peerId] = PathMetrics{
                curMetrics.hops + 1,
                curMetrics.delay + edge.delay,
                curMetrics.txDelay + txDelay,
                std::min(curMetrics.bottleneckBps, edge.rateBps),
            };
            q.push(peerId);
        }
    }
    return metrics;
}

void RdmaFabricHelper::Build(NodeContainer gpus, NodeContainer switches, NodeContainer nvswitches) {
    if (gpus.GetN() == 0) {
        return;
    }

    DiscoverAdjacency(gpus, switches, nvswitches);
    if (m_adj.empty()) {
        return; // pure p2p topology, no RDMA fabric to wire up
    }

    InternetStackHelper internetStack;
    internetStack.Install(gpus);

    // GPUsPerServer perf hint (RdmaHw routing correctness is governed purely
    // by the explicit is_nvswitch table entries added below, regardless of
    // this value): count GPUs attached to the first discovered nvswitch.
    if (nvswitches.GetN() > 0) {
        uint32_t attachedGpus = 0;
        for (const QbbEdge& edge : m_adj[nvswitches.Get(0)->GetId()]) {
            if (DynamicCast<GPU>(edge.peer)) {
                ++attachedGpus;
            }
        }
        Config::SetDefault("ns3::RdmaHw::GPUsPerServer", UintegerValue(attachedGpus > 0 ? attachedGpus : 1));
    }

    // Assign IPs (GPU index = position in `gpus`, matching ns3codegen.py's
    // gpu_counter order) and create the RdmaHw/RdmaDriver pair per GPU.
    // Every qbb NIC a GPU owns gets the SAME identity address -- the RdmaHw
    // fabric is multi-rail/ECMP capable -- assigned directly via Ipv4
    // (bypassing Ipv4AddressHelper/Ipv4AddressGenerator's global collision
    // tracking, which would otherwise abort on the second NIC reusing the
    // same address).
    std::vector<Ipv4Address> gpuIp(gpus.GetN());
    std::vector<Ptr<RdmaHw>> rdmaHwOf(gpus.GetN());
    std::unordered_map<uint32_t, uint32_t> gpuIndexByNodeId;
    for (uint32_t i = 0; i < gpus.GetN(); ++i) {
        Ptr<Node> gpuNode = gpus.Get(i);
        gpuIndexByNodeId[gpuNode->GetId()] = i;

        std::ostringstream oss;
        oss << "10.0.0." << (i + 1);
        Ipv4Address ip(oss.str().c_str());
        gpuIp[i] = ip;

        Ptr<Ipv4> ipv4 = gpuNode->GetObject<Ipv4>();
        for (const QbbEdge& edge : m_adj[gpuNode->GetId()]) {
            int32_t iface = ipv4->GetInterfaceForDevice(edge.dev);
            if (iface == -1) {
                iface = ipv4->AddInterface(edge.dev);
            }
            ipv4->AddAddress(iface, Ipv4InterfaceAddress(ip, Ipv4Mask("255.0.0.0")));
            ipv4->SetMetric(iface, 1);
            ipv4->SetUp(iface);
        }
        DynamicCast<GPU>(gpuNode)->SetMyIp(ip);

        Ptr<RdmaHw> rdmaHw = CreateObject<RdmaHw>();
        Ptr<RdmaDriver> rdmaDriver = CreateObject<RdmaDriver>();
        rdmaDriver->SetNode(gpuNode);
        rdmaDriver->SetRdmaHw(rdmaHw);
        rdmaDriver->Init();
        gpuNode->AggregateObject(rdmaDriver);
        rdmaHwOf[i] = rdmaHw;
    }

    // BFS-ECMP rooted at each destination GPU. GPUs are always leaves of the
    // qbb subgraph (gpu<->gpu p2p links never appear in m_adj), so a node's
    // neighbors-at-distance-1-closer-to-dst are exactly its valid ECMP next
    // hops toward that destination.
    for (uint32_t dstIdx = 0; dstIdx < gpus.GetN(); ++dstIdx) {
        Ptr<Node> dstGpu = gpus.Get(dstIdx);
        uint32_t dstNodeId = dstGpu->GetId();
        Ipv4Address dstIp = gpuIp[dstIdx];

        std::unordered_map<uint32_t, PathMetrics> metrics = BfsFromDestination(dstNodeId);
        for (const auto& kv : metrics) {
            uint32_t nodeId = kv.first;
            if (nodeId == dstNodeId) {
                continue;
            }
            const PathMetrics& m = kv.second;
            std::vector<const QbbEdge*> nextHops;
            for (const QbbEdge& edge : m_adj[nodeId]) {
                auto peerIt = metrics.find(edge.peer->GetId());
                if (peerIt != metrics.end() && peerIt->second.hops == m.hops - 1) {
                    nextHops.push_back(&edge);
                }
            }
            if (nextHops.empty()) {
                continue;
            }
            Ptr<Node> node = m_nodeById[nodeId];

            if (Ptr<GPU> gpu = DynamicCast<GPU>(node)) {
                Time rtt = m.delay * 2 + m.txDelay;
                uint64_t rttNs = static_cast<uint64_t>(rtt.GetNanoSeconds());
                double bdpBytes = static_cast<double>(m.bottleneckBps) * static_cast<double>(rttNs) / 1e9 / 8.0;
                uint32_t winBytes = std::max(1u, static_cast<uint32_t>(std::round(bdpBytes)));
                gpu->PushPeerIpAddr(dstIdx, dstIp);
                gpu->PushPeerWin(dstIdx, winBytes);
                gpu->PushPeerBaseRtt(dstIdx, rttNs);

                Ptr<RdmaHw> hw = rdmaHwOf[gpuIndexByNodeId[nodeId]];
                for (const QbbEdge* edge : nextHops) {
                    bool isNvswitch = DynamicCast<NVSwitchNode>(edge->peer) != nullptr;
                    hw->AddTableEntry(dstIp, edge->ifIndex, isNvswitch);
                }
            } else if (Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(node)) {
                // Custom per-flow forwarding (CustomFlowForwarding +
                // AddFlowForwardingRule) is intentionally NOT set up here.
                // That mechanism is driven by AlgoTopology::ParseSwitchJson,
                // keyed by the real per-step mscclFlowId from the algorithm
                // XML -- a topology-build-time placeholder scheme here would
                // risk colliding with those real flow ids and would make it
                // impossible to tell whether a custom multipath scheme under
                // test is actually responsible for observed forwarding.
                for (const QbbEdge* edge : nextHops) {
                    sw->AddTableEntry(dstIp, edge->ifIndex);
                }
            } else if (Ptr<NVSwitchNode> nvsw = DynamicCast<NVSwitchNode>(node)) {
                for (const QbbEdge* edge : nextHops) {
                    nvsw->AddTableEntry(dstIp, edge->ifIndex);
                }
            }
        }
    }

    ConfigureMmu(switches, nvswitches);
}

void RdmaFabricHelper::ConfigureMmu(const NodeContainer& switches, const NodeContainer& nvswitches) const {
    if (m_adj.empty()) {
        return;
    }

    uint64_t baselineRateBps = std::numeric_limits<uint64_t>::max();
    for (const auto& kv : m_adj) {
        for (const QbbEdge& edge : kv.second) {
            baselineRateBps = std::min(baselineRateBps, edge.rateBps);
        }
    }

    NodeContainer allSwitches;
    allSwitches.Add(switches);
    allSwitches.Add(nvswitches);

    for (uint32_t n = 0; n < allSwitches.GetN(); ++n) {
        Ptr<Node> node = allSwitches.Get(n);
        auto it = m_adj.find(node->GetId());
        if (it == m_adj.end() || it->second.empty()) {
            continue;
        }

        Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(node);
        Ptr<NVSwitchNode> nvsw = DynamicCast<NVSwitchNode>(node);
        Ptr<SwitchMmu> mmu = sw ? sw->m_mmu : nvsw->m_mmu;

        // Fix vs. the original Python-generated code: ConfigNPort sums
        // headroom[1..n_port] into total_hdrm at call time, so every
        // ConfigHdrm call for this switch's ports must happen first.
        for (const QbbEdge& edge : it->second) {
            double delaySec = edge.delay.GetSeconds();
            double rateBps = static_cast<double>(edge.rateBps);
            uint32_t headroomBytes = static_cast<uint32_t>(std::round(rateBps * delaySec * 3 / 8)); // 3 link-RTTs of headroom
            double kminBytes = rateBps / 8 * 4e-6; // ~4us of queueing before ECN starts marking
            double kmaxBytes = kminBytes * 2;
            uint32_t shift = 3; // 1/8 of remaining buffer, astra-sim default
            double rate = rateBps;
            while (rate > baselineRateBps && shift > 0) {
                --shift;
                rate /= 2;
            }
            mmu->ConfigHdrm(edge.ifIndex, headroomBytes);
            mmu->ConfigEcn(edge.ifIndex, static_cast<uint32_t>(std::round(kminBytes / 1000)),
                static_cast<uint32_t>(std::round(kmaxBytes / 1000)), 0.2);
            mmu->pfc_a_shift[edge.ifIndex] = shift;
        }
        mmu->ConfigNPort(static_cast<uint32_t>(it->second.size()));
        mmu->node_id = node->GetId();

        if (sw) {
            sw->SetAttribute("EcnEnabled", BooleanValue(true));
        }
    }
}

} // namespace ns3
