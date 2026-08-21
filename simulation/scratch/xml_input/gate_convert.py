#!/usr/bin/env python3
"""Convert send-anchored depid/deps in an MSCCL XML into netgate/netwait pairs.

Background
----------
depid/deps expresses "the anchor step has completed."  For a *send* step the
executor completes at post -- the message has only reached the RDMA engine --
so a send-anchored depid is vacuous and releases the dependent message onto the
wire immediately.  Network gates (netgate/netwait) express wire ordering
instead: a gate opens when the owning send's message *completes* on the qp, and
a waiter is not posted until it is open.

This script rewrites exactly those deps whose anchor step is send-bearing and
leaves every buffer-readiness dep (recv/copy/reduce anchors) alone.

Encoding notes
--------------
* `deps` names the anchor's *global* step id, which the simulator's step
  numbering keeps equal to the anchor step's XML `s` index (nops consume an `s`
  index without advancing local step, and the accumulated-dependence bump makes
  up exactly the difference).  So the anchor is `tb[depid]/step[s=deps]`.
* Step `s` indices are never renumbered: other threadblocks reference steps by
  that index, so renumbering would silently repoint their deps.
* A nop step exists only to attach an extra dependence to the next real
  transfer.  When its dep becomes a gate the nop is emptied (depid=-1), which
  the parser then ignores entirely, rather than removed.
* The parser rejects a transfer that has accumulated dependences but no depid of
  its own, so the surviving data deps of a group are repacked with the last one
  landing on the transfer step itself.

Gate ids are node-local and assigned densely per GPU in sorted anchor order.
"""

import sys
import xml.etree.ElementTree as ET

SEND_TYPES = {"s", "rcs", "rrs", "rrcs"}


def convert_gpu(gpu):
    """Rewrite one <gpu>'s send-anchored deps in place. Returns (n_gates, n_waits)."""
    tbs = {int(tb.get("id")): tb for tb in gpu.findall("tb")}
    steps = {b: {int(st.get("s")): st for st in tb.findall("step")} for b, tb in tbs.items()}

    def is_send_anchor(bid, sid):
        anchor = steps.get(bid, {}).get(sid)
        return anchor is not None and anchor.get("type") in SEND_TYPES

    # Pass 1: which anchors are send anchors, i.e. which ops own a gate.
    anchors = set()
    for bid in steps:
        for st in steps[bid].values():
            dep_bid, dep_sid = int(st.get("depid")), int(st.get("deps"))
            if dep_bid >= 0 and is_send_anchor(dep_bid, dep_sid):
                anchors.add((dep_bid, dep_sid))
    gate_of = {a: i for i, a in enumerate(sorted(anchors))}

    # Pass 2: rewrite each dependence group (a run of dep-carrying nops plus the
    # transfer they attach to) into at most one gate wait plus the data deps.
    n_waits = 0
    for bid, tb in tbs.items():
        pending = []  # deps contributed by the nops seen so far in this group
        nops = []
        for sid in sorted(steps[bid]):
            st = steps[bid][sid]
            dep_bid, dep_sid = int(st.get("depid")), int(st.get("deps"))
            if st.get("type") == "nop":
                nops.append(st)
                if dep_bid >= 0:
                    pending.append((dep_bid, dep_sid))
                continue

            group = pending + ([(dep_bid, dep_sid)] if dep_bid >= 0 else [])
            gates = [d for d in group if d in gate_of]
            data = [d for d in group if d not in gate_of]
            assert len(gates) <= 1, (
                f"gpu {gpu.get('id')} tb {bid} step {sid} would wait on {len(gates)} gates; "
                "an op may wait on at most one"
            )
            if gates:
                assert st.get("type") in SEND_TYPES, (
                    f"gpu {gpu.get('id')} tb {bid} step {sid} is type {st.get('type')}; "
                    "gates are valid only on send-bearing ops"
                )
                st.set("netwait", str(gate_of[gates[0]]))
                n_waits += 1

            # Repack the surviving data deps: last one on the transfer (the parser
            # requires that), the rest spread over this group's nops, remainder emptied.
            carried = data[:-1] if data else []
            for nop in nops:
                if carried:
                    d = carried.pop(0)
                    nop.set("depid", str(d[0]))
                    nop.set("deps", str(d[1]))
                else:
                    nop.set("depid", "-1")
                    nop.set("deps", "-1")
            assert not carried, f"gpu {gpu.get('id')} tb {bid} step {sid}: too few nops to carry deps"
            if data:
                st.set("depid", str(data[-1][0]))
                st.set("deps", str(data[-1][1]))
            else:
                st.set("depid", "-1")
                st.set("deps", "-1")
            pending, nops = [], []

        assert not pending, f"gpu {gpu.get('id')} tb {bid}: trailing nop deps with no transfer to attach to"
        for nop in nops:
            nop.set("depid", "-1")
            nop.set("deps", "-1")

    # Pass 3: stamp the owners.
    for (bid, sid), gate in gate_of.items():
        steps[bid][sid].set("netgate", str(gate))

    return len(gate_of), n_waits


def main(src, dst):
    tree = ET.parse(src)
    root = tree.getroot()
    total_gates = total_waits = 0
    for gpu in root.findall("gpu"):
        gates, waits = convert_gpu(gpu)
        total_gates += gates
        total_waits += waits
        print(f"  gpu {gpu.get('id'):>3}: {gates} gates, {waits} wait sites")
    tree.write(dst, encoding="utf-8", xml_declaration=False)
    print(f"{src} -> {dst}: {total_gates} gates, {total_waits} wait sites")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <in.xml> <out.xml>")
    main(sys.argv[1], sys.argv[2])
