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
  - A schedule using an MSCCL step type the simulator does not implement yet (rcs, rrs, rrcs: see
    UNSUPPORTED_STEPS) is listed as skipped with the reason instead of being run. Today that is
    ForestColl, whose AllGather trees forward with rcs.

SIZES. A size S is a per-GPU chunk in decimal MB, as on the hardware, where nccl-tests' buffer is
3 x S x 10^6 bytes for both collectives (the summaries' `bytes` column). The harness's
--inputBytes is one rank's INPUT, which differs between the two:

    alltoall   a rank sends a chunk to every rank:   --inputBytes = 3 x S x 10^6
    allgather  a rank contributes one chunk:         --inputBytes =     S x 10^6, --coll=allgather

Time is the harness's "Total simulated time" (the collective's last step). Bandwidth is
computed here as bytes / time, which is nccl-tests' algbw. The harness's own "algorithm
bandwidth" line multiplies the input by the rank count, so for AllToAll it reads 3x higher.

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
SIZES_MB = ["0.016", "0.064", "0.256", "1", "4", "16", "64"]   # msccl_perf.sbatch's SIZES_MB
# MSCCL step types collectives.cc does not implement yet (it aborts on them). A schedule with any
# of them is reported as skipped, with the reason, rather than run and counted as a failure.
UNSUPPORTED_STEPS = {"rcs": "RecvCpSend", "rrs": "RecvRedSend", "rrcs": "RecvRedCpSend"}
FIELDS = ["case", "schedule", "coll", "size_mb", "hw_bytes", "input_bytes", "sim_ns", "sim_us",
          "sim_GBps", "check", "status"]


def hw_bytes(mb):
    """msccl_perf.sbatch's bytes_of: int(S * 1e6 + 0.5) * N."""
    return int(float(mb) * 1e6 + 0.5) * RANKS


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


def run_one(case, xml, coll, mb, outdir):
    chunk = int(float(mb) * 1e6 + 0.5)
    input_bytes = chunk * (RANKS if coll == "alltoall" else 1)
    row = dict(case=case, schedule=case.rsplit("@", 1)[0], coll=coll, size_mb=mb,
               hw_bytes=hw_bytes(mb), input_bytes=input_bytes, sim_ns="", sim_us="", sim_GBps="",
               check="none")
    bad = unsupported(xml)
    if bad:
        row["status"] = "skipped: no " + ",".join(f"{t} ({UNSUPPORTED_STEPS[t]})" for t in bad) \
                        + " step in the simulator"
        return row
    label = "tb_" + re.sub(r"[^A-Za-z0-9_.-]", "_", case)
    args = [PROGRAM, f"--xml={xml}", f"--coll={coll}", f"--inputBytes={input_bytes}",
            "--correctness=1", "--checkLog=minimal", "--qlenRows=0", f"--label={label}"]
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
               sim_GBps=f"{hw_bytes(mb) / sim_ns:.2f}" if sim_ns else "", check=check, status=status)
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
            f.write(f"{r['case']:<38}{r['hw_bytes']:>11}{us:>11}{gb:>9}{'-':>11}{'-':>9} {chk}\n")


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
                    help=f"per-GPU sizes in decimal MB (default {','.join(SIZES_MB)})")
    ap.add_argument("--only", help="regex on the schedule name, e.g. 'teccl-two|taccl'")
    ap.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1,
                    help="runs in parallel (default: CPU count); each is single-threaded")
    ap.add_argument("--outdir", default=os.path.join(HERE, "sweep_results", f"{PROGRAM}_hw"))
    ap.add_argument("--hw", nargs="+", default=[], metavar="SUMMARY",
                    help="hardware perf summary.txt files to compare against")
    ap.add_argument("--force", action="store_true", help="re-run cases already in results.csv")
    ap.add_argument("--dry-run", action="store_true", help="list the cases and stop")
    args = ap.parse_args()

    todo = cases(args.sizes.split(","), args.only)
    if not todo:
        sys.exit(f"no {PROGRAM}_<a2a|ag>_*.xml schedules in {XML_DIR} match")
    if args.dry_run:
        for case, xml, coll, mb in todo:
            ib = int(float(mb) * 1e6 + 0.5) * (RANKS if coll == "alltoall" else 1)
            bad = unsupported(xml)
            print(f"{case:<38} --xml={xml} --coll={coll} --inputBytes={ib}"
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
            done = {r["case"]: r for r in csv.DictReader(f) if r["status"] == "ok"}
    pending = [c for c in todo if c[0] not in done]
    print(f"{len(todo)} cases, {len(todo) - len(pending)} already in {csv_path}, "
          f"running {len(pending)} with {args.jobs} in parallel")

    results = dict(done)
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futs = {pool.submit(run_one, *c, args.outdir): c for c in pending}
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
