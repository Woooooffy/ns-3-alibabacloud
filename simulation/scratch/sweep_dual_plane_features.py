#!/usr/bin/env python3
"""Sweep dual_plane_hetero's feature ablations over a range of alltoall message sizes.

Sizes on the command line are PER GPU PAIR: every rank sends that much to each of the 96
ranks, so the scratch's --inputBytes (one rank's total input) is 96x the pair size, and the
top of the range -- 1 GB per pair -- is 96 GB per rank, 9.2 TB moved. Start small.

    ./sweep_dual_plane_features.py --start 1KB --end 64KB          # a quick shakeout
    ./sweep_dual_plane_features.py --start 1KB --end 1GB           # the real thing

Six configurations per size: a baseline with every feature off, one run per feature turned
on alone, and one with all of them on (the scratch's own defaults).

Results are appended to results.csv in the output directory and re-read on startup, so an
interrupted sweep resumes where it stopped; --force re-runs anyway. Tables are (re)printed
from that file at the end, and also written to tables.md.
"""
import argparse, collections, csv, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
NS3_DIR = os.path.dirname(HERE)                    # simulation/
LOG = os.path.join(HERE, "logs")

# 96 GPUs in dual_plane_hetero, so a rank's total input is 96x the per-pair message.
RANKS = 96
# Fraction of a rank's input that leaves the host: 88 of its 96 peers are off-node (the other
# 8 share its NVSwitch). Used only to guess a run's duration when picking a sampling period.
OFF_NODE = 88.0 / 96.0
# Two 400G fabric NICs per GPU.
HOST_TX_BPS = 2 * 400e9

# name -> the flags that differ from the scratch's own defaults.
# baseline is everything off; each feature run flips exactly one knob back on.
CONFIGS = collections.OrderedDict([
    ("baseline", dict(rate=0, netDeps=0, flowId=0, nicSel="merged")),
    ("rate",     dict(rate=1, netDeps=0, flowId=0, nicSel="merged")),
    ("netDeps",  dict(rate=0, netDeps=1, flowId=0, nicSel="merged")),
    ("flowId",   dict(rate=0, netDeps=0, flowId=1, nicSel="merged")),
    ("nicSel",   dict(rate=0, netDeps=0, flowId=0, nicSel="schedule")),
    ("all",      dict(rate=1, netDeps=1, flowId=1, nicSel="schedule")),
])
BASELINE = "baseline"

# Constant across every run, per the sweep's terms.
PROTO_CHUNK_BYTES = 2 * 1024 * 1024
MAX_MSGS_IN_FLIGHT = 8

FIELDS = ["pair_bytes", "config", "input_bytes", "sim_time_ns", "algbw_gbps",
          "pause", "resume", "max_qlen_bytes", "nic_mean_gbps", "nic_peak_gbps",
          "nic_interval_ns", "qlen_rows", "wall_s"]


# ---- sizes -------------------------------------------------------------------------------

def parse_size(text):
    m = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*([KMG]?)B?\s*", text, re.I)
    if not m:
        raise argparse.ArgumentTypeError(f"cannot read size {text!r} (try 1KB, 4MB, 1GB)")
    return int(float(m.group(1)) * {"": 1, "K": 1 << 10, "M": 1 << 20, "G": 1 << 30}[m.group(2).upper()])


def fmt_size(n):
    for unit, div in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= div:
            v = n / div
            return f"{v:.0f}{unit}" if v == int(v) else f"{v:g}{unit}"
    return f"{n}B"


def sizes(start, end):
    out, s = [], start
    while s <= end:
        out.append(s)
        s *= 2
    return out


def nic_interval_ns(input_bytes, target_samples=500):
    """Sampling period for --nicBwInterval, scaled to the run's expected length.

    A fixed period cannot serve a range this wide: 100 ns is right for a run that lasts a few
    microseconds and would emit tens of millions of rows for one that lasts a second. Estimate
    the duration from the wire bound (a rank's off-node bytes over its two 400G NICs), aim for
    a few hundred samples per NIC, and snap to a 1/2/5 step.
    """
    est_ns = input_bytes * OFF_NODE / HOST_TX_BPS * 8 * 1e9
    raw = max(100.0, est_ns / target_samples)
    mag = 10 ** int(len(str(int(raw))) - 1)
    for step in (1, 2, 5, 10):
        if raw <= step * mag:
            return int(step * mag)
    return int(10 * mag)


