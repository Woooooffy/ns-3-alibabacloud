#!/usr/bin/env python3
"""Generate a SUBSET alltoall MSCCL schedule for the mini_* controlled set, and predict
the ECMP uplink split it will produce.

Participation is a property of the SCHEDULE, not the fabric: AlgoTopology calls a GPU
"active" exactly when its <gpu> element carries at least one threadblock
(algo_topology.cc, `if (mscclAlgo->nBlocks > 0) m_activeGpuIds.push_back(gpuId)`), and
CollectivesApplicationHelper::Install(AlgoTopology&) installs apps on that set alone.
A GPU left out of the XML therefore has no application, no qp and no traffic -- it is a
passive node of the fabric. Nothing in the .topo DSL or the generated .cc needs to change.

WHY SUBSETS: on a leaf with two equal-cost uplinks, a pure alltoall over participant set
S puts K = |S n leaf0| * |S n leaf1| flows into each leaf's uplink group (one qp per
(channel,peer), so with nchannels=1 one chunk per peer == one flow per ordered pair).
K odd forces a lopsided hash -- ECMP's best case is ceil(K/2):floor(K/2) -- so the fabric
cannot use both spines evenly no matter how good the hash is. That is the baseline a
chunk-splitting scheme is meant to beat.

The predicted split below replicates SwitchNode::EcmpHash exactly: seed = leaf node id,
key = (sip, dip, sport | dport<<16) with sip/dip = 10.0.0.(gpuIdx+1), dport = 21000, and
sport = 20000 + the peer's index among this node's send peers in ascending peer order
(collectives.cc AllocateRdmaSport, fed from sendPeerInfo in tb-parse order -- which is
why this generator emits send tbs sorted by peer id, and why reordering them would
change the prediction).
"""
import argparse, os, sys

# ---- topologies: gpu -> leaf, and each leaf switch's ns-3 node id (== its ecmp seed) ----
TOPOS = {
    # stem          ngpus  leaf0 gpus     leaf1 gpus     leaf0 id  leaf1 id
    "mini_1g1n": dict(ngpus=4, leaf0=[0, 1],       leaf1=[2, 3],       ids=(4, 5)),
    "mini_2g1n": dict(ngpus=8, leaf0=[0, 1, 2, 3], leaf1=[4, 5, 6, 7], ids=(8, 9)),
    "mini_2g2n": dict(ngpus=8, leaf0=[0, 1, 2, 3], leaf1=[4, 5, 6, 7], ids=(8, 9)),
}

# the four scenarios from the subset design; see the table in the header of each file
SCENARIOS = {
    "1A": ("mini_1g1n", [0, 2],                "K=1 per direction: ECMP physically cannot use the 2nd spine"),
    "2B": ("mini_2g1n", [0, 2, 3, 5],          "K=3 (3x1): hash lands 0:3 / 3:0, beating the shared-NIC bound"),
    "2C": ("mini_2g1n", [0, 2, 3, 4, 5, 6],    "K=9 (3x3): hash lands 8:1 on leaf1"),
    "3A": ("mini_2g2n", [0, 2, 3, 4, 5, 6],    "K=9 (3x3): same subset, per-GPU NICs"),
}


