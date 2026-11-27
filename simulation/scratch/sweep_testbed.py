#!/usr/bin/env python3
"""Run every testbed schedule in xml_input/ through testbed_3node, at the sizes the hardware ran.

    scratch/sweep_testbed.py                      # every schedule, every size
    scratch/sweep_testbed.py --only teccl-two     # schedules whose name matches a regex
    scratch/sweep_testbed.py --hw <perf>/summary.txt [<perf>/summary.txt ...]   # + comparison

The schedules are scratch/xml_input/testbed_3node_<a2a|ag>_<schedule>.xml, lowered from one day's
solve. <schedule> is the row name in that day's hardware summary (experiments
results/testbed/<day>/perf-*/summary.txt) without its size. The cases match scripts/cluster/
msccl_perf.sbatch in the experiments repo:

  - TACCL and TE-CCL are solved per size, so <schedule> ends in -<size>MB, and that file runs only
    at its own size.
  - OptCCL (-K<k>) and ForestColl (-k<k>) are size-independent, so each runs at every --sizes.
    The plain optccl-<coll> file is the same bytes as optccl-<coll>-K16; the hardware has no row for
    it either, so it is skipped.
  - A schedule using an MSCCL step type the simulator does not implement yet (rrs, rrcs: see
    UNSUPPORTED_STEPS) is listed as skipped with the reason instead of being run. None does today.
    ForestColl's AllGather trees forward with rcs, which runs; --fused-slices sets how it forwards
    (2, the default, is the kernel's recvCopySend: two slices per sub-transfer, each sent once it
    has wholly landed, at least 32 KiB; 1 is store-and-forward of the whole sub-transfer).

SIZES are base 2: KB = 2^10 bytes, MB = 2^20, nccl-tests' convention and every earlier ns-3
sweep's. A size is a per-GPU chunk C, written the way the schedule files and the hardware
summaries name it: 0.016 / 0.064 / 0.256 are 16 / 64 / 256 KB (C = 16 x 2^10, ...), and 1 / 4 /
16 / 64 are MB (C = 2^20, ...). --sizes takes either spelling (0.016 or 16KB, 1 or 1MB). The
nccl-tests buffer is 3 x C for both collectives. The harness's --inputBytes is one rank's
INPUT, which differs between the two:

    alltoall   a rank sends a chunk to every rank:   --inputBytes = 3 x C
    allgather  a rank contributes one chunk:         --inputBytes =     C, --coll=allgather

The solves and the 2026-11-25 hardware runs (experiments scripts/cluster/msccl_perf.sbatch)
used DECIMAL sizes instead, C = S x 10^6 bytes: 4.9% smaller at MB sizes and 2.3% at KB sizes.
That is too little to need a re-solve, but a sim/hw ratio compares slightly different byte
counts until the hardware runs use base 2 too.

Time is the harness's "Total simulated time" (the collective's last step). Bandwidth is
computed here as buffer / time, which is nccl-tests' algbw. The harness's own "algorithm
bandwidth" line multiplies the input by the rank count, so for AllToAll it reads 3x higher.

PIPELINING. Every run passes --protoChunkBytes=2097152 (2 MiB; --proto-chunk-bytes to change it),
the MSCCL kernel's gridOffset step on the testbed's runtime (executor a84c69f6): NCCL's Simple
protocol buffer, 4 MiB (DEFAULT_BUFFSIZE, NCCL_BUFFSIZE unset on the testbed), over NCCL_STEPS = 8
FIFO steps is 512 KiB per step (calcBytePerStep), and msccl_kernel.cu walks a chunk
MSCCL_CHUNKSTEPS = 4 steps at a time, 2 MiB. Cross-threadblock dependencies are checked per
iteration (the flag is (workIndex, iter, step)), so a send that depends on a receive starts once
the receive's first 2 MiB has arrived, not the whole chunk. At 0, the harness default, the
simulator waits for the whole chunk, which made OptCCL's AllGather K1 up to 1.46x the hardware.

Every run keeps the correctness check on (cheap at 3 ranks) and passes --qlenRows=0. Otherwise
it runs testbed_3node's defaults (--flowId=0 --nicSel=merged, acks on, MTU 4096). Results go to
sweep_results/testbed_3node_hw/: summary.txt in the hardware summaries' column layout (the
in-place columns read '-'), results.csv, and per-run output in runs/. An interrupted sweep picks
up where it stopped; --force re-runs. Build first (./ns3 build scratch/testbed_3node): every run
is --no-build.
"""
import argparse, concurrent.futures as cf, csv, glob, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
NS3_DIR = os.path.dirname(HERE)
XML_DIR = os.path.join(HERE, "xml_input")
PROGRAM = "testbed_3node"
RANKS = 3
SIZES_MB = ["0.016", "0.064", "0.256", "1", "4", "16", "64"]   # the schedules' and summaries' labels
# MSCCL step types collectives.cc does not implement yet (it aborts on them). A schedule with any
# of them is reported as skipped, with the reason, rather than run and counted as a failure.
UNSUPPORTED_STEPS = {"rrs": "RecvRedSend", "rrcs": "RecvRedCpSend"}
# FusedSendSlices for rcs steps (--fusedSlices); 2 is the MSCCL kernel's recvCopySend.
FUSED_SLICES = 2
# The MSCCL kernel's gridOffset step on the testbed's runtime; see PIPELINING above.
PROTO_CHUNK_BYTES = 2 * 1024 * 1024
FIELDS = ["proto_chunk_bytes", "fused_slices", "case", "schedule", "coll", "size_mb", "chunk_bytes", "buffer_bytes", "input_bytes", "sim_ns", "sim_us",
          "sim_GBps", "check", "status"]


