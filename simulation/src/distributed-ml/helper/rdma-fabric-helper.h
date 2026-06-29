#ifndef RDMA_FABRIC_HELPER_H
#define RDMA_FABRIC_HELPER_H

#include "ns3/node-container.h"
#include "ns3/node.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ns3 {

class QbbNetDevice;

/**
 * Builds the RDMA fabric for an already-constructed qbb topology: IP
 * addressing, BFS-ECMP routing tables (SwitchNode/NVSwitchNode/RdmaHw),
 * per-GPU-pair RDMA pacing (bandwidth-delay-product window + base RTT), and
 * switch/nvswitch MMU/PFC/ECN configuration.
 *
 * Unlike a typical ns-3 Helper, this does not install any devices itself --
 * it discovers the qbb link graph from NetDevices/Channels already installed
 * on `gpus`/`switches`/`nvswitches` (the same live-graph-walk idiom as
 * AlgoTopology::ResolveOutPort), so the caller only needs to have built the
 * topology (nodes + qbb links) before calling Build(). This keeps Build()'s
 * cost proportional to topology size at ns-3 runtime, rather than requiring
 * the caller to hand it a precomputed routing table.
 */
class RdmaFabricHelper {
public:
    RdmaFabricHelper() = default;

    /**
     * Installs the internet stack on `gpus`, assigns per-GPU IPs, runs
     * BFS-ECMP routing from every GPU, installs SwitchNode/NVSwitchNode
     * routing tables, creates and wires one RdmaHw/RdmaDriver per GPU, and
     * configures switch/nvswitch MMU/PFC/ECN. Any `Config::SetDefault` calls
     * on `ns3::RdmaHw::*` attributes must be made before calling Build(),
     * since RdmaHw instances are created inside this call.
     *
     * This does NOT set up SwitchNode's CustomFlowForwarding/
     * AddFlowForwardingRule mechanism -- that is driven separately by
     * AlgoTopology::ParseSwitchJson, keyed by the real per-step mscclFlowId
     * from the algorithm XML, after Build() has installed plain ECMP routes.
     *
     * No-op if `gpus` is empty or no qbb links are found.
     */
    void Build(NodeContainer gpus, NodeContainer switches, NodeContainer nvswitches);

private:
    struct QbbEdge {
        Ptr<Node> peer;
        Ptr<QbbNetDevice> dev;
        uint32_t ifIndex;
        Time delay;
        uint64_t rateBps;
        uint32_t mtu;
    };

    struct PathMetrics {
        uint32_t hops;
        Time delay;
        Time txDelay;
        uint64_t bottleneckBps;
    };

    // Node::GetId() -> that node's qbb neighbors (gpu<->gpu p2p links are
    // never part of this graph, matching the original Python's qbb_adj).
    std::unordered_map<uint32_t, std::vector<QbbEdge>> m_adj;
    std::unordered_map<uint32_t, Ptr<Node>> m_nodeById;

    void DiscoverAdjacency(const NodeContainer& gpus, const NodeContainer& switches,
        const NodeContainer& nvswitches);
    // BFS rooted at the destination: returns, for every reachable node,
    // (hop count, accumulated one-way propagation delay, accumulated
    // one-way MTU transmission delay, bottleneck bandwidth) along the
    // shortest-hop path -- mirrors astra-sim's pairBdp/pairRtt derivation.
    std::unordered_map<uint32_t, PathMetrics> BfsFromDestination(uint32_t dstNodeId) const;
    void ConfigureMmu(const NodeContainer& switches, const NodeContainer& nvswitches) const;
};

} // namespace ns3

#endif /* RDMA_FABRIC_HELPER_H */