# ---- running -----------------------------------------------------------------------------

def run_one(pair_bytes, name, flags, args):
    input_bytes = pair_bytes * RANKS
    label = f"sweep_{name}_{fmt_size(pair_bytes)}"
    interval = nic_interval_ns(input_bytes)
    # The per-packet queue trace is one row per enqueue and per dequeue at every hop, so it
    # scales with the traffic and passes a gigabyte well before the top of this range. The
    # scratch tracks the per-port high-water marks either way, which is all the table needs.
    qlen_rows = 1 if input_bytes <= args.qlen_rows_max_bytes else 0

    argv = [f"scratch/dual_plane_hetero",
            f"--inputBytes={input_bytes}", f"--label={label}", f"--coll={args.coll}",
            f"--rate={flags['rate']}", f"--netDeps={flags['netDeps']}",
            f"--flowId={flags['flowId']}", f"--nicSel={flags['nicSel']}",
            f"--protoChunkBytes={PROTO_CHUNK_BYTES}",
            f"--maxMsgsInFlight={MAX_MSGS_IN_FLIGHT}",
            f"--nicBwInterval={interval}", f"--qlenRows={qlen_rows}",
            "--checkLog=silent"]
    cmd = ["./ns3", "run", " ".join(argv)]

    print(f"  [{name:8s}] {' '.join(argv[1:])}", flush=True)
    if args.dry_run:
        return None

    t0 = time.time()
    proc = subprocess.run(cmd, cwd=NS3_DIR, capture_output=True, text=True)
    wall = time.time() - t0
    out = proc.stdout + proc.stderr
    if proc.returncode != 0:
        sys.stderr.write(out[-4000:])
        raise SystemExit(f"run failed ({name}, {fmt_size(pair_bytes)}): exit {proc.returncode}")
    with open(os.path.join(args.outdir, f"run_{label}.log"), "w") as f:
        f.write(out)

    sim = re.search(r"Total simulated time:\s*(\d+)", out)
    if not sim:
        sys.stderr.write(out[-4000:])
        raise SystemExit(f"no simulated time reported ({name}, {fmt_size(pair_bytes)})")
    algbw = re.search(r"algorithm bandwidth:\s*([0-9.eE+-]+)", out)

    row = dict(pair_bytes=pair_bytes, config=name, input_bytes=input_bytes,
               sim_time_ns=int(sim.group(1)),
               algbw_gbps=float(algbw.group(1)) if algbw else "",
               nic_interval_ns=interval, qlen_rows=qlen_rows, wall_s=round(wall, 1))
    row.update(read_traces(label, args))
    return row


# ---- trace reduction ---------------------------------------------------------------------

def read_traces(label, args):
    """The three trace-derived columns, plus check_congestion.py's own report for the record."""
    m = {}
    m.update(pfc_counts(label))
    m["max_qlen_bytes"] = peak_qlen(label)
    m.update(nic_bw(label))

    # Run the shipped classifier too, so each sweep point keeps the human-readable breakdown
    # (which link class paused, how lopsided the NIC pair was) next to the reduced numbers.
    cc = subprocess.run([sys.executable, os.path.join(HERE, "check_congestion.py"), label],
                        cwd=HERE, capture_output=True, text=True)
    with open(os.path.join(args.outdir, f"congestion_{label}.txt"), "w") as f:
        f.write(cc.stdout + cc.stderr)

    if not args.keep_traces:
        # The raw CSVs are the bulk of the sweep's disk footprint and everything the tables
        # need has now been read out of them.
        for pat in ("switch_qlen_%s.csv", "host_nic_bw_%s.csv"):
            path = os.path.join(LOG, pat % label)
            if os.path.exists(path):
                os.remove(path)
    return m


