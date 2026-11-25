# NCCL `ncclAlltoAll`, as an MSCCL XML schedule

Three schedules for the large topologies, selected with `--xml`:

| XML | example | ranks | NVLink domains | fabric NICs/GPU |
|---|---|---|---|---|
| `dual_plane_clustered_ncclalltoall.xml` | `dual_plane_hetero.cc` | 96 | `[16,16,16,16,8,8,8,8]` | 2 × 400G |
| `rail_optimized_256gpu_ncclalltoall.xml` | `rail_optimized_256gpu.cc` | 256 | 32 × 8 | 1 × 400G |
| `rail_optimized_256gpu_dual_plane_ncclalltoall.xml` | `rail_optimized_256gpu_dual_plane.cc` | 256 | 32 × 8 | 2 × 400G |

…and sixteen selected with `--sched=p2p`, which is the sweep's `p2p` config (§7): fourteen
for the `mini_*` controlled set, plus `rail_optimized_256gpu_dual_plane_alltoall_p2p.xml`
and its `_no_rate` twin. Those last two hold the same 256-rank schedule as
`rail_optimized_256gpu_dual_plane_ncclalltoall.xml` — byte identical bar the `algo name`
attribute. The duplicate is deliberate: `--xml` runs a schedule as a one-off, `--sched` runs
it as a sweep COLUMN alongside `baseline`, and only the latter puts NCCL and the TE-CCL
solve in the same table. It costs about 2.1 MB of git objects (the two names are byte
identical to each other, so they share one blob).

All of them are produced by `gen_nccl_alltoall.py` (`python3 gen_nccl_alltoall.py`), which
transcribes NCCL 2.31.2-1 (`7b83616d`) and self-checks the permutation before emitting.
Do not hand-edit the XMLs.

---

## 1. What NCCL actually issues

`ncclAlltoAll` is not a collective algorithm. There is no cost model, no algorithm selection
and no protocol tuning anywhere in its path — `ncclTuningCompute` is never called for it and
structurally cannot be (`ncclFuncAlltoAll = 8`, `NCCL_NUM_FUNCTIONS = 5`).

The schedule is a pure function of `nRanks`, the per-node rank counts, `p2pSchedGroupSize`,
`p2pnChannels` and `p2pnChannelsPerPeer`. In order:

1. **Fan-out** (`enqueue.cc:3320`). For every `r` in `[0, nRanks)`, unconditionally append one
   send task and one recv task: `2 × nRanks` tasks, each of `count × eltSize` bytes.
   `r == rank` is included.
2. **Round permutation** (`ncclP2pSchedule`, `init.cc:940`), computed once at communicator init.
   Ranks are cut into groups of `p2pSchedGroupSize = 8`; group deltas are visited in the
   quadratic order `(x²+x)/2 mod pow2Up(nGroups)` with out-of-range deltas filtered out; within
   each group delta, `8` consecutive rounds sweep `delta ∈ [0,8)` with send rotating forward and
   recv rotating backward. Result: `p2pSchedule[round] = {sendRank, recvRank}`.
3. **Round → channel** (`ncclP2pChannelBaseForRound`, `channel.h:21`). With
   `p2pSchedGroupSize == NCCL_MAX_DEV_WORK_P2P_PER_BATCH == 8` this collapses to
   `channel = reverseBits(round / 8, log2Up(p2pnChannels))`.
4. **Batching** (`ncclAddWorkBatchToPlan`, `enqueue.cc:152`). Rules 4 and 6 together mean one
   batch per (channel, group-of-8-rounds), holding up to 8 works.
5. **Execution** (`sendrecv.h`). Within a batch: all works run concurrently on disjoint warp
   ranges, and each work's send and recv run concurrently too. Across batches on one channel:
   strictly sequential. Across channels: concurrent (one CUDA block each).

The resulting order and serialization scheme, in one line:

> **8 channels run in parallel; each channel walks its batches in order; each batch fires its
> 8 peers at once, send and recv concurrently.**

## 2. How that maps onto this simulator

