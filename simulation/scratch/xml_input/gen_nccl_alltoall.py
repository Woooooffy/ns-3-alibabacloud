#!/usr/bin/env python3
"""
Generate MSCCL-XML transcriptions of NCCL's ncclAlltoAll schedule for the three
ns-3 examples (dual_plane_hetero, rail_optimized_256gpu[_dual_plane]).

NCCL 2.31.2-1 (7b83616d). Everything below is a direct transcription of:
  src/init.cc:940            ncclP2pSchedule            -> the round permutation
  src/include/channel.h:21   ncclP2pChannelBaseForRound -> round -> channel
  src/include/bitops.h:443   reverseBits
  src/enqueue/enqueue.cc:3320 ncclAlltoAll fan-out      -> 2*nRanks p2p tasks
  src/enqueue/enqueue.cc:152  ncclAddWorkBatchToPlan    -> batching rules
  src/device/sendrecv.h:105   warp allocation           -> intra-batch concurrency

How the NCCL schedule maps onto what this simulator's XML parser/runtime mean
(see the header comment written into each XML for the full argument):

  NCCL concept                       MSCCL XML / ns-3 runtime
  ---------------------------------  --------------------------------------------
  one p2p work (send OR recv to peer) one <tb> with a single <step>
  send and recv run on separate warps two <tb>s per round, never one with 2 steps
                                      (steps inside a <tb> are strictly serial)
  channel = one CUDA block            chan= attribute; one qp per (chan, peer)
  works within a batch: concurrent    sibling <tb>s, no dependences
  batches on a channel: sequential    netdepid/netdeps chain (wire completion)
  self pair -> device-side copy       type="cpy" tb, send=-1 recv=-1
  p2pChunkSize / NCCL_STEPS ring      NOT in the XML: --protoChunkBytes / --maxMsgsInFlight
  no rate model at all                no rate= attribute anywhere
"""

import os

# ---------------------------------------------------------------- NCCL kernels

def pow2_up(n):
    p = 1
    while p < n:
        p <<= 1
    return p

def log2_up(n):
    b = 0
    while (1 << b) < n:
        b += 1
    return b

def reverse_bits(x, n_bits):
    """bitops.h:443 -- reverse the bottom nBits of x, zero the top bits."""
    if n_bits == 0:
        return 0
    r = 0
    for i in range(n_bits):
        r = (r << 1) | ((x >> i) & 1)
    return r

def gcd(a, b):
    while b:
        a, b = b, a % b
    return a

NCCL_MAX_DEV_WORK_P2P_PER_BATCH = 8   # device.h:392
NCCL_P2P_SCHEDULE_GROUP_SIZE = NCCL_MAX_DEV_WORK_P2P_PER_BATCH  # init.cc:938