def pfc_counts(label):
    path = os.path.join(LOG, f"switch_events_{label}.csv")
    n = collections.Counter()
    if os.path.exists(path):
        for row in csv.DictReader(open(path)):
            n[row["op"]] += 1
    return dict(pause=n["pause"], resume=n["resume"])


def peak_qlen(label):
    """Deepest egress queue reached anywhere in the network at any time, in bytes."""
    path = os.path.join(LOG, f"switch_qlen_max_{label}.csv")
    if os.path.exists(path):
        return max((int(r["max_qlen_bytes"]) for r in csv.DictReader(open(path))), default=0)
    path = os.path.join(LOG, f"switch_qlen_{label}.csv")   # older runs: reduce the rows
    if os.path.exists(path):
        return max((int(r["qlen_bytes"]) for r in csv.DictReader(open(path))), default=0)
    return ""


def nic_bw(label):
    """What a GPU's fabric NICs actually achieved, in Gbps per NIC.

    mean is over the window in which any fabric NIC was transmitting -- averaging over the
    whole run would just re-measure the run's length, which the latency column already gives.
    peak is the busiest single sample any one NIC reached, against a 400 Gbps line rate.
    """
    path = os.path.join(LOG, f"host_nic_bw_{label}.csv")
    if not os.path.exists(path):
        return dict(nic_mean_gbps="", nic_peak_gbps="")
    per_t = collections.defaultdict(float)      # time -> summed fabric bytes
    nics, peak = set(), 0.0
    for row in csv.DictReader(open(path)):
        if row["kind"] != "fabric":
            continue
        t, b, g = int(row["time_ns"]), int(row["bytes"]), float(row["gbps"])
        nics.add((row["node_id"], row["port_id"]))
        per_t[t] += b
        if g > peak:
            peak = g
    active = [t for t, b in per_t.items() if b > 0]
    if not active or not nics:
        return dict(nic_mean_gbps="", nic_peak_gbps="")
    total_bytes = sum(per_t.values())
    # Samples are right-edge labelled, so the busy window starts one period before the first
    # non-empty sample.
    period = min((b - a for a, b in zip(sorted(per_t), sorted(per_t)[1:])), default=0)
    span_ns = max(active) - min(active) + (period or 1)
    mean = total_bytes * 8 / (span_ns * 1e-9) / len(nics) / 1e9
    return dict(nic_mean_gbps=round(mean, 2), nic_peak_gbps=round(peak, 2))


# ---- results file ------------------------------------------------------------------------

def load(path):
    if not os.path.exists(path):
        return {}
    done = {}
    for row in csv.DictReader(open(path)):
        done[(int(row["pair_bytes"]), row["config"])] = row
    return done


def append(path, row):
    new = not os.path.exists(path)
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, FIELDS)
        if new:
            w.writeheader()
        w.writerow(row)


# ---- tables ----------------------------------------------------------------------------

def num(row, key):
    try:
        return float(row[key])
    except (TypeError, ValueError, KeyError):
        return None


def tables(done, sweep_sizes, out):
    names = list(CONFIGS)
    lines = []

    def table(title, note, cell):
        lines.append(f"### {title}\n")
        if note:
            lines.append(note + "\n")
        lines.append("| size/pair | " + " | ".join(names) + " |")
        lines.append("|---:|" + "---:|" * len(names))
        for s in sweep_sizes:
            cells = []
            for n in names:
                row = done.get((s, n))
                cells.append(cell(row, done.get((s, BASELINE))) if row else "-")
            lines.append(f"| {fmt_size(s)} | " + " | ".join(cells) + " |")
        lines.append("")

    def latency(row, base):
        t = num(row, "sim_time_ns")
        if t is None:
            return "-"
        us = f"{t / 1e3:,.1f}"
        b = num(base, "sim_time_ns") if base else None
        if not b or row["config"] == BASELINE:
            return us
        return f"{us} ({(b - t) / b * 100:+.1f}%)"

    def pfc(row, _):
        p, r = num(row, "pause"), num(row, "resume")
        if p is None:
            return "-"
        return f"{int(p):,}" if p == r else f"{int(p):,}/{int(r):,}"

    def qmax(row, _):
        v = num(row, "max_qlen_bytes")
        return "-" if v is None else f"{v / 1e3:,.1f}"

    def bw(row, _):
        m, pk = num(row, "nic_mean_gbps"), num(row, "nic_peak_gbps")
        return "-" if m is None else f"{m:.1f} / {pk:.0f}" if pk is not None else f"{m:.1f}"

    table("Latency", "Simulated completion time in us; in parentheses, improvement over the "
                     "baseline column (positive = faster).", latency)
    table("PFC pause / resume frames",
          "Count over the whole run. A single number means pause and resume agreed; "
          "`pause/resume` shows them separately when they did not.", pfc)
    table("Peak queue depth (KB)",
          "Deepest egress queue reached on any switch port, at any instant, anywhere in the "
          "network.", qmax)
    table("GPU fabric NIC bandwidth (Gbps, mean / peak per NIC)",
          "Mean is per fabric NIC over the window in which any NIC was transmitting; peak is "
          "the busiest single sample. Line rate is 400.", bw)

    text = "\n".join(lines)
    print(text)
    with open(out, "w") as f:
        f.write(text)