| NCCL | MSCCL XML / ns-3 runtime | why |
|---|---|---|
| one p2p work | one `<tb>` with one `<step>` | — |
| send ∥ recv of a work | **two** `<tb>`s per round, `recv="-1"` and `send="-1"` | steps inside a `<tb>` are strictly serial (`TBState::busy`); fusing them would falsely serialize |
| channel = CUDA block | `chan=` | runtime keys one persistent qp per `(channel, peer)` — exactly NCCL's per-channel connection |
| works within a batch | sibling `<tb>`s, no dependences | all `<tb>`s are scheduled at once, gated only by deps |
| batches sequential on a channel | `netdepid`/`netdeps` chain | `netdep` waits on *wire* completion; `depid` would impose nothing, since a send step "completes" for `depid` the instant it is posted |
| self pair → device-side copy | `type="cpy"`, `send="-1" recv="-1"` | AllToAll is out-of-place, so `enqueue.cc`'s self-elision (which needs `send->buff == recv->buff`) does not fire |
| `p2pChunkSize` / `NCCL_STEPS` ring | **command line**, not the XML | it is a transport property, not a schedule |
| no rate model | no `rate=` attribute anywhere | adding rates would model a different runtime |

Per rank: `nRanks` rounds → `nRanks-1` send tbs + `nRanks-1` recv tbs + 1 copy tb
(191 tbs at 96 ranks, 511 at 256).

`i_chunks = o_chunks = nchunksperloop = nRanks`, `cnt="1"`, so one chunk per peer and
`CHUNK_SIZE = inputBytes / nRanks` — one contiguous message per peer, which is what NCCL posts.

Recv tbs are deliberately **not** chained. Each `(channel, recvpeer)` pair carries exactly one
recv step per rank, so the runtime's peer-keyed FIFO matching is unambiguous, and posting recvs
early costs nothing on the wire — an RC receiver is always ready. Arrival order on the receiver
is set by the sender's batch order, as it is in NCCL.

## 3. Running them

The three examples derive their XML filename from `--coll`, which is whitelisted to
`allgather|alltoall`, so these schedules are selected with the `--xml` override added for them
(it replaces the filename only; the path, the tester and the switch-JSON name are untouched):

```sh
./ns3 run "dual_plane_hetero --xml=dual_plane_clustered_ncclalltoall.xml \
    --flowId=0 --nicSel=merged --inputBytes=134217728 \
    --protoChunkBytes=131072 --maxMsgsInFlight=8 --qlenRows=0"

./ns3 run "rail_optimized_256gpu --xml=rail_optimized_256gpu_ncclalltoall.xml \
    --flowId=0 --nicSel=rr --inputBytes=134217728 \
    --protoChunkBytes=131072 --maxMsgsInFlight=8 --qlenRows=0"

./ns3 run "rail_optimized_256gpu_dual_plane --xml=rail_optimized_256gpu_dual_plane_ncclalltoall.xml \
    --flowId=0 --nicSel=merged --inputBytes=134217728 \
    --protoChunkBytes=131072 --maxMsgsInFlight=8 --qlenRows=0"
```

Flag by flag:

- `--xml=...` selects the schedule. Without it the harness would load the TACCL/TE-CCL
  `*_alltoall.xml` as before; `--coll` stays at its default `alltoall`.
- `--flowId=0` and a `nicSel` other than `schedule`: there is no switch JSON for these (and there
  should not be — NCCL neither pins NICs from a schedule nor stamps flow ids). Either flag left on
  makes the harness look for `<prefix>_alltoall.json` and pin/route by a schedule this algorithm
  never produced.
- `--nicSel=merged` on the two dual-plane topologies is NCCL's own `NCCL_IB_MERGE_NICS`: the two
  NICs reaching a peer are fused into one logical device and every message is split across them.
  On single-NIC `rail_optimized_256gpu` all three modes coincide; `rr` is the honest name for it.
- `--protoChunkBytes=131072` is `NCCL_P2P_NET_CHUNKSIZE` (`init.cc:868`). Use `262144` to model a
  Grace/ARM host with `compCap >= 100`, which doubles it.
- `--maxMsgsInFlight=8` is `NCCL_STEPS` (`device.h:26`), the depth of the p2p buffer ring.
- `--rate` is irrelevant here and should be left at its default: these schedules carry no rates,
  and `--xml` bypasses the `_no_rate` filename logic entirely.
