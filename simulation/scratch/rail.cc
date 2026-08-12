#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/distributed-ml-module.h"

#include <sys/stat.h>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <tuple>

using namespace ns3;

// ---- event-driven congestion monitoring ----------------------------------------------
// These are TracedCallbacks fired synchronously from inside existing packet events on the
// switch egress ports (QbbNetDevice's QbbEnqueue/QbbDequeue/QbbDrop/QbbPfc trace sources).
// Because they run as a side effect of events already in the queue, they schedule NOTHING
// of their own -- the simulator's event list, its natural termination, and Simulator::Now()
// (hence the reported algorithm latency/bandwidth) are all completely unaffected.

// running per-(switch id, port ifIndex, priority queue) egress occupancy in bytes,
// reconstructed from enqueue/dequeue deltas so each row carries the exact post-event depth.
static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int64_t> g_qBytes;

// QbbEnqueue: fires just before a packet is pushed onto egress queue `qIndex`.
static void OnSwitchEnqueue(FILE* out, uint32_t swId, uint32_t port, Ptr<const Packet> p, uint32_t qIndex) {
    int64_t& depth = g_qBytes[std::make_tuple(swId, port, qIndex)];
    depth += p->GetSize();
    fprintf(out, "%ld,%u,%u,%u,%ld,enq\n", Simulator::Now().GetNanoSeconds(), swId, port, qIndex, depth);
}

// QbbDequeue: fires as a packet leaves egress queue `qIndex` onto the wire.
static void OnSwitchDequeue(FILE* out, uint32_t swId, uint32_t port, Ptr<const Packet> p, uint32_t qIndex) {
    int64_t& depth = g_qBytes[std::make_tuple(swId, port, qIndex)];
    depth -= p->GetSize();
    if (depth < 0) depth = 0; // guard against control pkts (e.g. PFC) not counted on enqueue
    fprintf(out, "%ld,%u,%u,%u,%ld,deq\n", Simulator::Now().GetNanoSeconds(), swId, port, qIndex, depth);
}

// QbbDrop: fires when admission control / buffer overflow discards a packet.
static void OnSwitchDrop(FILE* out, uint32_t swId, uint32_t port, Ptr<const Packet> p, uint32_t qIndex) {
    fprintf(out, "%ld,%u,%u,%u,%u,drop\n", Simulator::Now().GetNanoSeconds(), swId, port, qIndex, p->GetSize());
}

// QbbPfc: type 1 = PAUSE sent upstream (this port's ingress is congested), 0 = RESUME.
// q_id and bytes columns are left blank so PFC rows share the drop event schema.
static void OnSwitchPfc(FILE* out, uint32_t swId, uint32_t port, uint32_t type) {
    fprintf(out, "%ld,%u,%u,,,%s\n", Simulator::Now().GetNanoSeconds(), swId, port, type == 1 ? "pause" : "resume");
}

using namespace ns3;

