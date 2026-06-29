// Include a header file from your module to test.
#include "ns3/rdma-fabric-helper.h"

#include "ns3/gpu.h"
#include "ns3/ipv4.h"
#include "ns3/node-container.h"
#include "ns3/qbb-helper.h"
#include "ns3/switch-mmu.h"
#include "ns3/switch-node.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"

using namespace ns3;

/**
 * @defgroup rdma-fabric-helper-tests Tests for RdmaFabricHelper
 * @ingroup distributed-ml
 * @ingroup tests
 */

namespace {

// Builds a QbbHelper with the given mtu/delay/data-rate, matching how
// ns3writer.py emits a link helper for a qbb link.
QbbHelper MakeQbbHelper(uint32_t mtu, const char* delay, const char* dataRate) {
    QbbHelper qbb;
    qbb.SetDeviceAttribute("Mtu", UintegerValue(mtu));
    qbb.SetChannelAttribute("Delay", StringValue(delay));
    qbb.SetDeviceAttribute("DataRate", StringValue(dataRate));
    return qbb;
}

} // namespace

/**
 * @ingroup rdma-fabric-helper-tests
 * Builds the exact 3-GPU/1-switch topology from topology/examples/3nodes.topo
 * and asserts RdmaFabricHelper::Build reproduces the literal golden values
 * already baked into simulation/scratch/3nodes.cc (peer win 53500 bytes,
 * base RTT 6028 ns, MMU headroom 26625 / kmin 36000 / kmax 71000 / shift 3).
 */
class RdmaFabric3NodesTestCase : public TestCase {
  public:
    RdmaFabric3NodesTestCase();

  private:
    void DoRun() override;
};

RdmaFabric3NodesTestCase::RdmaFabric3NodesTestCase()
    : TestCase("RdmaFabricHelper reproduces 3nodes.topo golden values") {
}

void RdmaFabric3NodesTestCase::DoRun() {
    NodeContainer gpus;
    for (uint32_t i = 0; i < 3; ++i) {
        gpus.Add(CreateObject<GPU>());
    }
    NodeContainer switches;
    switches.Add(CreateObject<SwitchNode>());

    QbbHelper qbb = MakeQbbHelper(9000, "1us", "71Gbps");
    NetDeviceContainer d0 = qbb.Install(gpus.Get(0), switches.Get(0));
    qbb.Install(gpus.Get(1), switches.Get(0));
    qbb.Install(gpus.Get(2), switches.Get(0));

    RdmaFabricHelper().Build(gpus, switches, NodeContainer());

    Ptr<GPU> gpu0 = DynamicCast<GPU>(gpus.Get(0));
    NS_TEST_ASSERT_MSG_EQ(gpu0->GetMyIp(), Ipv4Address("10.0.0.1"), "gpu0 should get identity IP 10.0.0.1");
    NS_TEST_ASSERT_MSG_EQ(gpu0->GetPeerWin(1), 53500u, "peer win (BDP) must match 3nodes.cc golden value");
    NS_TEST_ASSERT_MSG_EQ(gpu0->GetPeerBaseRtt(1), 6028u, "peer base RTT must match 3nodes.cc golden value");
    NS_TEST_ASSERT_MSG_EQ(gpu0->GetPeerWin(2), 53500u, "peer win (BDP) must match 3nodes.cc golden value");
    NS_TEST_ASSERT_MSG_EQ(gpu0->GetPeerBaseRtt(2), 6028u, "peer base RTT must match 3nodes.cc golden value");

    Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(switches.Get(0));
    uint32_t ifIndex = d0.Get(1)->GetIfIndex();
    NS_TEST_ASSERT_MSG_EQ(sw->m_mmu->headroom[ifIndex], 26625u, "MMU headroom must match 3nodes.cc golden value");
    NS_TEST_ASSERT_MSG_EQ(sw->m_mmu->kmin[ifIndex], 36000u, "MMU kmin (post ConfigEcn *1000 scaling) must match golden value");
    NS_TEST_ASSERT_MSG_EQ(sw->m_mmu->kmax[ifIndex], 71000u, "MMU kmax (post ConfigEcn *1000 scaling) must match golden value");
    NS_TEST_ASSERT_MSG_EQ(sw->m_mmu->pfc_a_shift[ifIndex], 3u, "MMU pfc_a_shift must match golden value");
}

/**
 * @ingroup rdma-fabric-helper-tests
 * Builds the exact 4-GPU/4-switch topology from
 * topology/examples/fat_tree_pod.topo (k=4, no core switches) and asserts
 * the same-rack vs. cross-rack peer win/RTT values match
 * topology/examples/output/fat_tree_pod.cc's literals.
 */
class RdmaFabricFatTreePodTestCase : public TestCase {
  public:
    RdmaFabricFatTreePodTestCase();

  private:
    void DoRun() override;
};

RdmaFabricFatTreePodTestCase::RdmaFabricFatTreePodTestCase()
    : TestCase("RdmaFabricHelper reproduces fat_tree_pod.topo golden values") {
}