- `--netDeps` must stay at its default `1`. It is what enforces batch ordering; `--netDeps=0` is
  the meaningful ablation ("release every batch at once") and is worth running as a contrast.

## 4. Deviations, stated

1. **`p2pnChannels = 8`.** Real NCCL takes this from the graph search and would likely report 16
   or 32 on this hardware. This simulator's `MAXCHANNELS` is 8 (`msccl.h`). The clamp is benign
   for the *per-peer* schedule: the multi-node reduction in `paths.cc:1008` forces
   `p2pnChannelsPerPeer = 1` for any `p2pnChannels <= 16` at 96 or 256 ranks, so 8 and 16 emit
   identical per-peer work — no multi-part split, and the 4 KiB part partitioning of
   `device.h:261` stays inert. What 8 does change is batch concurrency: 12 groups over 8 channels
   gives a channel at most 2 batches at 96 ranks, 4 at 256, whereas 32 channels would let every
   batch run at once. **These files are therefore, if anything, less bursty than the hardware.**
2. **Batch ordering is per-slot, not block-wide.** NCCL's kernel barriers the whole block between
   batches. An XML `netdep` can name only one `(tb, step)` pair, so an 8-way join is not
   expressible. Chaining slot `j` of batch `k+1` to slot `j` of batch `k` gives 8 independent
   serial lanes per channel: same batch order, same 8-way intra-batch concurrency, but a fast
   slot may run one batch ahead of a slow one. For alltoall, where every work is the same size,
   the two are close. The alternative — picking one "representative" work of each batch to chain
   against — would be arbitrary and would bias toward whichever peer was picked.
3. **`allowUB = false`** (unregistered buffers, no graph capture), so the net/IPC registration
   branches are dead and `nChannels` per direction is whatever `§7.2` produced, which is 1.
4. **The `u32fp8` chunk-size round-trip and LL protocol are not modelled.** LL only applies below
   16 KiB per peer (`NCCL_P2P_LL_THRESHOLD`); at the message sizes these topologies are run at,
   every peer is SIMPLE. If you sweep down to tiny `inputBytes`, that assumption breaks.
5. **`ncclHierCeAlltoAll` is off**, matching the default `CTAPolicy` (no `NCCL_CTA_POLICY_ZERO`).

## 5. What the schedule looks like on each fabric

Because a batch is one whole group of 8 rounds, and groups are cut inside a node,
**every batch is 8 consecutive ranks of a single NVLink domain** — a batch is entirely
intra-node or entirely aimed at one remote node.

Rank 0, `dual_plane_hetero` (96 ranks, 8 channels):

```
chan 0: k=0  -> ranks 0..7   (intra) | k=8  -> ranks 16..23
chan 1: k=4  -> ranks 80..87
chan 2: k=2  -> ranks 24..31         | k=10 -> ranks 72..79
chan 3: k=6  -> ranks 32..39
chan 4: k=1  -> ranks 8..15  (intra) | k=9  -> ranks 88..95
chan 5: k=5  -> ranks 40..47
chan 6: k=3  -> ranks 48..55         | k=11 -> ranks 64..71
chan 7: k=7  -> ranks 56..63
```

16 of rank 0's 96 sends stay inside its 16-GPU NVLink domain; the other 80 hit the fabric. Four
channels carry one batch and fire it immediately, so **64 of the 80 remote peer messages are
posted at t=0**, and the remaining 16 follow one wire-completion later. That burst is the thing
to look at against the TACCL schedule in `dual_plane_clustered_alltoall.xml`, which paces.

Rank 0, `rail_optimized_256gpu` (256 ranks, 8 channels, 4 batches per channel): only 8 of 256
sends are intra-domain. Every batch targets all 8 local ranks of one remote node — i.e. one GPU
on each of the 8 rails. **Rank 0 has a single NIC, on leaf 0, so 7 of every 8 messages in a batch
leave its rail and must cross the spine.** NCCL's p2p schedule is fabric-blind (the spec is
explicit that link contention is not modelled anywhere in NCCL), so it makes no attempt to
exploit rail locality; that is the headline result this XML exists to measure.