def chunk_bytes(label):
    """A size label's per-GPU chunk, base 2: below 1 the label is KB/1000 (0.016 = 16 KB)."""
    v = float(label)
    n = v * 2**20 if v >= 1 else round(v * 1000) * 2**10
    if n != int(n) or n <= 0:
        raise SystemExit(f"size {label}: not a whole number of bytes in base 2")
    return int(n)


def label_of(size):
    """'16KB' -> '0.016', '1MB' -> '1'; a bare label passes through."""
    m = re.fullmatch(r"([0-9.]+)\s*(KB|MB)?", size.strip(), re.I)
    if not m:
        raise SystemExit(f"cannot read size {size!r}: give 0.016 or 16KB, 1 or 1MB")
    v, unit = float(m.group(1)), (m.group(2) or "").upper()
    v = v / 1000 if unit == "KB" else v
    return f"{v:g}"


def input_bytes(coll, label):
    return chunk_bytes(label) * (RANKS if coll == "alltoall" else 1)


def cases(sizes, only):
    """(case, xml, coll, size_mb) for every schedule in xml_input, in the hardware's row names."""
    out = []
    for path in sorted(glob.glob(os.path.join(XML_DIR, f"{PROGRAM}_*_*.xml"))):
        m = re.match(rf"{PROGRAM}_(a2a|ag)_(.+)\.xml$", os.path.basename(path))
        if not m:
            continue
        coll = "alltoall" if m.group(1) == "a2a" else "allgather"
        sched = m.group(2)
        if re.fullmatch(rf"optccl-{coll}", sched):
            continue                                   # identical to optccl-<coll>-K16
        if only and not re.search(only, sched):
            continue
        s = re.search(r"-([0-9.]+)MB$", sched)
        for mb in ([s.group(1)] if s else sizes):
            if s and mb not in sizes:
                continue
            out.append((f"{sched}@{mb}MB", os.path.basename(path), coll, mb))
    return out


def unsupported(xml):
    with open(os.path.join(XML_DIR, xml)) as f:
        types = set(re.findall(r'type="([a-z]+)"', f.read()))
    return sorted(t for t in types if t in UNSUPPORTED_STEPS)