# ---------------------------------------------------------------- ecmp prediction ----
def ecmp_hash(key: bytes, seed: int) -> int:
    M = 0xFFFFFFFF
    h = seed & M
    for i in range(len(key) // 4):
        k = int.from_bytes(key[4 * i:4 * i + 4], "little")
        k = (k * 0xCC9E2D51) & M
        k = ((k << 15) | (k >> 17)) & M
        k = (k * 0x1B873593) & M
        h ^= k
        h = ((h << 13) | (h >> 19)) & M
        h = (h + ((h << 2) & M) + 0xE6546B64) & M
    h ^= len(key)
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & M
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & M
    h ^= h >> 16
    return h


def spine_of(src: int, dst: int, sport: int, seed: int) -> int:
    """Which of the leaf's two equal-cost uplinks this qp hashes onto.

    nextHops is built in device order by RdmaFabricHelper, and every mini leaf is wired
    downlinks-first then spine0 then spine1, so index 0 == spine0 and 1 == spine1.
    """
    key = (((10 << 24) | (src + 1)).to_bytes(4, "little")
           + ((10 << 24) | (dst + 1)).to_bytes(4, "little")
           + (sport | (21000 << 16)).to_bytes(4, "little"))
    return ecmp_hash(key, seed) % 2


def sport_of(subset, src, dst):
    return 20000 + sorted(p for p in subset if p != src).index(dst)


def predict(topo, subset):
    t = TOPOS[topo]
    out = []
    for near, far, seed in ((t["leaf0"], t["leaf1"], t["ids"][0]),
                            (t["leaf1"], t["leaf0"], t["ids"][1])):
        counts, rows = [0, 0], []
        for s in sorted(g for g in subset if g in near):
            for d in sorted(g for g in subset if g in far):
                sp = spine_of(s, d, sport_of(subset, s, d), seed)
                counts[sp] += 1
                rows.append((s, d, sport_of(subset, s, d), sp))
        out.append((seed, counts, rows))
    return out


# ------------------------------------------------------------------- xml emission ----
def emit(topo, subset, tag, note):
    """One send tb and one recv tb per peer, single channel, one chunk per destination.

    Offsets are in PARTICIPANT-RANK space, which is what CollectiveTester assumes: its
    m_participants[] is the ascending active-gpu list, so rank r's own partition sits at
    output offset r and the slice it sends to rank q is read from input offset q.
    """
    t = TOPOS[topo]
    S = sorted(subset)
    P = len(S)
    rank = {g: r for r, g in enumerate(S)}

    L = []
    members = ", ".join("g%d" % g for g in S)
    L.append(f'<!-- {topo} subset alltoall, scenario {tag}: participants {{{members}}} -->')
    L.append(f'<!-- {note} -->')
    L.append('<!-- GENERATED by gen_subset_alltoall.py. Do not hand-edit: reordering the')
    L.append('     send tbs changes sport allocation and therefore the ECMP split. -->')
    for seed, counts, _ in predict(topo, subset):
        L.append(f'<!-- predicted uplink split, leaf node {seed}: '
                 f'spine0={counts[0]} spine1={counts[1]} -->')
    L.append(f'<algo name="subset_a2a_{tag}" nchannels="1" nchunksperloop="{P}" proto="Simple"'
             f' maxBytes="9223372036854775807" minBytes="0" coll="alltoall" inplace="0"'
             f' outofplace="1" redop="nop" ngpus="{t["ngpus"]}">')
    for g in S:
        r = rank[g]
        peers = [p for p in S if p != g]
        L.append(f'  <gpu id="{g}" i_chunks="{P}" o_chunks="{P}" s_chunks="0">')
        L.append(f'    <copy i_off="{r}" o_off="{r}"/>')
        tb = 0
        for p in peers:  # recv tbs first, ascending peer -- mirrors the hand-written a2a XMLs
            L.append(f'    <tb id="{tb}" send="-1" recv="{p}" chan="0">')
            L.append(f'      <step s="0" type="r" srcbuf="i" srcoff="{r}" dstbuf="o"'
                     f' dstoff="{rank[p]}" cnt="1" depid="-1" deps="-1" hasdep="0"/>')
            L.append('    </tb>')
            tb += 1
        for p in peers:  # send tbs, ascending peer -> sport 20000, 20001, ... in this order
            L.append(f'    <tb id="{tb}" send="{p}" recv="-1" chan="0">')
            L.append(f'      <step s="0" type="s" srcbuf="i" srcoff="{rank[p]}" dstbuf="o"'
                     f' dstoff="{r}" cnt="1" depid="-1" deps="-1" hasdep="0"/>')
            L.append('    </tb>')
            tb += 1
        L.append('  </gpu>')
    L.append('</algo>')
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", nargs="?", choices=sorted(SCENARIOS) + ["all"], default="all")
    ap.add_argument("--dir", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    todo = sorted(SCENARIOS) if args.scenario == "all" else [args.scenario]
    for tag in todo:
        topo, subset, note = SCENARIOS[tag]
        path = os.path.join(args.dir, f"{topo}_sub{tag}.xml")
        text = emit(topo, subset, tag, note)
        if not args.dry_run:
            with open(path, "w") as f:
                f.write(text)
        t = TOPOS[topo]
        L0 = [g for g in subset if g in t["leaf0"]]
        L1 = [g for g in subset if g in t["leaf1"]]
        print(f"{tag}  {topo}  {{{', '.join('g%d' % g for g in subset)}}}"
              f"   {len(L0)}x{len(L1)} = K={len(L0)*len(L1)} flows/direction")
        for seed, counts, rows in predict(topo, subset):
            worst = max(counts)
            K = sum(counts)
            floor = ((K + 1) // 2) / (K / 2)   # what parity alone forces, for ANY seed
            print(f"      leaf node {seed}: spine0={counts[0]:2d}  spine1={counts[1]:2d}"
                  f"  -> busiest uplink carries {worst/(K/2):.2f}x the even share"
                  f"  (parity floor for K={K} is {floor:.2f}x at {(K+1)//2}:{K//2})")
        print(f"      -> {os.path.relpath(path, os.getcwd())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