The dual-plane 256-GPU variant issues the identical schedule — the two files differ only in the
name — and differs at runtime purely in `--nicSel`, where `merged` stripes each message over both
planes.

## 6. A routing bug these schedules exposed

The first run of `rail_optimized_256gpu` with this XML failed with a stream of
`ERROR: shouldn't receive ack` (`rdma-hw.cc:665`). That error is only reachable when a NACK
arrives while `L2AckInterval == 0` (the default no-ack mode), and the only NACK generator is
`ReceiverCheckSeq` returning 2 — a sequence gap. Source-side reordering is impossible (a qp's
NIC is resolved once and cached; switch ECMP is a deterministic 5-tuple hash), and the
headroom-exhaustion drop prints `"Drop: ... Headroom full"`, which never appeared. That leaves
lost packets from the one silent drop path: `SendToDev`'s `idx < 0`.

Cause: `RdmaFabricHelper::BfsFromDestination` ran unweighted BFS over the whole qbb graph. Its
stated assumption — "GPUs are always leaves of the qbb subgraph" — is false for a multi-homed
GPU, which has an NVLink port *and* one or more fabric ports and is therefore a transit node of
that graph. So `leaf_j -> gpu(node n, rank j) -> nvswitch_n -> gpu(node n, rank k)` is three
hops, tying exactly with `leaf_j -> spine -> leaf_k -> gpu`, and the tie was installed as an
equal-cost next hop. Packets hashed onto it were handed to a GPU that is not a router, which
drops them silently.

Measured over the real adjacency of each example:

| topology | leaf routing entries with a black-hole next hop | share of packets lost per affected flow |
|---|---|---|
| `rail_optimized_256gpu` | 1792 / 3072 | ~3% (1 of 33 next-hop edges) |
| `rail_optimized_256gpu_dual_plane` | 1536 / 3072 | ~6% (2 of 34) |
| `dual_plane_hetero` | 0 / 960 | none |

`dual_plane_hetero` is clean because its leaf assignment is *clustered*: the GPUs on a leaf are
exactly the GPUs of the NVLink domains that leaf owns, so a transit-through-GPU path is never
shorter than or equal to the fabric path. Rail-optimized assignment (leaf `j` owns rank `j` of
every node) makes the tie universal. That is why this surfaced only now — `xml_input/` holds no
other schedule for either rail-optimized example, so this is the first all-pairs traffic ever
run on them.

Fixed in `rdma-fabric-helper.cc` in two places: the BFS no longer relaxes through a GPU other
than the root, and next-hop selection rejects a GPU peer unless it is the destination. Both are
needed — the second alone leaves the dead ends, and the first alone leaves them selectable,
since a non-destination GPU still has a valid distance.

Verified over the real adjacency of all three topologies: zero black holes, every switch keeps a
valid next hop, every GPU pair stays reachable, and **`dual_plane_hetero`'s next-hop sets are
bit-identical to before** (0 of 10080 changed), so results already collected on that topology are
unaffected. The two rail-optimized topologies' routes do change substantially — necessarily, as
they were previously routing a share of every affected flow into a black hole.

---

## 7. The `mini_*` set: the sweep's `p2p` config

The same transcription, aimed at the three small topologies and at the four
partial-participation subsets. Seven communicators, each written under two names:

| scenario | scratch | `--scenario` | communicator | XML (plus a `_no_rate` twin) |
|---|---|---|---:|---|
| full | `mini_1gpu_1nic` | — | 4 ranks, domains `[1,1,1,1]` | `mini_1g1n_a2a_p2p.xml` |
| full | `mini_2gpu_1nic` | — | 8 ranks, domains `[2,2,2,2]` | `mini_2g1n_a2a_p2p.xml` |
| full | `mini_2gpu_2nic` | — | 8 ranks, domains `[2,2,2,2]` | `mini_2g2n_a2a_p2p.xml` |
| 1A | `mini_1gpu_1nic` | `1A` | g0,g2 — `[1,1]` | `mini_1g1n_1A_a2a_p2p.xml` |
| 2B | `mini_2gpu_1nic` | `2B` | g0,g2,g3,g5 — `[1,2,1]` | `mini_2g1n_2B_a2a_p2p.xml` |
| 2C | `mini_2gpu_1nic` | `2C` | g0,g2,g3,g4,g5,g6 — `[1,2,2,1]` | `mini_2g1n_2C_a2a_p2p.xml` |
| 3A | `mini_2gpu_2nic` | `3A` | g0,g2,g3,g4,g5,g6 — `[1,2,2,1]` | `mini_2g2n_3A_a2a_p2p.xml` |
| — | `rail_optimized_256gpu_dual_plane` | — | 256 ranks, 32 domains of 8 | `rail_optimized_256gpu_dual_plane_alltoall_p2p.xml` |