int main(int argc, char *argv[]) {
    NS_LOG_COMPONENT_DEFINE("HETERO_CLUSTER");
//    LogComponentEnable("CollectivesApplication", LOG_INFO);
		// LogComponentEnable("SwitchNode", LOG_LEVEL_INFO);
		LogComponentEnable("AlgoTopo", LOG_LEVEL_WARN);
    uint32_t inputBytes = (1 << 20);
    // label distinguishes output files between runs, e.g. --label=with_rate vs --label=no_rate
    std::string label = "rail";
    // Verification logging. This topology has 256 GPUs, so a full per-buffer dump is hundreds of
    // GB of text and will hang or OOM the run; minimal mode prints only the first few mismatches
    // (and then stops scanning) -- enough to localize a bug. "verbose" is for small debug topologies.
    std::string checkLog = "minimal"; // silent | minimal | verbose
    uint32_t maxMismatches = 10;
    CommandLine cmd;
    cmd.AddValue("inputBytes", "Total input size in bytes", inputBytes);
    cmd.AddValue("label", "Suffix for the congestion-monitor output CSVs", label);
    cmd.AddValue("checkLog", "Correctness-check logging: silent | minimal | verbose", checkLog);
    cmd.AddValue("maxMismatches", "Mismatch lines to print before giving up (minimal mode)", maxMismatches);
    cmd.Parse(argc, argv);

    NodeContainer gpunodes;
    NodeContainer regswtches;
    NodeContainer nvswtches;

    // PFC backpressure (CheckAndSendPfc) runs unconditionally in SwitchNode, but only
    // has an effect once QcnEnabled lets a stalled NIC's queue resume; ECN marking is
    // separately gated per-switch by the EcnEnabled attribute set below.
    Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(true));

    for (uint32_t i = 0; i < 256; ++i) { gpunodes.Add(CreateObject<GPU>()); }
    for (uint32_t i = 0; i < 12; ++i) { regswtches.Add(CreateObject<SwitchNode>()); }
    for (uint32_t i = 0; i < 32; ++i) { nvswtches.Add(CreateObject<NVSwitchNode>()); }
    QbbHelper link_helper0;
    link_helper0.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper0.SetChannelAttribute("Delay", StringValue("100ns"));
    link_helper0.SetDeviceAttribute("DataRate", StringValue("1800GBps"));

    QbbHelper link_helper1;
    link_helper1.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper1.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper1.SetDeviceAttribute("DataRate", StringValue("400Gbps"));

    QbbHelper link_helper2;
    link_helper2.SetDeviceAttribute("Mtu", UintegerValue(4096));
    link_helper2.SetChannelAttribute("Delay", StringValue("700ns"));
    link_helper2.SetDeviceAttribute("DataRate", StringValue("3200Gbps"));

    NetDeviceContainer devs0_0 = link_helper0.Install(gpunodes.Get(0), nvswtches.Get(0));
    NetDeviceContainer devs0_1 = link_helper0.Install(gpunodes.Get(1), nvswtches.Get(0));
    NetDeviceContainer devs0_2 = link_helper0.Install(gpunodes.Get(2), nvswtches.Get(0));
    NetDeviceContainer devs0_3 = link_helper0.Install(gpunodes.Get(3), nvswtches.Get(0));
    NetDeviceContainer devs0_4 = link_helper0.Install(gpunodes.Get(4), nvswtches.Get(0));
    NetDeviceContainer devs0_5 = link_helper0.Install(gpunodes.Get(5), nvswtches.Get(0));
    NetDeviceContainer devs0_6 = link_helper0.Install(gpunodes.Get(6), nvswtches.Get(0));
    NetDeviceContainer devs0_7 = link_helper0.Install(gpunodes.Get(7), nvswtches.Get(0));
    NetDeviceContainer devs0_8 = link_helper0.Install(gpunodes.Get(8), nvswtches.Get(1));
    NetDeviceContainer devs0_9 = link_helper0.Install(gpunodes.Get(9), nvswtches.Get(1));
    NetDeviceContainer devs0_10 = link_helper0.Install(gpunodes.Get(10), nvswtches.Get(1));
    NetDeviceContainer devs0_11 = link_helper0.Install(gpunodes.Get(11), nvswtches.Get(1));
    NetDeviceContainer devs0_12 = link_helper0.Install(gpunodes.Get(12), nvswtches.Get(1));
    NetDeviceContainer devs0_13 = link_helper0.Install(gpunodes.Get(13), nvswtches.Get(1));
    NetDeviceContainer devs0_14 = link_helper0.Install(gpunodes.Get(14), nvswtches.Get(1));
    NetDeviceContainer devs0_15 = link_helper0.Install(gpunodes.Get(15), nvswtches.Get(1));
    NetDeviceContainer devs0_16 = link_helper0.Install(gpunodes.Get(16), nvswtches.Get(2));
    NetDeviceContainer devs0_17 = link_helper0.Install(gpunodes.Get(17), nvswtches.Get(2));
    NetDeviceContainer devs0_18 = link_helper0.Install(gpunodes.Get(18), nvswtches.Get(2));
    NetDeviceContainer devs0_19 = link_helper0.Install(gpunodes.Get(19), nvswtches.Get(2));
    NetDeviceContainer devs0_20 = link_helper0.Install(gpunodes.Get(20), nvswtches.Get(2));
    NetDeviceContainer devs0_21 = link_helper0.Install(gpunodes.Get(21), nvswtches.Get(2));
    NetDeviceContainer devs0_22 = link_helper0.Install(gpunodes.Get(22), nvswtches.Get(2));
    NetDeviceContainer devs0_23 = link_helper0.Install(gpunodes.Get(23), nvswtches.Get(2));
    NetDeviceContainer devs0_24 = link_helper0.Install(gpunodes.Get(24), nvswtches.Get(3));
    NetDeviceContainer devs0_25 = link_helper0.Install(gpunodes.Get(25), nvswtches.Get(3));
    NetDeviceContainer devs0_26 = link_helper0.Install(gpunodes.Get(26), nvswtches.Get(3));
    NetDeviceContainer devs0_27 = link_helper0.Install(gpunodes.Get(27), nvswtches.Get(3));
    NetDeviceContainer devs0_28 = link_helper0.Install(gpunodes.Get(28), nvswtches.Get(3));
    NetDeviceContainer devs0_29 = link_helper0.Install(gpunodes.Get(29), nvswtches.Get(3));
    NetDeviceContainer devs0_30 = link_helper0.Install(gpunodes.Get(30), nvswtches.Get(3));
    NetDeviceContainer devs0_31 = link_helper0.Install(gpunodes.Get(31), nvswtches.Get(3));
    NetDeviceContainer devs0_32 = link_helper0.Install(gpunodes.Get(32), nvswtches.Get(4));
    NetDeviceContainer devs0_33 = link_helper0.Install(gpunodes.Get(33), nvswtches.Get(4));
    NetDeviceContainer devs0_34 = link_helper0.Install(gpunodes.Get(34), nvswtches.Get(4));
    NetDeviceContainer devs0_35 = link_helper0.Install(gpunodes.Get(35), nvswtches.Get(4));
    NetDeviceContainer devs0_36 = link_helper0.Install(gpunodes.Get(36), nvswtches.Get(4));
    NetDeviceContainer devs0_37 = link_helper0.Install(gpunodes.Get(37), nvswtches.Get(4));
    NetDeviceContainer devs0_38 = link_helper0.Install(gpunodes.Get(38), nvswtches.Get(4));
    NetDeviceContainer devs0_39 = link_helper0.Install(gpunodes.Get(39), nvswtches.Get(4));
    NetDeviceContainer devs0_40 = link_helper0.Install(gpunodes.Get(40), nvswtches.Get(5));
    NetDeviceContainer devs0_41 = link_helper0.Install(gpunodes.Get(41), nvswtches.Get(5));
    NetDeviceContainer devs0_42 = link_helper0.Install(gpunodes.Get(42), nvswtches.Get(5));
    NetDeviceContainer devs0_43 = link_helper0.Install(gpunodes.Get(43), nvswtches.Get(5));
    NetDeviceContainer devs0_44 = link_helper0.Install(gpunodes.Get(44), nvswtches.Get(5));
    NetDeviceContainer devs0_45 = link_helper0.Install(gpunodes.Get(45), nvswtches.Get(5));
    NetDeviceContainer devs0_46 = link_helper0.Install(gpunodes.Get(46), nvswtches.Get(5));
    NetDeviceContainer devs0_47 = link_helper0.Install(gpunodes.Get(47), nvswtches.Get(5));
    NetDeviceContainer devs0_48 = link_helper0.Install(gpunodes.Get(48), nvswtches.Get(6));
    NetDeviceContainer devs0_49 = link_helper0.Install(gpunodes.Get(49), nvswtches.Get(6));
    NetDeviceContainer devs0_50 = link_helper0.Install(gpunodes.Get(50), nvswtches.Get(6));
    NetDeviceContainer devs0_51 = link_helper0.Install(gpunodes.Get(51), nvswtches.Get(6));
    NetDeviceContainer devs0_52 = link_helper0.Install(gpunodes.Get(52), nvswtches.Get(6));
    NetDeviceContainer devs0_53 = link_helper0.Install(gpunodes.Get(53), nvswtches.Get(6));
    NetDeviceContainer devs0_54 = link_helper0.Install(gpunodes.Get(54), nvswtches.Get(6));
    NetDeviceContainer devs0_55 = link_helper0.Install(gpunodes.Get(55), nvswtches.Get(6));
    NetDeviceContainer devs0_56 = link_helper0.Install(gpunodes.Get(56), nvswtches.Get(7));
    NetDeviceContainer devs0_57 = link_helper0.Install(gpunodes.Get(57), nvswtches.Get(7));
    NetDeviceContainer devs0_58 = link_helper0.Install(gpunodes.Get(58), nvswtches.Get(7));
    NetDeviceContainer devs0_59 = link_helper0.Install(gpunodes.Get(59), nvswtches.Get(7));
    NetDeviceContainer devs0_60 = link_helper0.Install(gpunodes.Get(60), nvswtches.Get(7));
    NetDeviceContainer devs0_61 = link_helper0.Install(gpunodes.Get(61), nvswtches.Get(7));
    NetDeviceContainer devs0_62 = link_helper0.Install(gpunodes.Get(62), nvswtches.Get(7));
    NetDeviceContainer devs0_63 = link_helper0.Install(gpunodes.Get(63), nvswtches.Get(7));
    NetDeviceContainer devs0_64 = link_helper0.Install(gpunodes.Get(64), nvswtches.Get(8));
    NetDeviceContainer devs0_65 = link_helper0.Install(gpunodes.Get(65), nvswtches.Get(8));
    NetDeviceContainer devs0_66 = link_helper0.Install(gpunodes.Get(66), nvswtches.Get(8));
    NetDeviceContainer devs0_67 = link_helper0.Install(gpunodes.Get(67), nvswtches.Get(8));
    NetDeviceContainer devs0_68 = link_helper0.Install(gpunodes.Get(68), nvswtches.Get(8));
    NetDeviceContainer devs0_69 = link_helper0.Install(gpunodes.Get(69), nvswtches.Get(8));
    NetDeviceContainer devs0_70 = link_helper0.Install(gpunodes.Get(70), nvswtches.Get(8));
    NetDeviceContainer devs0_71 = link_helper0.Install(gpunodes.Get(71), nvswtches.Get(8));
    NetDeviceContainer devs0_72 = link_helper0.Install(gpunodes.Get(72), nvswtches.Get(9));
    NetDeviceContainer devs0_73 = link_helper0.Install(gpunodes.Get(73), nvswtches.Get(9));
    NetDeviceContainer devs0_74 = link_helper0.Install(gpunodes.Get(74), nvswtches.Get(9));
    NetDeviceContainer devs0_75 = link_helper0.Install(gpunodes.Get(75), nvswtches.Get(9));
    NetDeviceContainer devs0_76 = link_helper0.Install(gpunodes.Get(76), nvswtches.Get(9));
    NetDeviceContainer devs0_77 = link_helper0.Install(gpunodes.Get(77), nvswtches.Get(9));
    NetDeviceContainer devs0_78 = link_helper0.Install(gpunodes.Get(78), nvswtches.Get(9));
    NetDeviceContainer devs0_79 = link_helper0.Install(gpunodes.Get(79), nvswtches.Get(9));
    NetDeviceContainer devs0_80 = link_helper0.Install(gpunodes.Get(80), nvswtches.Get(10));
    NetDeviceContainer devs0_81 = link_helper0.Install(gpunodes.Get(81), nvswtches.Get(10));
    NetDeviceContainer devs0_82 = link_helper0.Install(gpunodes.Get(82), nvswtches.Get(10));
    NetDeviceContainer devs0_83 = link_helper0.Install(gpunodes.Get(83), nvswtches.Get(10));
    NetDeviceContainer devs0_84 = link_helper0.Install(gpunodes.Get(84), nvswtches.Get(10));
    NetDeviceContainer devs0_85 = link_helper0.Install(gpunodes.Get(85), nvswtches.Get(10));
    NetDeviceContainer devs0_86 = link_helper0.Install(gpunodes.Get(86), nvswtches.Get(10));
    NetDeviceContainer devs0_87 = link_helper0.Install(gpunodes.Get(87), nvswtches.Get(10));
    NetDeviceContainer devs0_88 = link_helper0.Install(gpunodes.Get(88), nvswtches.Get(11));
    NetDeviceContainer devs0_89 = link_helper0.Install(gpunodes.Get(89), nvswtches.Get(11));
    NetDeviceContainer devs0_90 = link_helper0.Install(gpunodes.Get(90), nvswtches.Get(11));
    NetDeviceContainer devs0_91 = link_helper0.Install(gpunodes.Get(91), nvswtches.Get(11));
    NetDeviceContainer devs0_92 = link_helper0.Install(gpunodes.Get(92), nvswtches.Get(11));
    NetDeviceContainer devs0_93 = link_helper0.Install(gpunodes.Get(93), nvswtches.Get(11));
    NetDeviceContainer devs0_94 = link_helper0.Install(gpunodes.Get(94), nvswtches.Get(11));
    NetDeviceContainer devs0_95 = link_helper0.Install(gpunodes.Get(95), nvswtches.Get(11));
    NetDeviceContainer devs0_96 = link_helper0.Install(gpunodes.Get(96), nvswtches.Get(12));
    NetDeviceContainer devs0_97 = link_helper0.Install(gpunodes.Get(97), nvswtches.Get(12));
    NetDeviceContainer devs0_98 = link_helper0.Install(gpunodes.Get(98), nvswtches.Get(12));
    NetDeviceContainer devs0_99 = link_helper0.Install(gpunodes.Get(99), nvswtches.Get(12));
    NetDeviceContainer devs0_100 = link_helper0.Install(gpunodes.Get(100), nvswtches.Get(12));
    NetDeviceContainer devs0_101 = link_helper0.Install(gpunodes.Get(101), nvswtches.Get(12));
    NetDeviceContainer devs0_102 = link_helper0.Install(gpunodes.Get(102), nvswtches.Get(12));
    NetDeviceContainer devs0_103 = link_helper0.Install(gpunodes.Get(103), nvswtches.Get(12));
    NetDeviceContainer devs0_104 = link_helper0.Install(gpunodes.Get(104), nvswtches.Get(13));
    NetDeviceContainer devs0_105 = link_helper0.Install(gpunodes.Get(105), nvswtches.Get(13));
    NetDeviceContainer devs0_106 = link_helper0.Install(gpunodes.Get(106), nvswtches.Get(13));
    NetDeviceContainer devs0_107 = link_helper0.Install(gpunodes.Get(107), nvswtches.Get(13));
    NetDeviceContainer devs0_108 = link_helper0.Install(gpunodes.Get(108), nvswtches.Get(13));
    NetDeviceContainer devs0_109 = link_helper0.Install(gpunodes.Get(109), nvswtches.Get(13));
    NetDeviceContainer devs0_110 = link_helper0.Install(gpunodes.Get(110), nvswtches.Get(13));
    NetDeviceContainer devs0_111 = link_helper0.Install(gpunodes.Get(111), nvswtches.Get(13));
    NetDeviceContainer devs0_112 = link_helper0.Install(gpunodes.Get(112), nvswtches.Get(14));
    NetDeviceContainer devs0_113 = link_helper0.Install(gpunodes.Get(113), nvswtches.Get(14));
    NetDeviceContainer devs0_114 = link_helper0.Install(gpunodes.Get(114), nvswtches.Get(14));
    NetDeviceContainer devs0_115 = link_helper0.Install(gpunodes.Get(115), nvswtches.Get(14));
    NetDeviceContainer devs0_116 = link_helper0.Install(gpunodes.Get(116), nvswtches.Get(14));
    NetDeviceContainer devs0_117 = link_helper0.Install(gpunodes.Get(117), nvswtches.Get(14));
    NetDeviceContainer devs0_118 = link_helper0.Install(gpunodes.Get(118), nvswtches.Get(14));
    NetDeviceContainer devs0_119 = link_helper0.Install(gpunodes.Get(119), nvswtches.Get(14));
    NetDeviceContainer devs0_120 = link_helper0.Install(gpunodes.Get(120), nvswtches.Get(15));
    NetDeviceContainer devs0_121 = link_helper0.Install(gpunodes.Get(121), nvswtches.Get(15));
    NetDeviceContainer devs0_122 = link_helper0.Install(gpunodes.Get(122), nvswtches.Get(15));
    NetDeviceContainer devs0_123 = link_helper0.Install(gpunodes.Get(123), nvswtches.Get(15));
    NetDeviceContainer devs0_124 = link_helper0.Install(gpunodes.Get(124), nvswtches.Get(15));
    NetDeviceContainer devs0_125 = link_helper0.Install(gpunodes.Get(125), nvswtches.Get(15));
    NetDeviceContainer devs0_126 = link_helper0.Install(gpunodes.Get(126), nvswtches.Get(15));
    NetDeviceContainer devs0_127 = link_helper0.Install(gpunodes.Get(127), nvswtches.Get(15));
    NetDeviceContainer devs0_128 = link_helper0.Install(gpunodes.Get(128), nvswtches.Get(16));
    NetDeviceContainer devs0_129 = link_helper0.Install(gpunodes.Get(129), nvswtches.Get(16));
    NetDeviceContainer devs0_130 = link_helper0.Install(gpunodes.Get(130), nvswtches.Get(16));
    NetDeviceContainer devs0_131 = link_helper0.Install(gpunodes.Get(131), nvswtches.Get(16));
    NetDeviceContainer devs0_132 = link_helper0.Install(gpunodes.Get(132), nvswtches.Get(16));
    NetDeviceContainer devs0_133 = link_helper0.Install(gpunodes.Get(133), nvswtches.Get(16));
    NetDeviceContainer devs0_134 = link_helper0.Install(gpunodes.Get(134), nvswtches.Get(16));
    NetDeviceContainer devs0_135 = link_helper0.Install(gpunodes.Get(135), nvswtches.Get(16));
    NetDeviceContainer devs0_136 = link_helper0.Install(gpunodes.Get(136), nvswtches.Get(17));
    NetDeviceContainer devs0_137 = link_helper0.Install(gpunodes.Get(137), nvswtches.Get(17));
    NetDeviceContainer devs0_138 = link_helper0.Install(gpunodes.Get(138), nvswtches.Get(17));
    NetDeviceContainer devs0_139 = link_helper0.Install(gpunodes.Get(139), nvswtches.Get(17));
    NetDeviceContainer devs0_140 = link_helper0.Install(gpunodes.Get(140), nvswtches.Get(17));
    NetDeviceContainer devs0_141 = link_helper0.Install(gpunodes.Get(141), nvswtches.Get(17));
    NetDeviceContainer devs0_142 = link_helper0.Install(gpunodes.Get(142), nvswtches.Get(17));
    NetDeviceContainer devs0_143 = link_helper0.Install(gpunodes.Get(143), nvswtches.Get(17));
    NetDeviceContainer devs0_144 = link_helper0.Install(gpunodes.Get(144), nvswtches.Get(18));
    NetDeviceContainer devs0_145 = link_helper0.Install(gpunodes.Get(145), nvswtches.Get(18));
    NetDeviceContainer devs0_146 = link_helper0.Install(gpunodes.Get(146), nvswtches.Get(18));
    NetDeviceContainer devs0_147 = link_helper0.Install(gpunodes.Get(147), nvswtches.Get(18));
    NetDeviceContainer devs0_148 = link_helper0.Install(gpunodes.Get(148), nvswtches.Get(18));
    NetDeviceContainer devs0_149 = link_helper0.Install(gpunodes.Get(149), nvswtches.Get(18));
    NetDeviceContainer devs0_150 = link_helper0.Install(gpunodes.Get(150), nvswtches.Get(18));
    NetDeviceContainer devs0_151 = link_helper0.Install(gpunodes.Get(151), nvswtches.Get(18));
    NetDeviceContainer devs0_152 = link_helper0.Install(gpunodes.Get(152), nvswtches.Get(19));
    NetDeviceContainer devs0_153 = link_helper0.Install(gpunodes.Get(153), nvswtches.Get(19));
    NetDeviceContainer devs0_154 = link_helper0.Install(gpunodes.Get(154), nvswtches.Get(19));
    NetDeviceContainer devs0_155 = link_helper0.Install(gpunodes.Get(155), nvswtches.Get(19));
    NetDeviceContainer devs0_156 = link_helper0.Install(gpunodes.Get(156), nvswtches.Get(19));
    NetDeviceContainer devs0_157 = link_helper0.Install(gpunodes.Get(157), nvswtches.Get(19));
    NetDeviceContainer devs0_158 = link_helper0.Install(gpunodes.Get(158), nvswtches.Get(19));
    NetDeviceContainer devs0_159 = link_helper0.Install(gpunodes.Get(159), nvswtches.Get(19));
    NetDeviceContainer devs0_160 = link_helper0.Install(gpunodes.Get(160), nvswtches.Get(20));
    NetDeviceContainer devs0_161 = link_helper0.Install(gpunodes.Get(161), nvswtches.Get(20));
    NetDeviceContainer devs0_162 = link_helper0.Install(gpunodes.Get(162), nvswtches.Get(20));
    NetDeviceContainer devs0_163 = link_helper0.Install(gpunodes.Get(163), nvswtches.Get(20));
    NetDeviceContainer devs0_164 = link_helper0.Install(gpunodes.Get(164), nvswtches.Get(20));
    NetDeviceContainer devs0_165 = link_helper0.Install(gpunodes.Get(165), nvswtches.Get(20));
    NetDeviceContainer devs0_166 = link_helper0.Install(gpunodes.Get(166), nvswtches.Get(20));
    NetDeviceContainer devs0_167 = link_helper0.Install(gpunodes.Get(167), nvswtches.Get(20));
    NetDeviceContainer devs0_168 = link_helper0.Install(gpunodes.Get(168), nvswtches.Get(21));
    NetDeviceContainer devs0_169 = link_helper0.Install(gpunodes.Get(169), nvswtches.Get(21));
    NetDeviceContainer devs0_170 = link_helper0.Install(gpunodes.Get(170), nvswtches.Get(21));
    NetDeviceContainer devs0_171 = link_helper0.Install(gpunodes.Get(171), nvswtches.Get(21));
    NetDeviceContainer devs0_172 = link_helper0.Install(gpunodes.Get(172), nvswtches.Get(21));
    NetDeviceContainer devs0_173 = link_helper0.Install(gpunodes.Get(173), nvswtches.Get(21));
    NetDeviceContainer devs0_174 = link_helper0.Install(gpunodes.Get(174), nvswtches.Get(21));
    NetDeviceContainer devs0_175 = link_helper0.Install(gpunodes.Get(175), nvswtches.Get(21));
    NetDeviceContainer devs0_176 = link_helper0.Install(gpunodes.Get(176), nvswtches.Get(22));
    NetDeviceContainer devs0_177 = link_helper0.Install(gpunodes.Get(177), nvswtches.Get(22));
    NetDeviceContainer devs0_178 = link_helper0.Install(gpunodes.Get(178), nvswtches.Get(22));
    NetDeviceContainer devs0_179 = link_helper0.Install(gpunodes.Get(179), nvswtches.Get(22));
    NetDeviceContainer devs0_180 = link_helper0.Install(gpunodes.Get(180), nvswtches.Get(22));
    NetDeviceContainer devs0_181 = link_helper0.Install(gpunodes.Get(181), nvswtches.Get(22));
    NetDeviceContainer devs0_182 = link_helper0.Install(gpunodes.Get(182), nvswtches.Get(22));
    NetDeviceContainer devs0_183 = link_helper0.Install(gpunodes.Get(183), nvswtches.Get(22));
    NetDeviceContainer devs0_184 = link_helper0.Install(gpunodes.Get(184), nvswtches.Get(23));
    NetDeviceContainer devs0_185 = link_helper0.Install(gpunodes.Get(185), nvswtches.Get(23));
    NetDeviceContainer devs0_186 = link_helper0.Install(gpunodes.Get(186), nvswtches.Get(23));
    NetDeviceContainer devs0_187 = link_helper0.Install(gpunodes.Get(187), nvswtches.Get(23));
    NetDeviceContainer devs0_188 = link_helper0.Install(gpunodes.Get(188), nvswtches.Get(23));
    NetDeviceContainer devs0_189 = link_helper0.Install(gpunodes.Get(189), nvswtches.Get(23));
    NetDeviceContainer devs0_190 = link_helper0.Install(gpunodes.Get(190), nvswtches.Get(23));
    NetDeviceContainer devs0_191 = link_helper0.Install(gpunodes.Get(191), nvswtches.Get(23));
    NetDeviceContainer devs0_192 = link_helper0.Install(gpunodes.Get(192), nvswtches.Get(24));
    NetDeviceContainer devs0_193 = link_helper0.Install(gpunodes.Get(193), nvswtches.Get(24));
    NetDeviceContainer devs0_194 = link_helper0.Install(gpunodes.Get(194), nvswtches.Get(24));
    NetDeviceContainer devs0_195 = link_helper0.Install(gpunodes.Get(195), nvswtches.Get(24));
    NetDeviceContainer devs0_196 = link_helper0.Install(gpunodes.Get(196), nvswtches.Get(24));
    NetDeviceContainer devs0_197 = link_helper0.Install(gpunodes.Get(197), nvswtches.Get(24));
    NetDeviceContainer devs0_198 = link_helper0.Install(gpunodes.Get(198), nvswtches.Get(24));
    NetDeviceContainer devs0_199 = link_helper0.Install(gpunodes.Get(199), nvswtches.Get(24));
    NetDeviceContainer devs0_200 = link_helper0.Install(gpunodes.Get(200), nvswtches.Get(25));
    NetDeviceContainer devs0_201 = link_helper0.Install(gpunodes.Get(201), nvswtches.Get(25));
    NetDeviceContainer devs0_202 = link_helper0.Install(gpunodes.Get(202), nvswtches.Get(25));
    NetDeviceContainer devs0_203 = link_helper0.Install(gpunodes.Get(203), nvswtches.Get(25));
    NetDeviceContainer devs0_204 = link_helper0.Install(gpunodes.Get(204), nvswtches.Get(25));
    NetDeviceContainer devs0_205 = link_helper0.Install(gpunodes.Get(205), nvswtches.Get(25));
    NetDeviceContainer devs0_206 = link_helper0.Install(gpunodes.Get(206), nvswtches.Get(25));
    NetDeviceContainer devs0_207 = link_helper0.Install(gpunodes.Get(207), nvswtches.Get(25));
    NetDeviceContainer devs0_208 = link_helper0.Install(gpunodes.Get(208), nvswtches.Get(26));
    NetDeviceContainer devs0_209 = link_helper0.Install(gpunodes.Get(209), nvswtches.Get(26));
    NetDeviceContainer devs0_210 = link_helper0.Install(gpunodes.Get(210), nvswtches.Get(26));
    NetDeviceContainer devs0_211 = link_helper0.Install(gpunodes.Get(211), nvswtches.Get(26));
    NetDeviceContainer devs0_212 = link_helper0.Install(gpunodes.Get(212), nvswtches.Get(26));
    NetDeviceContainer devs0_213 = link_helper0.Install(gpunodes.Get(213), nvswtches.Get(26));
    NetDeviceContainer devs0_214 = link_helper0.Install(gpunodes.Get(214), nvswtches.Get(26));
    NetDeviceContainer devs0_215 = link_helper0.Install(gpunodes.Get(215), nvswtches.Get(26));
    NetDeviceContainer devs0_216 = link_helper0.Install(gpunodes.Get(216), nvswtches.Get(27));
    NetDeviceContainer devs0_217 = link_helper0.Install(gpunodes.Get(217), nvswtches.Get(27));
    NetDeviceContainer devs0_218 = link_helper0.Install(gpunodes.Get(218), nvswtches.Get(27));
    NetDeviceContainer devs0_219 = link_helper0.Install(gpunodes.Get(219), nvswtches.Get(27));
    NetDeviceContainer devs0_220 = link_helper0.Install(gpunodes.Get(220), nvswtches.Get(27));
    NetDeviceContainer devs0_221 = link_helper0.Install(gpunodes.Get(221), nvswtches.Get(27));
    NetDeviceContainer devs0_222 = link_helper0.Install(gpunodes.Get(222), nvswtches.Get(27));
    NetDeviceContainer devs0_223 = link_helper0.Install(gpunodes.Get(223), nvswtches.Get(27));
    NetDeviceContainer devs0_224 = link_helper0.Install(gpunodes.Get(224), nvswtches.Get(28));
    NetDeviceContainer devs0_225 = link_helper0.Install(gpunodes.Get(225), nvswtches.Get(28));
    NetDeviceContainer devs0_226 = link_helper0.Install(gpunodes.Get(226), nvswtches.Get(28));
    NetDeviceContainer devs0_227 = link_helper0.Install(gpunodes.Get(227), nvswtches.Get(28));
    NetDeviceContainer devs0_228 = link_helper0.Install(gpunodes.Get(228), nvswtches.Get(28));
    NetDeviceContainer devs0_229 = link_helper0.Install(gpunodes.Get(229), nvswtches.Get(28));
    NetDeviceContainer devs0_230 = link_helper0.Install(gpunodes.Get(230), nvswtches.Get(28));
    NetDeviceContainer devs0_231 = link_helper0.Install(gpunodes.Get(231), nvswtches.Get(28));
    NetDeviceContainer devs0_232 = link_helper0.Install(gpunodes.Get(232), nvswtches.Get(29));
    NetDeviceContainer devs0_233 = link_helper0.Install(gpunodes.Get(233), nvswtches.Get(29));
    NetDeviceContainer devs0_234 = link_helper0.Install(gpunodes.Get(234), nvswtches.Get(29));
    NetDeviceContainer devs0_235 = link_helper0.Install(gpunodes.Get(235), nvswtches.Get(29));
    NetDeviceContainer devs0_236 = link_helper0.Install(gpunodes.Get(236), nvswtches.Get(29));
    NetDeviceContainer devs0_237 = link_helper0.Install(gpunodes.Get(237), nvswtches.Get(29));
    NetDeviceContainer devs0_238 = link_helper0.Install(gpunodes.Get(238), nvswtches.Get(29));
    NetDeviceContainer devs0_239 = link_helper0.Install(gpunodes.Get(239), nvswtches.Get(29));
    NetDeviceContainer devs0_240 = link_helper0.Install(gpunodes.Get(240), nvswtches.Get(30));
    NetDeviceContainer devs0_241 = link_helper0.Install(gpunodes.Get(241), nvswtches.Get(30));
    NetDeviceContainer devs0_242 = link_helper0.Install(gpunodes.Get(242), nvswtches.Get(30));
    NetDeviceContainer devs0_243 = link_helper0.Install(gpunodes.Get(243), nvswtches.Get(30));
    NetDeviceContainer devs0_244 = link_helper0.Install(gpunodes.Get(244), nvswtches.Get(30));
    NetDeviceContainer devs0_245 = link_helper0.Install(gpunodes.Get(245), nvswtches.Get(30));
    NetDeviceContainer devs0_246 = link_helper0.Install(gpunodes.Get(246), nvswtches.Get(30));
    NetDeviceContainer devs0_247 = link_helper0.Install(gpunodes.Get(247), nvswtches.Get(30));
    NetDeviceContainer devs0_248 = link_helper0.Install(gpunodes.Get(248), nvswtches.Get(31));
    NetDeviceContainer devs0_249 = link_helper0.Install(gpunodes.Get(249), nvswtches.Get(31));
    NetDeviceContainer devs0_250 = link_helper0.Install(gpunodes.Get(250), nvswtches.Get(31));
    NetDeviceContainer devs0_251 = link_helper0.Install(gpunodes.Get(251), nvswtches.Get(31));
    NetDeviceContainer devs0_252 = link_helper0.Install(gpunodes.Get(252), nvswtches.Get(31));
    NetDeviceContainer devs0_253 = link_helper0.Install(gpunodes.Get(253), nvswtches.Get(31));
    NetDeviceContainer devs0_254 = link_helper0.Install(gpunodes.Get(254), nvswtches.Get(31));
    NetDeviceContainer devs0_255 = link_helper0.Install(gpunodes.Get(255), nvswtches.Get(31));
    NetDeviceContainer devs1_256 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(0));
    NetDeviceContainer devs1_257 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(8));
    NetDeviceContainer devs1_258 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(16));
    NetDeviceContainer devs1_259 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(24));
    NetDeviceContainer devs1_260 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(32));
    NetDeviceContainer devs1_261 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(40));
    NetDeviceContainer devs1_262 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(48));
    NetDeviceContainer devs1_263 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(56));
    NetDeviceContainer devs1_264 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(64));
    NetDeviceContainer devs1_265 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(72));
    NetDeviceContainer devs1_266 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(80));
    NetDeviceContainer devs1_267 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(88));
    NetDeviceContainer devs1_268 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(96));
    NetDeviceContainer devs1_269 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(104));
    NetDeviceContainer devs1_270 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(112));
    NetDeviceContainer devs1_271 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(120));
    NetDeviceContainer devs1_272 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(128));
    NetDeviceContainer devs1_273 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(136));
    NetDeviceContainer devs1_274 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(144));
    NetDeviceContainer devs1_275 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(152));
    NetDeviceContainer devs1_276 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(160));
    NetDeviceContainer devs1_277 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(168));
    NetDeviceContainer devs1_278 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(176));
    NetDeviceContainer devs1_279 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(184));
    NetDeviceContainer devs1_280 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(192));
    NetDeviceContainer devs1_281 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(200));
    NetDeviceContainer devs1_282 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(208));
    NetDeviceContainer devs1_283 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(216));
    NetDeviceContainer devs1_284 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(224));
    NetDeviceContainer devs1_285 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(232));
    NetDeviceContainer devs1_286 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(240));
    NetDeviceContainer devs1_287 = link_helper1.Install(regswtches.Get(0), gpunodes.Get(248));
    NetDeviceContainer devs1_288 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(1));
    NetDeviceContainer devs1_289 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(9));
    NetDeviceContainer devs1_290 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(17));
    NetDeviceContainer devs1_291 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(25));
    NetDeviceContainer devs1_292 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(33));
    NetDeviceContainer devs1_293 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(41));
    NetDeviceContainer devs1_294 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(49));
    NetDeviceContainer devs1_295 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(57));
    NetDeviceContainer devs1_296 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(65));
    NetDeviceContainer devs1_297 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(73));
    NetDeviceContainer devs1_298 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(81));
    NetDeviceContainer devs1_299 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(89));
    NetDeviceContainer devs1_300 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(97));
    NetDeviceContainer devs1_301 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(105));
    NetDeviceContainer devs1_302 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(113));
    NetDeviceContainer devs1_303 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(121));
    NetDeviceContainer devs1_304 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(129));
    NetDeviceContainer devs1_305 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(137));
    NetDeviceContainer devs1_306 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(145));
    NetDeviceContainer devs1_307 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(153));
    NetDeviceContainer devs1_308 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(161));
    NetDeviceContainer devs1_309 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(169));
    NetDeviceContainer devs1_310 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(177));
    NetDeviceContainer devs1_311 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(185));
    NetDeviceContainer devs1_312 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(193));
    NetDeviceContainer devs1_313 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(201));
    NetDeviceContainer devs1_314 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(209));
    NetDeviceContainer devs1_315 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(217));
    NetDeviceContainer devs1_316 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(225));
    NetDeviceContainer devs1_317 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(233));
    NetDeviceContainer devs1_318 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(241));
    NetDeviceContainer devs1_319 = link_helper1.Install(regswtches.Get(1), gpunodes.Get(249));
    NetDeviceContainer devs1_320 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(2));
    NetDeviceContainer devs1_321 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(10));
    NetDeviceContainer devs1_322 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(18));
    NetDeviceContainer devs1_323 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(26));
    NetDeviceContainer devs1_324 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(34));
    NetDeviceContainer devs1_325 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(42));
    NetDeviceContainer devs1_326 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(50));
    NetDeviceContainer devs1_327 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(58));
    NetDeviceContainer devs1_328 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(66));
    NetDeviceContainer devs1_329 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(74));
    NetDeviceContainer devs1_330 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(82));
    NetDeviceContainer devs1_331 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(90));
    NetDeviceContainer devs1_332 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(98));
    NetDeviceContainer devs1_333 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(106));
    NetDeviceContainer devs1_334 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(114));
    NetDeviceContainer devs1_335 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(122));
    NetDeviceContainer devs1_336 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(130));
    NetDeviceContainer devs1_337 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(138));
    NetDeviceContainer devs1_338 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(146));
    NetDeviceContainer devs1_339 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(154));
    NetDeviceContainer devs1_340 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(162));
    NetDeviceContainer devs1_341 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(170));
    NetDeviceContainer devs1_342 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(178));
    NetDeviceContainer devs1_343 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(186));
    NetDeviceContainer devs1_344 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(194));
    NetDeviceContainer devs1_345 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(202));
    NetDeviceContainer devs1_346 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(210));
    NetDeviceContainer devs1_347 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(218));
    NetDeviceContainer devs1_348 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(226));
    NetDeviceContainer devs1_349 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(234));
    NetDeviceContainer devs1_350 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(242));
    NetDeviceContainer devs1_351 = link_helper1.Install(regswtches.Get(2), gpunodes.Get(250));
    NetDeviceContainer devs1_352 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(3));
    NetDeviceContainer devs1_353 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(11));
    NetDeviceContainer devs1_354 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(19));
    NetDeviceContainer devs1_355 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(27));
    NetDeviceContainer devs1_356 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(35));
    NetDeviceContainer devs1_357 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(43));
    NetDeviceContainer devs1_358 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(51));
    NetDeviceContainer devs1_359 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(59));
    NetDeviceContainer devs1_360 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(67));
    NetDeviceContainer devs1_361 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(75));
    NetDeviceContainer devs1_362 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(83));
    NetDeviceContainer devs1_363 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(91));
    NetDeviceContainer devs1_364 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(99));
    NetDeviceContainer devs1_365 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(107));
    NetDeviceContainer devs1_366 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(115));
    NetDeviceContainer devs1_367 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(123));
    NetDeviceContainer devs1_368 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(131));
    NetDeviceContainer devs1_369 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(139));
    NetDeviceContainer devs1_370 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(147));
    NetDeviceContainer devs1_371 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(155));
    NetDeviceContainer devs1_372 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(163));
    NetDeviceContainer devs1_373 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(171));
    NetDeviceContainer devs1_374 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(179));
    NetDeviceContainer devs1_375 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(187));
    NetDeviceContainer devs1_376 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(195));
    NetDeviceContainer devs1_377 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(203));
    NetDeviceContainer devs1_378 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(211));
    NetDeviceContainer devs1_379 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(219));
    NetDeviceContainer devs1_380 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(227));
    NetDeviceContainer devs1_381 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(235));
    NetDeviceContainer devs1_382 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(243));
    NetDeviceContainer devs1_383 = link_helper1.Install(regswtches.Get(3), gpunodes.Get(251));
    NetDeviceContainer devs1_384 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(4));
    NetDeviceContainer devs1_385 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(12));
    NetDeviceContainer devs1_386 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(20));
    NetDeviceContainer devs1_387 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(28));
    NetDeviceContainer devs1_388 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(36));
    NetDeviceContainer devs1_389 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(44));
    NetDeviceContainer devs1_390 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(52));
    NetDeviceContainer devs1_391 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(60));
    NetDeviceContainer devs1_392 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(68));
    NetDeviceContainer devs1_393 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(76));
    NetDeviceContainer devs1_394 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(84));
    NetDeviceContainer devs1_395 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(92));
    NetDeviceContainer devs1_396 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(100));
    NetDeviceContainer devs1_397 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(108));
    NetDeviceContainer devs1_398 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(116));
    NetDeviceContainer devs1_399 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(124));
    NetDeviceContainer devs1_400 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(132));
    NetDeviceContainer devs1_401 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(140));
    NetDeviceContainer devs1_402 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(148));
    NetDeviceContainer devs1_403 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(156));
    NetDeviceContainer devs1_404 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(164));
    NetDeviceContainer devs1_405 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(172));
    NetDeviceContainer devs1_406 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(180));
    NetDeviceContainer devs1_407 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(188));
    NetDeviceContainer devs1_408 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(196));
    NetDeviceContainer devs1_409 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(204));
    NetDeviceContainer devs1_410 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(212));
    NetDeviceContainer devs1_411 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(220));
    NetDeviceContainer devs1_412 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(228));
    NetDeviceContainer devs1_413 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(236));
    NetDeviceContainer devs1_414 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(244));
    NetDeviceContainer devs1_415 = link_helper1.Install(regswtches.Get(4), gpunodes.Get(252));
    NetDeviceContainer devs1_416 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(5));
    NetDeviceContainer devs1_417 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(13));
    NetDeviceContainer devs1_418 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(21));
    NetDeviceContainer devs1_419 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(29));
    NetDeviceContainer devs1_420 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(37));
    NetDeviceContainer devs1_421 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(45));
    NetDeviceContainer devs1_422 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(53));
    NetDeviceContainer devs1_423 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(61));
    NetDeviceContainer devs1_424 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(69));
    NetDeviceContainer devs1_425 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(77));
    NetDeviceContainer devs1_426 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(85));
    NetDeviceContainer devs1_427 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(93));
    NetDeviceContainer devs1_428 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(101));
    NetDeviceContainer devs1_429 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(109));
    NetDeviceContainer devs1_430 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(117));
    NetDeviceContainer devs1_431 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(125));
    NetDeviceContainer devs1_432 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(133));
    NetDeviceContainer devs1_433 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(141));
    NetDeviceContainer devs1_434 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(149));
    NetDeviceContainer devs1_435 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(157));
    NetDeviceContainer devs1_436 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(165));
    NetDeviceContainer devs1_437 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(173));
    NetDeviceContainer devs1_438 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(181));
    NetDeviceContainer devs1_439 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(189));
    NetDeviceContainer devs1_440 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(197));
    NetDeviceContainer devs1_441 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(205));
    NetDeviceContainer devs1_442 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(213));
    NetDeviceContainer devs1_443 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(221));
    NetDeviceContainer devs1_444 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(229));
    NetDeviceContainer devs1_445 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(237));
    NetDeviceContainer devs1_446 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(245));
    NetDeviceContainer devs1_447 = link_helper1.Install(regswtches.Get(5), gpunodes.Get(253));
    NetDeviceContainer devs1_448 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(6));
    NetDeviceContainer devs1_449 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(14));
    NetDeviceContainer devs1_450 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(22));
    NetDeviceContainer devs1_451 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(30));
    NetDeviceContainer devs1_452 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(38));
    NetDeviceContainer devs1_453 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(46));
    NetDeviceContainer devs1_454 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(54));
    NetDeviceContainer devs1_455 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(62));
    NetDeviceContainer devs1_456 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(70));
    NetDeviceContainer devs1_457 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(78));
    NetDeviceContainer devs1_458 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(86));
    NetDeviceContainer devs1_459 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(94));
    NetDeviceContainer devs1_460 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(102));
    NetDeviceContainer devs1_461 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(110));
    NetDeviceContainer devs1_462 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(118));
    NetDeviceContainer devs1_463 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(126));
    NetDeviceContainer devs1_464 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(134));
    NetDeviceContainer devs1_465 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(142));
    NetDeviceContainer devs1_466 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(150));
    NetDeviceContainer devs1_467 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(158));
    NetDeviceContainer devs1_468 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(166));
    NetDeviceContainer devs1_469 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(174));
    NetDeviceContainer devs1_470 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(182));
    NetDeviceContainer devs1_471 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(190));
    NetDeviceContainer devs1_472 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(198));
    NetDeviceContainer devs1_473 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(206));
    NetDeviceContainer devs1_474 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(214));
    NetDeviceContainer devs1_475 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(222));
    NetDeviceContainer devs1_476 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(230));
    NetDeviceContainer devs1_477 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(238));
    NetDeviceContainer devs1_478 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(246));
    NetDeviceContainer devs1_479 = link_helper1.Install(regswtches.Get(6), gpunodes.Get(254));
    NetDeviceContainer devs1_480 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(7));
    NetDeviceContainer devs1_481 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(15));
    NetDeviceContainer devs1_482 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(23));
    NetDeviceContainer devs1_483 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(31));
    NetDeviceContainer devs1_484 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(39));
    NetDeviceContainer devs1_485 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(47));
    NetDeviceContainer devs1_486 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(55));
    NetDeviceContainer devs1_487 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(63));
    NetDeviceContainer devs1_488 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(71));
    NetDeviceContainer devs1_489 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(79));
    NetDeviceContainer devs1_490 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(87));
    NetDeviceContainer devs1_491 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(95));
    NetDeviceContainer devs1_492 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(103));
    NetDeviceContainer devs1_493 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(111));
    NetDeviceContainer devs1_494 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(119));
    NetDeviceContainer devs1_495 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(127));
    NetDeviceContainer devs1_496 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(135));
    NetDeviceContainer devs1_497 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(143));
    NetDeviceContainer devs1_498 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(151));
    NetDeviceContainer devs1_499 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(159));
    NetDeviceContainer devs1_500 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(167));
    NetDeviceContainer devs1_501 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(175));
    NetDeviceContainer devs1_502 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(183));
    NetDeviceContainer devs1_503 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(191));
    NetDeviceContainer devs1_504 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(199));
    NetDeviceContainer devs1_505 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(207));
    NetDeviceContainer devs1_506 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(215));
    NetDeviceContainer devs1_507 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(223));
    NetDeviceContainer devs1_508 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(231));
    NetDeviceContainer devs1_509 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(239));
    NetDeviceContainer devs1_510 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(247));
    NetDeviceContainer devs1_511 = link_helper1.Install(regswtches.Get(7), gpunodes.Get(255));
    NetDeviceContainer devs2_512 = link_helper2.Install(regswtches.Get(0), regswtches.Get(8));
    NetDeviceContainer devs2_513 = link_helper2.Install(regswtches.Get(0), regswtches.Get(9));
    NetDeviceContainer devs2_514 = link_helper2.Install(regswtches.Get(0), regswtches.Get(10));
    NetDeviceContainer devs2_515 = link_helper2.Install(regswtches.Get(0), regswtches.Get(11));
    NetDeviceContainer devs2_516 = link_helper2.Install(regswtches.Get(1), regswtches.Get(8));
    NetDeviceContainer devs2_517 = link_helper2.Install(regswtches.Get(1), regswtches.Get(9));
    NetDeviceContainer devs2_518 = link_helper2.Install(regswtches.Get(1), regswtches.Get(10));
    NetDeviceContainer devs2_519 = link_helper2.Install(regswtches.Get(1), regswtches.Get(11));
    NetDeviceContainer devs2_520 = link_helper2.Install(regswtches.Get(2), regswtches.Get(8));
    NetDeviceContainer devs2_521 = link_helper2.Install(regswtches.Get(2), regswtches.Get(9));
    NetDeviceContainer devs2_522 = link_helper2.Install(regswtches.Get(2), regswtches.Get(10));
    NetDeviceContainer devs2_523 = link_helper2.Install(regswtches.Get(2), regswtches.Get(11));
    NetDeviceContainer devs2_524 = link_helper2.Install(regswtches.Get(3), regswtches.Get(8));
    NetDeviceContainer devs2_525 = link_helper2.Install(regswtches.Get(3), regswtches.Get(9));
    NetDeviceContainer devs2_526 = link_helper2.Install(regswtches.Get(3), regswtches.Get(10));
    NetDeviceContainer devs2_527 = link_helper2.Install(regswtches.Get(3), regswtches.Get(11));
    NetDeviceContainer devs2_528 = link_helper2.Install(regswtches.Get(4), regswtches.Get(8));
    NetDeviceContainer devs2_529 = link_helper2.Install(regswtches.Get(4), regswtches.Get(9));
    NetDeviceContainer devs2_530 = link_helper2.Install(regswtches.Get(4), regswtches.Get(10));
    NetDeviceContainer devs2_531 = link_helper2.Install(regswtches.Get(4), regswtches.Get(11));
    NetDeviceContainer devs2_532 = link_helper2.Install(regswtches.Get(5), regswtches.Get(8));
    NetDeviceContainer devs2_533 = link_helper2.Install(regswtches.Get(5), regswtches.Get(9));
    NetDeviceContainer devs2_534 = link_helper2.Install(regswtches.Get(5), regswtches.Get(10));
    NetDeviceContainer devs2_535 = link_helper2.Install(regswtches.Get(5), regswtches.Get(11));
    NetDeviceContainer devs2_536 = link_helper2.Install(regswtches.Get(6), regswtches.Get(8));
    NetDeviceContainer devs2_537 = link_helper2.Install(regswtches.Get(6), regswtches.Get(9));
    NetDeviceContainer devs2_538 = link_helper2.Install(regswtches.Get(6), regswtches.Get(10));
    NetDeviceContainer devs2_539 = link_helper2.Install(regswtches.Get(6), regswtches.Get(11));
    NetDeviceContainer devs2_540 = link_helper2.Install(regswtches.Get(7), regswtches.Get(8));
    NetDeviceContainer devs2_541 = link_helper2.Install(regswtches.Get(7), regswtches.Get(9));
    NetDeviceContainer devs2_542 = link_helper2.Install(regswtches.Get(7), regswtches.Get(10));
    NetDeviceContainer devs2_543 = link_helper2.Install(regswtches.Get(7), regswtches.Get(11));
    Config::SetDefault("ns3::RdmaHw::CcMode", UintegerValue(12));
    Config::SetDefault("ns3::RdmaHw::L2AckInterval", UintegerValue(0));
    Config::SetDefault("ns3::RdmaHw::L2ChunkSize", UintegerValue(4000));
    Config::SetDefault("ns3::RdmaHw::Mtu", UintegerValue(4096));

    // ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----
    RdmaFabricHelper rdmaFabric;
    rdmaFabric.Build(gpunodes, regswtches, nvswtches);


    std::string XML_ALGO = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/xml_input/rail_alltoall_ring.xml");

    std::string SWITCH_JSON = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/json_input/rail_alltoall_ring.json");

    // All output files go to simulation/scratch/logs. FindSelfDirectory() resolves to
    // simulation/build/scratch, so "../../scratch/logs" hops back up to the source tree.
    const std::string LOG_DIR = ns3::SystemPath::Append(ns3::SystemPath::FindSelfDirectory(), "../../scratch/logs");
    ns3::SystemPath::MakeDirectories(LOG_DIR); // no-op if it already exists

    const std::string LOG_FILE = ns3::SystemPath::Append(LOG_DIR, "Alltoall_DSL_test.txt");

    constexpr DataType::Type dtype = DataType::INT32;
    const uint32_t INPUT_BYTES = inputBytes;
    bool CORRECTNESS_CHECK = true;
    bool FLOW_ID = true;

    AlgoTopology topo(gpunodes, regswtches);
    AlgoParseResult result = topo.ParseAlgoXml(XML_ALGO.c_str());
    // Fatal, not a log line: a failed parse leaves the topology empty and the failure would
    // otherwise surface far downstream (zero input chunks) with NS_LOG_ERROR off by default.
    if (result != AlgoParseResult::ALGO_PARSE_SUCCESS) NS_FATAL_ERROR("Encountered issue in parsing XML algorithm, error code " << result);
    if (FLOW_ID){
        AlgoParseResult switchResult = topo.ParseSwitchJson(SWITCH_JSON.c_str());
        // Fatal for the same reason the XML parse above is: ParseSwitchJson returns on the
        // first bad entry, leaving flow forwarding half-installed (CustomFlowForwarding on,
        // table mostly empty), and the run then silently falls back to ECMP everywhere.
        if (switchResult != AlgoParseResult::ALGO_PARSE_SUCCESS) NS_FATAL_ERROR("Encountered issue in parsing switch JSON, error code " << switchResult);
    }

    static std::ofstream logtxt;

    // log file
    logtxt.open(LOG_FILE);
    if (!logtxt.is_open()){
        NS_FATAL_ERROR("Failed to log file");
    }
    chmod(LOG_FILE.c_str(), 0666);

    // Chunk count and participant set come straight from the parsed algorithm, so ChunkSize
    // and the tester can never drift from the XML: for alltoall the per-rank input chunk count
    // equals nchunksperloop (16 in ..._more_epochs, 8 in the single-loop variant), and swapping
    // XMLs needs no source edit here.
    const int N_CHUNKS = topo.GetNInputChunks();
    const int N_NODES = (int) topo.GetActiveGpuIds().size();
    NS_ASSERT_MSG(N_CHUNKS > 0, "Parsed algorithm reports zero input chunks; check the XML.");
    const int CHUNK_SIZE = (INPUT_BYTES / N_CHUNKS) / DataType::GetSizeBytes(dtype);

    // install apps
    CollectivesApplicationHelper app_helper;
    app_helper.SetAttribute("DataType", EnumValue(dtype));
    app_helper.SetAttribute("ChunkSize", UintegerValue(CHUNK_SIZE));
    app_helper.SetAttribute("CorrectnessCheck", BooleanValue(CORRECTNESS_CHECK));
    ApplicationContainer apps = app_helper.Install<GPU>(topo);

    NS_LOG_INFO("Finished installing collective apps.");

    CollectiveTester tester(apps, false, logtxt);
    CollectiveLogMode logMode = CollectiveLogMode::MINIMAL;
    if (checkLog == "silent") logMode = CollectiveLogMode::SILENT;
    else if (checkLog == "verbose") logMode = CollectiveLogMode::VERBOSE;
    else if (checkLog != "minimal") NS_FATAL_ERROR("Unknown --checkLog value '" << checkLog << "' (expected silent|minimal|verbose).");
    tester.SetLogMode(logMode);
    tester.SetMaxMismatches(maxMismatches);
    if (CORRECTNESS_CHECK) {
        tester.SetupAlltoall(topo, CHUNK_SIZE * N_CHUNKS);
    }
    else{
        NS_LOG_UNCOND("Skipping correctness check.");
    }

    // ---- congestion monitoring: event-driven switch egress queue / drop / PFC traces ----
    // Connect to the QbbNetDevice trace sources on every switch egress port. These fire
    // synchronously from within packet events, so they add no simulator events and leave the
    // reported latency/bandwidth (Simulator::Now()) untouched. Each qlen row is emitted on an
    // actual enqueue/dequeue, giving an exact, unsampled occupancy trace.
    std::string qlenPath = ns3::SystemPath::Append(LOG_DIR, "switch_qlen_" + label + ".csv");
    std::string eventPath = ns3::SystemPath::Append(LOG_DIR, "switch_events_" + label + ".csv");
    FILE* qlenOut = fopen(qlenPath.c_str(), "w");
    FILE* eventOut = fopen(eventPath.c_str(), "w");
    if (!qlenOut || !eventOut) NS_FATAL_ERROR("Failed to open congestion-monitor output files.");
    fprintf(qlenOut, "time_ns,sw_id,port_id,q_id,qlen_bytes,op\n");
    fprintf(eventOut, "time_ns,node_id,port_id,q_id,bytes,op\n"); // node_id 0-3=GPU, 4-6=switch; drops (with size) and PFC pause/resume (bytes/q_id blank)

    for (uint32_t s = 0; s < regswtches.GetN(); ++s) {
        Ptr<Node> sw = regswtches.Get(s);
        uint32_t swId = sw->GetId();
        for (uint32_t d = 0; d < sw->GetNDevices(); ++d) {
            Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(sw->GetDevice(d));
            if (!dev) continue; // skip any non-Qbb (e.g. loopback) device
            uint32_t port = dev->GetIfIndex();
            dev->TraceConnectWithoutContext("QbbEnqueue", MakeBoundCallback(&OnSwitchEnqueue, qlenOut, swId, port));
            dev->TraceConnectWithoutContext("QbbDequeue", MakeBoundCallback(&OnSwitchDequeue, qlenOut, swId, port));
            dev->TraceConnectWithoutContext("QbbDrop",    MakeBoundCallback(&OnSwitchDrop, eventOut, swId, port));
            dev->TraceConnectWithoutContext("QbbPfc",     MakeBoundCallback(&OnSwitchPfc, eventOut, swId, port));
        }
    }

    // The QbbPfc trace fires on the device that RECEIVES a PAUSE, and a switch backpressures a
    // congested ingress link by pausing the sender on the far end -- which for edge-switch <-> GPU
    // links is a host NIC, not a switch. So also connect the drop/PFC traces on the GPU NICs;
    // otherwise switch->host backpressure (the common case here) is never recorded. In the events
    // file, node ids 0..3 are GPUs and 4..6 are switches. Queue-occupancy (enqueue/dequeue) stays
    // switch-only, since host egress is just the GPU injecting and isn't the congestion of interest.
    for (uint32_t g = 0; g < gpunodes.GetN(); ++g) {
        Ptr<Node> gpu = gpunodes.Get(g);
        uint32_t gpuId = gpu->GetId();
        for (uint32_t d = 0; d < gpu->GetNDevices(); ++d) {
            Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(gpu->GetDevice(d));
            if (!dev) continue;
            uint32_t port = dev->GetIfIndex();
            dev->TraceConnectWithoutContext("QbbDrop", MakeBoundCallback(&OnSwitchDrop, eventOut, gpuId, port));
            dev->TraceConnectWithoutContext("QbbPfc",  MakeBoundCallback(&OnSwitchPfc, eventOut, gpuId, port));
        }
    }

    Simulator::Run();
    fclose(qlenOut);
    fclose(eventOut);
    std::cout << "Switch queue trace: " << qlenPath << std::endl;
    std::cout << "Switch drop/PFC trace: " << eventPath << std::endl;
    Time simTime = Simulator::Now();
    std::cout << "Total simulated time: "
        << simTime.GetNanoSeconds() << " nanoseconds" << std::endl;

    // algorithm bandwidth: total data moved per rank / time
    std::cout << "Alltoall algorithm bandwidth: "
        << (double) INPUT_BYTES * N_NODES / simTime.GetSeconds() / 1e9 << " GB/s" << std::endl;
    if (CORRECTNESS_CHECK) {
        CollectiveTestResult alltoall_res = tester.VerifyAlltoall(topo, CHUNK_SIZE * N_CHUNKS);
        if (alltoall_res == CollectiveTestResult::TEST_OK) std::cout << "Alltoall verified." << std::endl;
        else std::cout << "Alltoall incorrect." << std::endl;
    }

    Simulator::Destroy();
    NS_LOG_UNCOND("Done simulation");
    return 0;

}