class Comm:
    """The parts of ncclComm the alltoall schedule is a pure function of."""

    def __init__(self, node_sizes, p2p_n_channels, gpu_ids=None, ngpus=None):
        self.node_sizes = list(node_sizes)
        self.n_nodes = len(node_sizes)
        self.n_ranks = sum(node_sizes)
        self.max_local_ranks = max(node_sizes)
        # A communicator need not span the whole machine. NCCL numbers the ranks of a
        # communicator 0..n_ranks-1 and knows nothing about the GPUs left out of it, which is
        # exactly the partial-participation case: gpu_ids[rank] is the ns-3 node the rank
        # lives on, and ngpus is the topology's GPU count (what <algo ngpus> must declare, or
        # ParseAlgoXml rejects the file). Buffer offsets stay in RANK space, because that is
        # what CollectiveTester numbers its participants by.
        self.gpu_ids = list(gpu_ids) if gpu_ids is not None else list(range(self.n_ranks))
        assert len(self.gpu_ids) == self.n_ranks, "gpu_ids must name every rank"
        assert self.gpu_ids == sorted(self.gpu_ids), (
            "gpu_ids must ascend: the tester assigns participant ranks in ascending "
            "active-gpu-id order, so rank r must be the r-th smallest participant")
        self.ngpus = ngpus if ngpus is not None else self.n_ranks

        # rank layout: nodes are contiguous blocks of ranks, matching how every one
        # of the three .cc topologies wires gpunodes.Get(i) to its NVSwitch.
        self.local_rank_to_rank = []
        self.rank_to_node = [0] * self.n_ranks
        self.rank_to_local = [0] * self.n_ranks
        r = 0
        for n, sz in enumerate(node_sizes):
            self.local_rank_to_rank.append(list(range(r, r + sz)))
            for l in range(sz):
                self.rank_to_node[r + l] = n
                self.rank_to_local[r + l] = l
            r += sz

        # --- init.cc:943-950 : p2pSchedGroupSize
        gs = NCCL_P2P_SCHEDULE_GROUP_SIZE if self.n_nodes > 1 else self.max_local_ranks
        for sz in node_sizes:
            if sz % gs != 0 or sz < gs:
                gs = gcd(gs, sz)
        self.group_size = gs
        self.n_groups = self.n_ranks // gs
        self.n_groups_pow2 = pow2_up(self.n_groups)

        # --- init.cc:966-972 : group -> (node, local offset)
        self.group_to_local = []
        self.group_to_node = []
        for n, sz in enumerate(node_sizes):
            assert sz % gs == 0, "ncclInternalError: localRanks %% groupSize != 0"
            for g in range(sz // gs):
                self.group_to_local.append(g * gs)
                self.group_to_node.append(n)
        assert len(self.group_to_node) == self.n_groups

        # --- paths.cc:1008 : p2pnChannels / p2pnChannelsPerPeer
        self.p2p_n_channels = pow2_up(p2p_n_channels)
        pcp = 1  # pow2Up of ncclTopoGetNchannels; see the note in main()
        if self.n_nodes > 1:
            while (pcp * -(-self.n_ranks // NCCL_MAX_DEV_WORK_P2P_PER_BATCH)
                   > self.p2p_n_channels and pcp > 1):
                pcp //= 2
        else:
            pcp = min(self.p2p_n_channels, pcp)
        self.p2p_n_channels_per_peer = pcp

    def p2p_schedule(self, rank):
        """init.cc:980-1005 -- [(sendRank, recvRank)] indexed by round, for `rank`."""
        gs = self.group_size
        n_groups = self.n_groups
        local = self.rank_to_local[rank] % gs
        group = self.rank_to_local[rank] // gs
        for n in range(self.rank_to_node[rank]):
            group += self.node_sizes[n] // gs

        sched = []
        group_round, group_delta = 0, 0
        while True:
            if group_delta < n_groups:
                send_group = (group + group_delta) % n_groups
                recv_group = (group - group_delta + n_groups) % n_groups
                send_node = self.group_to_node[send_group]
                recv_node = self.group_to_node[recv_group]
                for delta in range(gs):
                    send_local = self.group_to_local[send_group] + (local + delta) % gs
                    recv_local = self.group_to_local[recv_group] + (local - delta + gs) % gs
                    sched.append((self.local_rank_to_rank[send_node][send_local],
                                  self.local_rank_to_rank[recv_node][recv_local]))
            group_round += 1
            group_delta = (group_delta + group_round) & (self.n_groups_pow2 - 1)
            if group_round == self.n_groups_pow2:
                break
        assert len(sched) == self.n_ranks, "P2p schedule creation has bugs."
        return sched

    def channel_base_for_round(self, p2p_round):
        """channel.h:21."""
        if self.n_nodes > 1:
            local_size = self.group_size
            group_delta = p2p_round // local_size
            local_delta = p2p_round % local_size
            base = group_delta * -(-local_size // NCCL_MAX_DEV_WORK_P2P_PER_BATCH)
            base += local_delta // NCCL_MAX_DEV_WORK_P2P_PER_BATCH
        else:
            base = p2p_round
        return reverse_bits(base, log2_up(self.p2p_n_channels))


# ------------------------------------------------------------------ validation

def validate(comm):
    for rank in range(comm.n_ranks):
        sched = comm.p2p_schedule(rank)
        sends = sorted(s for s, _ in sched)
        recvs = sorted(r for _, r in sched)
        assert sends == list(range(comm.n_ranks)), f"rank {rank}: sends not a permutation"
        assert recvs == list(range(comm.n_ranks)), f"rank {rank}: recvs not a permutation"
        assert sched[0] == (rank, rank), f"rank {rank}: round 0 is not the self pair"
    # every rank must agree pairwise: if I send to p on round r, p receives from me
    # on the round it assigns to me -- and both must land on the same channel, else
    # the two ends' qps would not line up.
    for rank in range(comm.n_ranks):
        sched = comm.p2p_schedule(rank)
        for r, (s, _) in enumerate(sched):
            peer_sched = comm.p2p_schedule(s)
            pr = [i for i, (_, rr) in enumerate(peer_sched) if rr == rank]
            assert len(pr) == 1
            assert comm.channel_base_for_round(r) == comm.channel_base_for_round(pr[0]), (
                f"channel mismatch: {rank}->{s} on chan {comm.channel_base_for_round(r)} "
                f"but {s} recvs on chan {comm.channel_base_for_round(pr[0])}")
    # channel mapping inverse (spec §11.2)
    n = comm.p2p_n_channels
    for base in range(n):
        for part in range(comm.p2p_n_channels_per_peer):
            chan = (base + part) & (n - 1)
            assert ((chan - base) & (n - 1)) == part


# -------------------------------------------------------------------- emission

HEADER = """<!--
  NCCL ncclAlltoAll, transcribed as an MSCCL schedule.  GENERATED by
  simulation/scratch/xml_input/gen_nccl_alltoall.py ; do not hand edit.
  Full rationale, deviations and how to run: README_nccl_alltoall.md, alongside.

  Source of truth: NCCL 2.31.2-1 (7b83616d).  ncclAlltoAll is not an algorithm.
  It expands to 2*nRanks p2p tasks (enqueue.cc:3320); a fixed permutation
  (ncclP2pSchedule, init.cc:940) maps them onto rounds, and a bit reversal
  (ncclP2pChannelBaseForRound, channel.h:21) maps rounds onto channels.  There is
  no cost model, no algorithm selection and no protocol tuning in that path.

  Topology modelled: {topo}
    {n_ranks} ranks, {n_nodes} NVLink domains of sizes {sizes}
    p2pSchedGroupSize   = {gs}   (init.cc:943, gcd path)
    nGroups             = {ngroups}, nGroupsPow2 = {ngroups2}
    p2pnChannels        = {nchan}
    p2pnChannelsPerPeer = {pcp}   (multi node reduction, paths.cc:1008)
    max batches on one channel = {maxbatch}

  Execution model, as expressed here:

  * One <tb> per p2p work, not one per round.  A round is a (sendRank, recvRank)
    pair whose send and recv run on disjoint warp ranges of the same block
    (sendrecv.h:105).  Steps inside one <tb> are strictly serial in this runtime,
    so each round emits a send only <tb> (recv="-1") and a recv only <tb>
    (send="-1").  Fusing them into one <tb> would falsely serialise them.

  * chan= is ncclP2pChannelBaseForRound(round).  p2pnChannelsPerPeer is {pcp}, so
    channelBase IS the channel and the 4 KiB part split of device.h:261 is inert.
    The runtime keys one persistent qp per (channel, peer), which is exactly
    NCCL's per channel connection.

  * Works in a batch are concurrent; batches on a channel are sequential.  Because
    p2pSchedGroupSize == NCCL_MAX_DEV_WORK_P2P_PER_BATCH == 8, a batch is exactly
    one group of 8 consecutive rounds (enqueue.cc:152, rules 4 and 6).  Batch
    order is carried by netdepid/netdeps, the wire dependence, chained per batch
    slot (round % 8).  depid/deps would impose nothing here: a send step
    "completes" for depid purposes the instant it is posted, long before its
    bytes are gone.

  * Round 0 is always the self pair (groupDelta 0, delta 0).  AllToAll is out of
    place, so the self elision in enqueue.cc does not fire and NCCL keeps a real
    work item, run as a device side copy (sendrecv.h, isCopy).  That is the
    type="cpy" <tb>.

  * No rate= attribute anywhere: NCCL has no rate model in the p2p path.

  * The chunk pipeline (p2pChunkSize through an NCCL_STEPS deep buffer) is a
    transport property, not a schedule, so it is not in this file.  Supply it on
    the command line instead; the README gives the exact flags.

  Emitted per rank: {n_ranks} rounds, {n_send} send tbs, {n_recv} recv tbs, 1 copy tb.
-->
"""


def emit(comm, topo_name, algo_name, out_path):
    n = comm.n_ranks
    lines = []
    max_batches_per_chan = max(
        sum(1 for k in range(comm.n_groups) if reverse_bits(k, log2_up(comm.p2p_n_channels)) == c)
        for c in range(comm.p2p_n_channels))
    lines.append(HEADER.format(
        topo=topo_name, n_ranks=n, n_nodes=comm.n_nodes,
        sizes=comm.node_sizes, gs=comm.group_size, ngroups=comm.n_groups,
        ngroups2=comm.n_groups_pow2, nchan=comm.p2p_n_channels,
        pcp=comm.p2p_n_channels_per_peer, maxbatch=max_batches_per_chan,
        n_send=n - 1, n_recv=n - 1))
    lines.append(
        f'<algo name="{algo_name}" nchannels="{comm.p2p_n_channels}" nchunksperloop="{n}" '
        f'proto="Simple" maxBytes="9223372036854775807" minBytes="0" coll="alltoall" '
        f'inplace="0" outofplace="1" redop="nop" ngpus="{comm.ngpus}">')

    total_tbs = 0
    blocks = []          # (gpu id, lines) -- emitted in gpu-id order at the end
    for rank in range(n):
        sched = comm.p2p_schedule(rank)
        # tb id assignment: rounds in schedule order, send tb then recv tb, so a
        # reader can map tb id back to round trivially.  bid must be dense (the
        # parser rejects gaps), so ids are handed out as the tbs are built.
        send_tb_of_round = {}
        tbs = []  # (bid, send, recv, chan, steps[])
        for r, (s_rank, r_rank) in enumerate(sched):
            chan = comm.channel_base_for_round(r)
            if s_rank == rank:
                # the self work: a device-side copy, no connection, no channel peer.
                # recvRank is also `rank` on this round (round 0), so this one tb
                # covers both halves of the self pair.
                assert r_rank == rank and r == 0
                bid = len(tbs)
                tbs.append((bid, -1, -1, chan,
                            [dict(type="cpy", srcbuf="i", srcoff=rank, dstbuf="o",
                                  dstoff=rank, cnt=1, comment=f"round {r}: self pair")]))
                continue
            bid = len(tbs)
            send_tb_of_round[r] = bid
            # send=/recv= name ns-3 NODE ids (the runtime resolves a peer through NodeList),
            # while srcoff/dstoff are RANK indices into the buffers. On a whole-machine
            # communicator the two coincide; on a subset they do not, and mixing them up
            # would send the right bytes to the wrong GPU.
            tbs.append((bid, comm.gpu_ids[s_rank], -1, chan,
                        [dict(type="s", srcbuf="i", srcoff=s_rank, dstbuf="o", dstoff=rank,
                              cnt=1, comment=f"round {r} send")]))
            bid = len(tbs)
            tbs.append((bid, -1, comm.gpu_ids[r_rank], chan,
                        [dict(type="r", srcbuf="i", srcoff=rank, dstbuf="o", dstoff=r_rank,
                              cnt=1, comment=f"round {r} recv")]))

        # batch-ordering chain: within a channel, for each batch slot j, link the
        # send tb of batch k+1 to the send tb of batch k.  Skips the self round,
        # whose tb has no send step for a netdep to name (ValidateNetDeps rejects
        # a netdep on a non-sending step).
        gs = comm.group_size
        chains = {}  # (chan, slot) -> previous send bid
        for r in range(n):
            if r not in send_tb_of_round:
                continue
            chan = comm.channel_base_for_round(r)
            slot = r % gs
            key = (chan, slot)
            prev = chains.get(key)
            if prev is not None:
                st = tbs[send_tb_of_round[r]][4][0]
                st["netdepid"] = prev
                st["netdeps"] = 0
                st["comment"] += f" (waits on tb {prev} draining: previous batch on chan {chan})"
            chains[key] = send_tb_of_round[r]

        blk = [f'  <gpu id="{comm.gpu_ids[rank]}" i_chunks="{n}" o_chunks="{n}" s_chunks="0">']
        for bid, sp, rp, chan, steps in tbs:
            blk.append(f'    <tb id="{bid}" send="{sp}" recv="{rp}" chan="{chan}">')
            for sid, st in enumerate(steps):
                attrs = (f's="{sid}" type="{st["type"]}" srcbuf="{st["srcbuf"]}" '
                         f'srcoff="{st["srcoff"]}" dstbuf="{st["dstbuf"]}" '
                         f'dstoff="{st["dstoff"]}" cnt="{st["cnt"]}" '
                         f'depid="-1" deps="-1" hasdep="0"')
                if "netdepid" in st:
                    attrs += f' netdepid="{st["netdepid"]}" netdeps="{st["netdeps"]}"'
                blk.append(f'      <step {attrs}/> <!-- {st["comment"]} -->')
            blk.append('    </tb>')
        blk.append('  </gpu>')
        blocks.append((comm.gpu_ids[rank], blk))
        total_tbs = max(total_tbs, len(tbs))
    # GPUs outside the communicator, spelled out rather than omitted. The parser would be
    # happy either way (a <gpu> with no <tb> has nBlocks == 0 and is not "active"), but
    # writing them makes the participant set readable from the file and matches how the
    # TE-CCL partial solves are emitted.
    for gid in range(comm.ngpus):
        if gid not in set(comm.gpu_ids):
            blocks.append((gid, [f'  <gpu id="{gid}" i_chunks="0" o_chunks="0" s_chunks="0"/>'
                                 f' <!-- not in the communicator -->']))
    for _, blk in sorted(blocks):
        lines.extend(blk)
    lines.append('</algo>')

    text = "\n".join(lines) + "\n"
    with open(out_path, "w") as f:
        f.write(text)
    return total_tbs, text


# ------------------------------------------------------------------------ main

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    # p2pnChannels: clamped to this simulator's MAXCHANNELS (msccl.h).  See
    # deviation 1 in the emitted header.
    P2P_N_CHANNELS = 8

    targets = [
        # (node sizes, topology description, output basename)
        ([16, 16, 16, 16, 8, 8, 8, 8],
         "dual_plane_hetero.cc : 96 GPUs, 8 NVLink domains [16,16,16,16,8,8,8,8],\n"
         "                     2x400G fabric NICs per GPU, clustered leaf assignment",
         "dual_plane_clustered_ncclalltoall"),
        ([8] * 32,
         "rail_optimized_256gpu.cc : 256 GPUs, 32 NVLink domains of 8,\n"
         "                     1x400G fabric NIC per GPU, rail optimized (local rank to leaf)",
         "rail_optimized_256gpu_ncclalltoall"),
        ([8] * 32,
         "rail_optimized_256gpu_dual_plane.cc : 256 GPUs, 32 NVLink domains of 8,\n"
         "                     2x400G fabric NICs per GPU (two planes), rail optimized",
         "rail_optimized_256gpu_dual_plane_ncclalltoall"),
    ]

    # ---- the mini_* controlled set, as the sweep's `p2p` config ----------------------
    #
    # Same transcription, aimed at the small topologies and at the partial-participation
    # subsets. This is the "what would NCCL do" arm: ncclAlltoAll posts one direct message
    # per ordered pair and routes nothing, so it is the natural floor for a solver that
    # relays or splits a pair across paths.
    #
    # Named for the harness's --sched knob rather than for --xml, so the sweep selects it
    # the same way it selects `milp`: --sched=p2p reads <stem>[_<TAG>]_<coll>_p2p.xml. The
    # config runs with every feature off, which means --rate=0, which means the harness
    # appends _no_rate -- so each schedule is written under BOTH names. They are byte
    # identical on purpose: NCCL has no rate model in the p2p path, so there is nothing for
    # a rate-annotated variant to say.
    #
    # host_of maps a GPU to its NVLink domain, which is what NCCL's rank layout turns on.
    # mini_1gpu_1nic has no host tier at all, so every GPU is its own single-rank "node".
    MINI_TOPOS = {
        "mini_1gpu_1nic": dict(stem="mini_1g1n", ngpus=4, host_of=lambda g: g,
                               desc="4 GPUs, no host tier (each GPU its own domain), 100G, 2:1 taper"),
        "mini_2gpu_1nic": dict(stem="mini_2g1n", ngpus=8, host_of=lambda g: g // 2,
                               desc="8 GPUs, 4 hosts of 2 sharing ONE 100G NIC off a PCIe root complex"),
        "mini_2gpu_2nic": dict(stem="mini_2g2n", ngpus=8, host_of=lambda g: g // 2,
                               desc="8 GPUs, 4 hosts of 2, one 100G NIC per GPU"),
    }
    # (scratch, scenario tag or None for the whole machine, participating gpu ids)
    MINI_JOBS = [
        ("mini_1gpu_1nic", None, [0, 1, 2, 3]),
        ("mini_2gpu_1nic", None, list(range(8))),
        ("mini_2gpu_2nic", None, list(range(8))),
        ("mini_1gpu_1nic", "1A", [0, 2]),
        ("mini_2gpu_1nic", "2B", [0, 2, 3, 5]),
        ("mini_2gpu_1nic", "2C", [0, 2, 3, 4, 5, 6]),
        ("mini_2gpu_2nic", "3A", [0, 2, 3, 4, 5, 6]),
    ]
    for scratch, tag, parts in MINI_JOBS:
        t = MINI_TOPOS[scratch]
        parts = sorted(parts)
        # Participants grouped by NVLink domain, empty domains dropped. Comm requires a
        # node's ranks to be contiguous, which holds because parts is sorted and host_of is
        # monotone in the gpu id.
        sizes, order = [], []
        for g in parts:
            h = t["host_of"](g)
            if order and order[-1] == h:
                sizes[-1] += 1
            else:
                order.append(h)
                sizes.append(1)
        base = f"{t['stem']}" + (f"_{tag}" if tag else "") + "_a2a_p2p"
        comm = Comm(sizes, P2P_N_CHANNELS, gpu_ids=parts, ngpus=t["ngpus"])
        validate(comm)
        who = f"{scratch} scenario {tag}" if tag else f"{scratch} (all ranks)"
        desc = (f"{who} : {t['desc']}\n"
                f"                     communicator = {len(parts)} rank(s), gpus {parts},"
                f" domains {sizes}")
        ntb, text = emit(comm, desc, base, os.path.join(here, base + ".xml"))
        with open(os.path.join(here, base + "_no_rate.xml"), "w") as f:
            f.write(text)   # identical: no rate= attribute exists to strip
        print(f"{base}.xml (+_no_rate): ranks={comm.n_ranks} gpus={parts} domains={sizes} "
              f"groupSize={comm.group_size} nGroups={comm.n_groups} tbs/rank={ntb}")

    # rail_optimized_256gpu_dual_plane also carries a --sched knob, so its NCCL schedule is
    # emitted a second time under the name that knob derives. Same 256-rank communicator and
    # therefore the same schedule as the _ncclalltoall.xml above -- the duplicate exists only
    # so the sweep can select it as the `p2p` COLUMN (--sched=p2p) rather than as a one-off
    # --xml run, which is what puts it in the same table as baseline. As with the mini set,
    # both the plain and the _no_rate name are written and are byte identical, because
    # feature-off implies --rate=0 and NCCL has no rate model to strip.
    for sizes, topo, base in [t for t in targets
                              if t[2] == "rail_optimized_256gpu_dual_plane_ncclalltoall"]:
        comm = Comm(sizes, P2P_N_CHANNELS)
        validate(comm)
        sched_base = "rail_optimized_256gpu_dual_plane_alltoall_p2p"
        ntb, text = emit(comm, topo, sched_base, os.path.join(here, sched_base + ".xml"))
        with open(os.path.join(here, sched_base + "_no_rate.xml"), "w") as f:
            f.write(text)
        print(f"{sched_base}.xml (+_no_rate): nRanks={comm.n_ranks} tbs/rank={ntb} "
              f"size: {os.path.getsize(os.path.join(here, sched_base + '.xml'))/1e6:.2f} MB each")

    for sizes, topo, base in targets:
        comm = Comm(sizes, P2P_N_CHANNELS)
        validate(comm)
        out = os.path.join(here, base + ".xml")
        ntb, _ = emit(comm, topo, base, out)
        chan_batches = {}
        for k in range(comm.n_groups):
            chan_batches.setdefault(reverse_bits(k, log2_up(comm.p2p_n_channels)), []).append(k)
        print(f"{base}.xml: nRanks={comm.n_ranks} groupSize={comm.group_size} "
              f"nGroups={comm.n_groups} p2pnChannels={comm.p2p_n_channels} "
              f"pcp={comm.p2p_n_channels_per_peer} tbs/rank={ntb}")
        print("   batches per channel: " +
              ", ".join(f"c{c}:{len(v)}" for c, v in sorted(chan_batches.items())))
        print(f"   size: {os.path.getsize(out)/1e6:.2f} MB")


if __name__ == "__main__":
    main()