Run them through the sweep rather than by hand:

```sh
./sweep_dual_plane_features.py mini    --configs=baseline,p2p --start 4KB --end 64MB
./sweep_dual_plane_features.py partial --configs=baseline,p2p,milp --start 4KB --end 64MB
./sweep_dual_plane_features.py rail_optimized_256gpu_dual_plane --configs=baseline,p2p \
    --start 1MB --end 128MB
```

Note the filename convention differs between the two families, because the scratches do:
`mini_*` abbreviate the collective (`mini_1g1n_a2a_p2p.xml`) while the large scratches spell
it out (`rail_optimized_256gpu_dual_plane_alltoall_p2p.xml`). The sweep scrapes which
convention a scratch uses out of its source rather than being told.

`rail_optimized_256gpu` and `dual_plane_hetero` have NCCL schedules but no `--sched` knob, so
the sweep skips `p2p` on them with a warning and they stay `--xml`-only. Adding the knob is a
six-line change — copy it from `rail_optimized_256gpu_dual_plane.cc`.

### Why `--sched=p2p` and not `--xml`

The large three are selected with `--xml` because they have no sibling solves. These do:
each mini scenario has a TE-CCL multipath solve (the default), a single-chunk-per-pair
`milp` solve, and this. `--sched` is the harness knob for "same topology and collective,
different solve", it composes with `--scenario`, and it is what lets the sweep put all
three in one table as columns. `--xml` would override only the XML name and would not
compose with the scenario tag.

### Why each file exists twice

The `p2p` config runs with every feature off, which includes `--rate=0`, and the harness
appends `_no_rate` to the derived filename in that case. NCCL has no rate model in the p2p
path, so there is nothing for a rate-annotated variant to say and the two files are byte
identical. `<stem>_p2p.xml` also has to exist for the sweep's `has_sched` probe.

### Partial communicators

NCCL numbers the ranks of a communicator `0..n-1` and knows nothing about the GPUs left
out, which is exactly the partial case. So `<gpu id=>`, `send=` and `recv=` carry ns-3 node
ids while `srcoff`/`dstoff` stay in **rank** space — the space `CollectiveTester` numbers
its participants by. Non-participants are written out as empty `<gpu>` elements so the
communicator is readable from the file. Unlike the TE-CCL solves, a p2p schedule never
relays: every ordered pair sends directly, so there are no relay GPUs here.

### Properties worth stating, since this is a reference column

One qp per ordered pair in all seven (verified: pairs == qps), `chunks/dest == 1`, no
`rate=` attribute on any step, and one `type="cpy"` self step per rank. Cross-leaf flows
per direction: 4 / 16 / 16 for the full topologies and **1 / 3 / 9 / 9** for 1A / 2B / 2C /
3A — all four odd, which is the ECMP-parity property those subsets were chosen for.

### Deviation specific to this set

The sweep holds `--protoChunkBytes=2MiB` and `--maxMsgsInFlight=8` constant across every
config, so the `p2p` column is NCCL's **schedule** on this sweep's transport, not NCCL's
transport as well (real NCCL uses a 128 KiB p2p chunk — `NCCL_P2P_NET_CHUNKSIZE`). Varying
the transport per column would confound the schedule comparison, which is what is being
measured. Deviation 1 of §4 applies too: `p2pnChannels` is clamped to this simulator's
`MAXCHANNELS = 8`, which at these rank counts is far more channels than rounds, so most
channels carry a single batch and the `netdep` batch chain is nearly inert.
