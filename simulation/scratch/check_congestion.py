#!/usr/bin/env python3
"""Classify the traces dual_plane_hetero emits (PFC/drops, switch queues, per-NIC bandwidth).

usage:  python3 scratch/check_congestion.py <label> [<label2>]
e.g.    python3 scratch/check_congestion.py ecmp128 sched128

Node ids follow the scratch's creation order: GPUs 0-95, regular switches 96-105,
NVSwitches 106-113 (never traced). A GPU's device 0 is its NVLink NIC, devices 1
and 2 are its plane-A / plane-B fabric NICs -- so PFC on a GPU port 0 is NVLink
backpressure and says nothing about the fabric path.
"""
import csv, os, sys, collections

LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")

def classify(node, port):
    if node >= 96:  return "fabric switch"
    return "gpu NVLink NIC" if port == 0 else "gpu fabric NIC"

def events(label):
    path = os.path.join(LOG, f"switch_events_{label}.csv")
    kinds, drops = collections.Counter(), collections.Counter()
    for row in csv.DictReader(open(path)):
        node, port, op = int(row["node_id"]), int(row["port_id"]), row["op"]
        (drops if op == "drop" else kinds)[(classify(node, port), op)] += 1
    print(f"  events ({os.path.basename(path)}):")
    if not kinds and not drops:
        print("    none")
    for (where, op), n in sorted(kinds.items()) + sorted(drops.items()):
        print(f"    {where:16s} {op:8s} {n}")

def qlen(label):
    """Peak egress queue per port, split by link class -- imbalance shows up here."""
    path = os.path.join(LOG, f"switch_qlen_{label}.csv")
    peak = collections.Counter()
    if os.path.exists(path):
        for row in csv.DictReader(open(path)):
            k = (int(row["sw_id"]), int(row["port_id"]))
            v = int(row["qlen_bytes"])
            if v > peak[k]: peak[k] = v
    if not peak:
        # --qlenRows=0 (what a large sweep must use): the per-packet rows were never written,
        # but the scratch always dumps the per-port high-water marks, which is exactly what
        # this function reduces the rows to anyway.
        path = os.path.join(LOG, f"switch_qlen_max_{label}.csv")
        if not os.path.exists(path):
            print(f"  peak egress queue: no switch_qlen_{label}.csv or switch_qlen_max_{label}.csv")
            return
        for row in csv.DictReader(open(path)):
            k = (int(row["sw_id"]), int(row["port_id"]))
            v = int(row["max_qlen_bytes"])
            if v > peak[k]: peak[k] = v
    if not peak:
        print("  peak egress queue: no samples")
        return
    groups = collections.defaultdict(list)
    for (sw, port), v in peak.items():
        leaf = sw < 102                     # node ids 96..101 are the 6 leaves
        groups["leaf uplink" if leaf and port >= 32 else
               "leaf->gpu"   if leaf else "spine downlink"].append(v)
    print(f"  peak egress queue ({os.path.basename(path)}):")
    for k, v in sorted(groups.items()):
        v.sort()
        mean = sum(v) / len(v)
        print(f"    {k:16s} n={len(v):4d}  max {max(v)/1e3:8.1f} KB  mean {mean/1e3:7.1f}  "
              f"p50 {v[len(v)//2]/1e3:7.1f}  max/mean {max(v)/mean if mean else 0:5.2f}")

def nicbw(label):
    """Per-NIC utilization over time, from --nicBwInterval.

    The question this answers: does a NIC-selection policy leave one of a GPU's two fabric
    NICs idle while its twin is busy? Aggregate byte counts cannot tell -- both policies move
    the same totals -- so we look at each sample instant and ask how lopsided that GPU's two
    fabric NICs were right then.
    """
    path = os.path.join(LOG, f"host_nic_bw_{label}.csv")
    if not os.path.exists(path):
        print(f"  per-NIC bandwidth: {os.path.basename(path)} not found "
              f"(re-run with --nicBwInterval=<ns>)")
        return
    # (time, node) -> {port: gbps} for fabric ports only
    per = collections.defaultdict(dict)
    nvl = collections.defaultdict(float)
    for row in csv.DictReader(open(path)):
        t, node, gbps = int(row["time_ns"]), int(row["node_id"]), float(row["gbps"])
        if row["kind"] == "fabric":
            per[(t, node)][int(row["port_id"])] = gbps
        else:
            nvl[t] += gbps

    idle_paired, both_busy, tot_samples = 0, 0, 0
    wasted = 0.0        # Gb of capacity lost to one NIC idling while its twin was loaded
    busy_gbps, sum_gbps = 0.0, 0.0
    for (t, node), pv in per.items():
        if len(pv) != 2:
            continue
        a, b = sorted(pv.values())          # a = quieter NIC, b = busier
        tot_samples += 1
        sum_gbps += a + b
        if b < 1.0:
            continue                        # GPU idle at this instant; not a balance question
        busy_gbps += a + b
        if a < 0.05 * b:
            idle_paired += 1                # one NIC essentially idle while the other worked
            wasted += b - a
        else:
            both_busy += 1
    if not tot_samples:
        print("  per-NIC bandwidth: no paired fabric samples")
        return
    active = idle_paired + both_busy
    print(f"  per-NIC utilization ({os.path.basename(path)}):")
    print(f"    paired fabric samples          {tot_samples}")
    print(f"    ... GPU transmitting           {active} ({100*active/tot_samples:.1f}%)")
    if active:
        print(f"    ... one NIC idle, twin busy    {idle_paired} ({100*idle_paired/active:.1f}% of active)")
        print(f"    ... both NICs carrying traffic {both_busy} ({100*both_busy/active:.1f}% of active)")
        print(f"    mean per-NIC load while active {busy_gbps/(2*active):.1f} Gbps of 400")
        print(f"    imbalance headroom lost        {100*wasted/busy_gbps:.1f}% "
              f"(quieter NIC's shortfall vs its twin, over active samples)")
    if nvl:
        peak = max(nvl.values())
        print(f"    NVLink aggregate peak          {peak:.0f} Gbps across all GPUs")

for label in sys.argv[1:] or ["dual_plane_clustered"]:
    print(f"== {label} ==")
    events(label)
    qlen(label)
    nicbw(label)