# ---- main --------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--start", type=parse_size, default="1KB",
                    help="smallest per-GPU-pair message (default 1KB)")
    ap.add_argument("--end", type=parse_size, default="1GB",
                    help="largest per-GPU-pair message, doubling from --start (default 1GB)")
    ap.add_argument("--coll", default="alltoall", choices=["alltoall", "allgather"])
    ap.add_argument("--configs", default=",".join(CONFIGS),
                    help="comma-separated subset of: " + ",".join(CONFIGS))
    ap.add_argument("--outdir", default=os.path.join(HERE, "sweep_results"))
    ap.add_argument("--qlen-rows-max-bytes", type=parse_size, default="16MB",
                    help="keep the per-packet queue trace only while --inputBytes is at most "
                         "this (default 16MB); above it only the peak summary is written")
    ap.add_argument("--keep-traces", action="store_true",
                    help="keep the raw per-run CSVs instead of deleting them once reduced")
    ap.add_argument("--force", action="store_true", help="re-run points already in results.csv")
    ap.add_argument("--dry-run", action="store_true", help="print the commands and stop")
    ap.add_argument("--tables-only", action="store_true", help="reprint tables from results.csv")
    args = ap.parse_args()

    for n in args.configs.split(","):
        if n not in CONFIGS:
            raise SystemExit(f"unknown config {n!r}; pick from {','.join(CONFIGS)}")
    if args.start > args.end:
        raise SystemExit("--start is larger than --end")

    os.makedirs(args.outdir, exist_ok=True)
    results = os.path.join(args.outdir, "results.csv")
    sweep = sizes(args.start, args.end)
    done = load(results)

    if not args.tables_only:
        print(f"{len(sweep)} sizes x {len(args.configs.split(','))} configs, "
              f"{fmt_size(args.start)}..{fmt_size(args.end)} per pair "
              f"({fmt_size(args.start * RANKS)}..{fmt_size(args.end * RANKS)} per rank)\n")
        for s in sweep:
            print(f"{fmt_size(s)}/pair -> --inputBytes={s * RANKS}")
            for name in args.configs.split(","):
                if (s, name) in done and not args.force:
                    print(f"  [{name:8s}] already in results.csv, skipping")
                    continue
                row = run_one(s, name, CONFIGS[name], args)
                if row is None:
                    continue
                append(results, row)
                done[(s, name)] = {k: str(v) for k, v in row.items()}
                print(f"  [{name:8s}] {row['sim_time_ns'] / 1e3:,.1f} us"
                      f"  pfc {row['pause']}/{row['resume']}"
                      f"  qmax {row['max_qlen_bytes']} B"
                      f"  nic {row['nic_mean_gbps']} Gbps"
                      f"  ({row['wall_s']:.0f}s wall)", flush=True)
        if args.dry_run:
            return

    print()
    tables(done, [s for s in sweep if any((s, n) in done for n in CONFIGS)],
           os.path.join(args.outdir, "tables.md"))
    print(f"\nresults.csv, per-run logs and check_congestion reports in {args.outdir}")


if __name__ == "__main__":
    main()