def run_one(case, xml, coll, mb, outdir, proto, fused):
    chunk = chunk_bytes(mb)
    buf = chunk * RANKS
    ib = input_bytes(coll, mb)
    row = dict(proto_chunk_bytes=proto, fused_slices=fused, case=case, schedule=case.rsplit("@", 1)[0], coll=coll, size_mb=mb,
               chunk_bytes=chunk, buffer_bytes=buf, input_bytes=ib, sim_ns="", sim_us="", sim_GBps="",
               check="none")
    bad = unsupported(xml)
    if bad:
        row["status"] = "skipped: no " + ",".join(f"{t} ({UNSUPPORTED_STEPS[t]})" for t in bad) \
                        + " step in the simulator"
        return row
    label = "tb_" + re.sub(r"[^A-Za-z0-9_.-]", "_", case)
    args = [PROGRAM, f"--xml={xml}", f"--coll={coll}", f"--inputBytes={ib}",
            f"--protoChunkBytes={proto}", f"--fusedSlices={fused}", "--correctness=1", "--checkLog=minimal", "--qlenRows=0",
            f"--label={label}"]
    proc = subprocess.run(["./ns3", "run", "--no-build", " ".join(args)], cwd=NS3_DIR,
                          capture_output=True, text=True)
    text = proc.stdout + proc.stderr
    with open(os.path.join(outdir, "runs", label + ".log"), "w") as f:
        f.write(" ".join(args) + "\n\n" + text)
    t = re.search(r"Total simulated time: (\d+) nanoseconds", text)
    check = ("verified" if re.search(rf"{coll} verified", text) else
             "INCORRECT" if re.search(rf"{coll} incorrect", text) else "none")
    sim_ns = int(t.group(1)) if t else None
    status = "ok" if proc.returncode == 0 and sim_ns and check == "verified" else (
        f"FAILED exit={proc.returncode}" if proc.returncode else "FAILED")
    row.update(sim_ns=sim_ns or "", sim_us=f"{sim_ns / 1e3:.2f}" if sim_ns else "",
               sim_GBps=f"{buf / sim_ns:.2f}" if sim_ns else "", check=check, status=status)
    return row


def write_summary(rows, path):
    """The hardware summary's column layout, so the two files read side by side."""
    with open(path, "w") as f:
        f.write(f"{'case':<38}{'bytes':>11}{'oop_us':>11}{'oop_GBps':>9}{'ip_us':>11}{'ip_GBps':>9} checks\n")
        for r in rows:
            us = f"{float(r['sim_us']):.1f}" if r["sim_us"] else "-"
            gb = r["sim_GBps"] or "-"
            chk = ("ok" if r["status"] == "ok" else r["status"] if r["status"].startswith("skipped")
                   else f"{r['status']} check={r['check']}")
            f.write(f"{r['case']:<38}{r['buffer_bytes']:>11}{us:>11}{gb:>9}{'-':>11}{'-':>9} {chk}\n")


def read_hw(paths):
    hw = {}
    for p in paths:
        for line in open(p):
            f = line.split()
            if len(f) >= 4 and "@" in f[0] and f[0] != "case":
                try:
                    hw[f[0]] = float(f[2])
                except ValueError:
                    pass
    return hw