void RdmaFabricFatTreePodTestCase::DoRun() {
    NodeContainer gpus;
    for (uint32_t i = 0; i < 4; ++i) {
        gpus.Add(CreateObject<GPU>());
    }
    NodeContainer switches;
    for (uint32_t i = 0; i < 4; ++i) {
        switches.Add(CreateObject<SwitchNode>());
    }

    QbbHelper qbb = MakeQbbHelper(9000, "700ns", "400Gbps");
    // sw0 = esw1.sw, sw1 = esw2.sw, sw2 = asw1, sw3 = asw2
    // gpu0 = esw1.gpu1, gpu1 = esw1.gpu2, gpu2 = esw2.gpu1, gpu3 = esw2.gpu2
    qbb.Install(switches.Get(0), gpus.Get(0));
    qbb.Install(switches.Get(0), gpus.Get(1));
    qbb.Install(switches.Get(1), gpus.Get(2));
    qbb.Install(switches.Get(1), gpus.Get(3));
    qbb.Install(switches.Get(0), switches.Get(2));
    qbb.Install(switches.Get(0), switches.Get(3));
    qbb.Install(switches.Get(1), switches.Get(2));
    qbb.Install(switches.Get(1), switches.Get(3));

    RdmaFabricHelper().Build(gpus, switches, NodeContainer());

    Ptr<GPU> gpu1 = DynamicCast<GPU>(gpus.Get(1)); // esw1.gpu2
    // same-rack peer (gpu0, 2 hops via esw1.sw)
    NS_TEST_ASSERT_MSG_EQ(gpu1->GetPeerWin(0), 158000u, "same-rack peer win must match fat_tree_pod.cc golden value");
    NS_TEST_ASSERT_MSG_EQ(gpu1->GetPeerBaseRtt(0), 3160u, "same-rack peer base RTT must match fat_tree_pod.cc golden value");
    // cross-rack peers (gpu2/gpu3, 4 hops via an aggregation switch)
    NS_TEST_ASSERT_MSG_EQ(gpu1->GetPeerWin(2), 316000u, "cross-rack peer win must match fat_tree_pod.cc golden value");
    NS_TEST_ASSERT_MSG_EQ(gpu1->GetPeerBaseRtt(2), 6320u, "cross-rack peer base RTT must match fat_tree_pod.cc golden value");
    NS_TEST_ASSERT_MSG_EQ(gpu1->GetPeerWin(3), 316000u, "cross-rack peer win must match fat_tree_pod.cc golden value");
    NS_TEST_ASSERT_MSG_EQ(gpu1->GetPeerBaseRtt(3), 6320u, "cross-rack peer base RTT must match fat_tree_pod.cc golden value");
}

/**
 * @ingroup rdma-fabric-helper-tests
 * Regression test for the multi-rail IP-collision fix: a GPU with two qbb
 * NICs (one per switch) must not trigger ns-3's address-collision
 * NS_FATAL_ERROR, and both NICs must end up with the same identity IP.
 */
class RdmaFabricMultiRailTestCase : public TestCase {
  public:
    RdmaFabricMultiRailTestCase();

  private:
    void DoRun() override;
};

RdmaFabricMultiRailTestCase::RdmaFabricMultiRailTestCase()
    : TestCase("RdmaFabricHelper supports multi-rail GPUs without an IP collision") {
}

void RdmaFabricMultiRailTestCase::DoRun() {
    NodeContainer gpus;
    gpus.Add(CreateObject<GPU>());
    NodeContainer switches;
    switches.Add(CreateObject<SwitchNode>());
    switches.Add(CreateObject<SwitchNode>());

    QbbHelper qbb = MakeQbbHelper(9000, "1us", "100Gbps");
    NetDeviceContainer d0 = qbb.Install(gpus.Get(0), switches.Get(0));
    NetDeviceContainer d1 = qbb.Install(gpus.Get(0), switches.Get(1));

    RdmaFabricHelper().Build(gpus, switches, NodeContainer());

    Ptr<Ipv4> ipv4 = gpus.Get(0)->GetObject<Ipv4>();
    int32_t iface0 = ipv4->GetInterfaceForDevice(d0.Get(0));
    int32_t iface1 = ipv4->GetInterfaceForDevice(d1.Get(0));
    NS_TEST_ASSERT_MSG_EQ(ipv4->GetAddress(iface0, 0).GetLocal(), Ipv4Address("10.0.0.1"),
        "first qbb NIC should get the GPU's identity IP");
    NS_TEST_ASSERT_MSG_EQ(ipv4->GetAddress(iface1, 0).GetLocal(), Ipv4Address("10.0.0.1"),
        "second qbb NIC should get the SAME identity IP (multi-rail), not collide");
}

/**
 * @ingroup rdma-fabric-helper-tests
 * TestSuite for module rdma-fabric-helper
 */
class RdmaFabricHelperTestSuite : public TestSuite {
  public:
    RdmaFabricHelperTestSuite();
};

RdmaFabricHelperTestSuite::RdmaFabricHelperTestSuite()
    : TestSuite("rdma-fabric-helper", Type::UNIT) {
    AddTestCase(new RdmaFabric3NodesTestCase, TestCase::Duration::QUICK);
    AddTestCase(new RdmaFabricFatTreePodTestCase, TestCase::Duration::QUICK);
    AddTestCase(new RdmaFabricMultiRailTestCase, TestCase::Duration::QUICK);
}

static RdmaFabricHelperTestSuite sRdmaFabricHelperTestSuite;