def print_compare(rows, hw):
    print("\nThe simulator runs base-2 sizes (1MB = 2^20 B per GPU). The 2026-11-25 hardware runs used\n"
          "decimal ones (10^6 B), so at MB sizes the simulator moves 4.9% more bytes, and 2.3% at KB.")
    print(f"\n{'case':<38}{'hw_us':>10}{'sim_us':>10}{'sim/hw':>8}")
    for r in rows:
        h = hw.get(r["case"])
        s = float(r["sim_us"]) if r["sim_us"] else None
        ratio = f"{s / h:.2f}" if h and s else "-"
        print(f"{r['case']:<38}{(f'{h:.1f}' if h else '-'):>10}{(f'{s:.1f}' if s else '-'):>10}{ratio:>8}")
    missing = sorted(set(hw) - {r["case"] for r in rows})
    sched_rows = [c for c in missing if not c.startswith("nccl-")]
    if sched_rows:
        print(f"\nhardware rows with no simulated case ({len(sched_rows)}): {' '.join(sched_rows)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sizes", default=",".join(SIZES_MB),
                    help=f"per-GPU chunk sizes, base 2: 0.016 or 16KB, 1 or 1MB (default {','.join(SIZES_MB)})")
    ap.add_argument("--only", help="regex on the schedule name, e.g. 'teccl-two|taccl'")
    ap.add_argument("--proto-chunk-bytes", type=int, default=PROTO_CHUNK_BYTES, metavar="B",
                    help=f"--protoChunkBytes for every run (default {PROTO_CHUNK_BYTES}, the runtime's "
                         "gridOffset step; 0 = no pipelining)")
    ap.add_argument("--fused-slices", type=int, default=FUSED_SLICES, metavar="N",
                    help=f"--fusedSlices for every run (default {FUSED_SLICES}, the kernel's recvCopySend; "
                         "1 = store-and-forward). Affects only schedules with rcs steps")
    ap.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1,
                    help="runs in parallel (default: CPU count); each is single-threaded")
    ap.add_argument("--outdir", default=os.path.join(HERE, "sweep_results", f"{PROGRAM}_hw"))
    ap.add_argument("--hw", nargs="+", default=[], metavar="SUMMARY",
                    help="hardware perf summary.txt files to compare against")
    ap.add_argument("--force", action="store_true", help="re-run cases already in results.csv")
    ap.add_argument("--dry-run", action="store_true", help="list the cases and stop")
    args = ap.parse_args()

    todo = cases([label_of(x) for x in args.sizes.split(",")], args.only)
    if not todo:
        sys.exit(f"no {PROGRAM}_<a2a|ag>_*.xml schedules in {XML_DIR} match")
    if args.dry_run:
        for case, xml, coll, mb in todo:
            ib = input_bytes(coll, mb)
            bad = unsupported(xml)
            print(f"{case:<38} --xml={xml} --coll={coll} --inputBytes={ib} "
                  f"--protoChunkBytes={args.proto_chunk_bytes} --fusedSlices={args.fused_slices}"
                  + (f"   SKIP: {','.join(bad)} not in the simulator" if bad else ""))
        print(f"{len(todo)} cases")
        return
    if not glob.glob(os.path.join(NS3_DIR, "build", "scratch", f"ns3*-{PROGRAM}-*")):
        sys.exit(f"{PROGRAM} is not built: run ./ns3 build scratch/{PROGRAM} in {NS3_DIR} first")

    os.makedirs(os.path.join(args.outdir, "runs"), exist_ok=True)
    csv_path = os.path.join(args.outdir, "results.csv")
    done = {}
    if os.path.exists(csv_path) and not args.force:
        with open(csv_path) as f:
            # a row counts as done only if it ran with this pipelining setting; rows from
            # before the column existed (no pipelining) or with another value are re-run.
            # Likewise for fused_slices, where a row from before that column ran the default.
            done = {r["case"]: r for r in csv.DictReader(f) if r["status"] == "ok"
                    and r.get("proto_chunk_bytes") == str(args.proto_chunk_bytes)
                    and (r.get("fused_slices") or str(FUSED_SLICES)) == str(args.fused_slices)}
    pending = [c for c in todo if c[0] not in done]
    print(f"{len(todo)} cases, {len(todo) - len(pending)} already in {csv_path}, "
          f"running {len(pending)} with {args.jobs} in parallel")

    results = dict(done)
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futs = {pool.submit(run_one, *c, args.outdir, args.proto_chunk_bytes, args.fused_slices): c
                for c in pending}
        for i, fut in enumerate(cf.as_completed(futs), 1):
            r = fut.result()
            results[r["case"]] = r
            print(f"[{i}/{len(pending)}] {r['case']:<38} {r['sim_us'] or '-':>10} us  {r['status']}",
                  flush=True)

    order = {c[0]: i for i, c in enumerate(todo)}
    rows = sorted((r for r in results.values() if r["case"] in order), key=lambda r: order[r["case"]])
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    write_summary(rows, os.path.join(args.outdir, "summary.txt"))
    failed = [r["case"] for r in rows if r["status"].startswith("FAILED")]
    skipped = sorted({(r["schedule"], r["status"]) for r in rows if r["status"].startswith("skipped")})
    print(f"\nsummary: {os.path.join(args.outdir, 'summary.txt')}")
    for sched, why in skipped:
        print(f"skipped {sched}: {why[len('skipped: '):]}")
    if failed:
        print(f"FAILED ({len(failed)}), logs in {os.path.join(args.outdir, 'runs')}: {' '.join(failed)}")
    if args.hw:
        print_compare(rows, read_hw(args.hw))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
