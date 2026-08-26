#!/usr/bin/env python3
"""Sweep a dual-plane collective scratch's feature ablations over a range of message sizes.

The program to sweep is the first argument: a scratch name, or a path to its .cc.

    ./sweep_dual_plane_features.py dual_plane_hetero --start 1KB --end 64KB
    ./sweep_dual_plane_features.py rail_optimized_256gpu_dual_plane --start 1KB --end 1GB

Sizes on the command line are PER GPU PAIR: every rank sends that much to each rank, so the
scratch's --inputBytes (one rank's total input) is <ranks>x the pair size -- 1 GB per pair is
96 GB per rank on a 96-GPU scratch. The rank count is read out of the source, as is the set of
--flags the program actually accepts, so a scratch missing one of the ablation knobs still
sweeps (that knob is simply left at its own default). Start small.

Seven configurations per size: a baseline with every feature off, one run per feature turned
on alone (flow ids paired with schedule-pinned NICs, since routing by flow id says nothing
useful about connections injected on whichever NIC), one with everything on except the rate
annotations, and one with all of them on (the scratch's own defaults).

Results are appended to results.csv under a per-program output directory and re-read on
startup, so an interrupted sweep resumes where it stopped; --force re-runs anyway. Tables are
(re)printed from that file at the end, and also written to tables.md.
"""
import argparse, collections, csv, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
NS3_DIR = os.path.dirname(HERE)                    # simulation/
LOG = os.path.join(HERE, "logs")

SCRATCH_DIR = HERE                                  # simulation/scratch, where ns3 finds targets

# name -> the flags that differ from the scratch's own defaults.
# baseline is everything off; each feature run turns one feature back on, and "all" is the
# scratch's own defaults.
#
# flowId is deliberately NOT tested alone: per-flow switch forwarding only steers a packet the
# schedule already put on the right plane, so with merged NICs the routing rules would mostly
# be applied to connections injected on the wrong NIC. The flow-id run therefore carries
# --nicSel=schedule with it, and nicSel alone (schedule-pinned NICs, plain ECMP in the fabric)
# is the run that separates the two halves.
CONFIGS = collections.OrderedDict([
    ("baseline",   dict(rate=0, netDeps=0, flowId=0, nicSel="merged")),
    ("rate",       dict(rate=1, netDeps=0, flowId=0, nicSel="merged")),
    ("netDeps",    dict(rate=0, netDeps=1, flowId=0, nicSel="merged")),
    ("nicSel",     dict(rate=0, netDeps=0, flowId=0, nicSel="schedule")),
    ("flowId+nic", dict(rate=0, netDeps=0, flowId=1, nicSel="schedule")),
    # Everything on except the rate-annotated XML: isolates what the schedule's per-flow rates
    # buy once the rest of the machinery is already in force.
    ("noRate",     dict(rate=0, netDeps=1, flowId=1, nicSel="schedule")),
    ("all",        dict(rate=1, netDeps=1, flowId=1, nicSel="schedule")),
])
BASELINE = "baseline"

# Constant across every run, per the sweep's terms.
PROTO_CHUNK_BYTES = 2 * 1024 * 1024
MAX_MSGS_IN_FLIGHT = 8

FIELDS = ["pair_bytes", "config", "input_bytes", "sim_time_ns", "algbw_gbps",
          "pause", "resume", "max_qlen_bytes", "nic_mean_gbps", "nic_peak_gbps",
          "paced_pct", "unshapeable_pct", "nic_interval_ns", "qlen_rows", "wall_s"]


# ---- the program under test -------------------------------------------------------------

class Program:
    """Everything the sweep needs to know about the scratch it is driving.

    All of it is read out of the source rather than hard-coded, because the point of the
    argument is to run this against scratches with different rank counts and different
    subsets of the ablation knobs.
    """

    def __init__(self, spec, ranks=None):
        self.path = self._resolve(spec)
        self.name = os.path.splitext(os.path.basename(self.path))[0]
        # The shortcut ./ns3 run resolves to a real cmake target. Note that ./ns3 build cannot
        # take this: build passes its argument to a regex expecting an already-built path, so
        # only the run path knows how to turn scratch/<name> into a target.
        self.target = f"scratch/{self.name}"
        src = open(self.path, encoding="utf-8", errors="replace").read()

        self.flags = set(re.findall(r'cmd\.AddValue\(\s*"([A-Za-z0-9_]+)"', src))
        if "inputBytes" not in self.flags:
            raise SystemExit(
                f"{self.path} has no --inputBytes (no CommandLine at all, in the case of the\n"
                f"generated topology/output skeletons -- those build a topology and call\n"
                f"Simulator::Run with no collective installed). There is nothing to sweep.")

        self.ranks = ranks or self._count(src, "gpunodes")
        if not self.ranks:
            raise SystemExit(f"cannot find the GPU count in {self.path}; pass --ranks")
        self.nvswitches = self._count(src, "nvswtches") or 1
        # 64-bit --inputBytes is what lets a sweep past 4 GB per rank mean anything; the older
        # uint32_t declaration wraps silently, which would look like a suspiciously fast run.
        self.wide_input = bool(re.search(r"uint64_t\s+inputBytes", src))

    @staticmethod
    def _resolve(spec):
        cands = [spec] if os.sep in spec or spec.endswith(".cc") else []
        cands += [os.path.join(SCRATCH_DIR, spec + ".cc"), os.path.join(SCRATCH_DIR, spec)]
        for c in cands:
            if os.path.isfile(c):
                path = os.path.abspath(c)
                break
        else:
            raise SystemExit(f"cannot find a source for {spec!r} (looked in {SCRATCH_DIR})")
        # ns3 only builds targets under simulation/scratch, so a source living anywhere else
        # (topology/output, say) has to be copied in before it can be run at all.
        if os.path.dirname(path) != SCRATCH_DIR:
            twin = os.path.join(SCRATCH_DIR, os.path.basename(path))
            hint = f"\nA copy already exists at {twin} -- sweep that instead." \
                if os.path.isfile(twin) else \
                f"\nCopy it to {SCRATCH_DIR} first, then sweep it by name."
            raise SystemExit(f"{path} is not under {SCRATCH_DIR}, so ns3 has no target for it."
                             + hint)
        return path

    @staticmethod
    def _count(src, container):
        m = re.search(r"i\s*<\s*(\d+)\s*;.*?\b" + container + r"\.Add\(", src)
        return int(m.group(1)) if m else 0

    def off_node_fraction(self):
        """Share of a rank's input that leaves the host, for the duration estimate only.

        Peers sharing a rank's NVSwitch are reached over NVLink and never touch the fabric.
        Domains are not always equal-sized, but this only picks a sampling period.
        """
        per_domain = max(1, self.ranks // self.nvswitches)
        return max(0.1, (self.ranks - per_domain) / self.ranks)

    def filter(self, argv):
        """Drop --flags this program does not declare, so a leaner scratch still sweeps."""
        keep, dropped = [], []
        for a in argv:
            flag = a[2:].split("=")[0] if a.startswith("--") else None
            if flag and flag not in self.flags:
                dropped.append(flag)
            else:
                keep.append(a)
        return keep, dropped


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


def nic_interval_ns(input_bytes, prog, host_tx_gbps, target_samples=500):
    """Sampling period for --nicBwInterval, scaled to the run's expected length.

    A fixed period cannot serve a range this wide: 100 ns is right for a run that lasts a few
    microseconds and would emit tens of millions of rows for one that lasts a second. Estimate
    the duration from the wire bound (a rank's off-node bytes over its fabric NICs), aim for a
    few hundred samples per NIC, and snap to a 1/2/5 step.
    """
    est_ns = input_bytes * prog.off_node_fraction() / (host_tx_gbps * 1e9) * 8 * 1e9
    raw = max(100.0, est_ns / target_samples)
    mag = 10 ** int(len(str(int(raw))) - 1)
    for step in (1, 2, 5, 10):
        if raw <= step * mag:
            return int(step * mag)
    return int(10 * mag)


# ---- running -----------------------------------------------------------------------------

def run_one(pair_bytes, name, flags, args, prog, no_build):
    input_bytes = pair_bytes * prog.ranks
    label = f"sweep_{prog.name}_{name}_{fmt_size(pair_bytes)}"
    interval = nic_interval_ns(input_bytes, prog, args.host_tx_gbps)
    # The per-packet queue trace is one row per enqueue and per dequeue at every hop, so it
    # scales with the traffic and passes a gigabyte well before the top of this range. The
    # scratch tracks the per-port high-water marks either way, which is all the table needs.
    qlen_rows = 1 if input_bytes <= args.qlen_rows_max_bytes else 0
    if "qlenRows" not in prog.flags:
        qlen_rows = 1          # no knob to turn them off; the rows are written regardless

    argv = [prog.target,
            f"--inputBytes={input_bytes}", f"--label={label}", f"--coll={args.coll}",
            f"--rate={flags['rate']}", f"--netDeps={flags['netDeps']}",
            f"--flowId={flags['flowId']}", f"--nicSel={flags['nicSel']}",
            f"--protoChunkBytes={PROTO_CHUNK_BYTES}",
            f"--maxMsgsInFlight={MAX_MSGS_IN_FLIGHT}",
            f"--nicBwInterval={interval}", f"--qlenRows={qlen_rows}",
            "--checkLog=silent"]
    argv, dropped = prog.filter(argv)
    if dropped and not run_one.warned:
        print(f"  note: {prog.name} declares no " + ", ".join(f"--{d}" for d in sorted(set(dropped)))
              + " -- left at the program's own default", flush=True)
        run_one.warned = True
    # ns3 re-runs cmake and a build check before every invocation, which on a 147-run sweep
    # costs more than some of the runs themselves -- and worse, a source edit landing mid-sweep
    # would rebuild between two points and silently make them incomparable. Only the first run
    # of a sweep is allowed to build; every one after it is --no-build.
    cmd = ["./ns3", "run"] + (["--no-build"] if no_build else []) + [" ".join(argv)]

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
    row.update(pace_stats(out))
    row.update(read_traces(label, args))
    return row


run_one.warned = False


def pace_stats(out):
    """RdmaHw::PrintPaceStats, reduced to the two numbers the ablation turns on.

    paced_pct is the share of transmitted bytes whose inter-packet gap the schedule's XML rate
    actually set; it is what makes a --rate run different from a --rate=0 one, and 0 here means
    the two are the same simulation no matter what the latency column shows. unshapeable_pct is
    the share of rate-carrying messages that fit in one MTU -- those have no inter-packet gap to
    stretch, so their rate can only ever be discarded, which is the expected failure mode when a
    schedule solved for large messages is replayed at small ones.
    """
    m = re.search(r"bytes shaped by the XML rate:\s*[0-9.]+ of [0-9.]+ MB \(([0-9.]+)%\)", out)
    u = re.search(r"one MTU or less \(rate unshapeable\):\s*\d+ \(([0-9.]+)%\)", out)
    return dict(paced_pct=float(m.group(1)) if m else "",
                unshapeable_pct=float(u.group(1)) if u else "")


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


def tables(done, sweep_sizes, out, line_gbps):
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
    def paced(row, _):
        p, u = num(row, "paced_pct"), num(row, "unshapeable_pct")
        if p is None:
            return "-"
        return f"{p:.1f}" if u is None else f"{p:.1f} / {u:.0f}"

    table("Bytes shaped by the XML rate (%, paced / unshapeable messages)",
          "Left: share of transmitted bytes whose gap the schedule's `rate` actually set. Right: "
          "share of rate-carrying messages that fit in one MTU, which have no inter-packet gap to "
          "stretch. A 0 on the left means the run is identical to one with `rate` off.", paced)
    table("GPU fabric NIC bandwidth (Gbps, mean / peak per NIC)",
          "Mean is per fabric NIC over the window in which any NIC was transmitting; peak is "
          f"the busiest single sample. Line rate is {line_gbps:g}.", bw)

    text = "\n".join(lines)
    print(text)
    with open(out, "w") as f:
        f.write(text)


# ---- main --------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("program",
                    help="scratch to sweep: a name under simulation/scratch, or a path to a .cc")
    ap.add_argument("--start", type=parse_size, default="1KB",
                    help="smallest per-GPU-pair message (default 1KB)")
    ap.add_argument("--end", type=parse_size, default="1GB",
                    help="largest per-GPU-pair message, doubling from --start (default 1GB)")
    ap.add_argument("--coll", default="alltoall", choices=["alltoall", "allgather"])
    ap.add_argument("--configs", default=",".join(CONFIGS),
                    help="comma-separated subset of: " + ",".join(CONFIGS))
    ap.add_argument("--ranks", type=int,
                    help="GPUs in the topology (default: read from the source)")
    ap.add_argument("--host-tx-gbps", type=float, default=800.0,
                    help="a GPU's total fabric egress, used only to size the --nicBwInterval "
                         "sampling period (default 800 = two 400G NICs)")
    ap.add_argument("--nic-line-gbps", type=float, default=400.0,
                    help="per-NIC line rate, quoted in the bandwidth table's caption")
    ap.add_argument("--outdir",
                    help="default: sweep_results/<program> next to this script")
    ap.add_argument("--qlen-rows-max-bytes", type=parse_size, default="16MB",
                    help="keep the per-packet queue trace only while --inputBytes is at most "
                         "this (default 16MB); above it only the peak summary is written")
    ap.add_argument("--keep-traces", action="store_true",
                    help="keep the raw per-run CSVs instead of deleting them once reduced")
    ap.add_argument("--skip-build", action="store_true",
                    help="assume the target is already built, so even the first run is --no-build")
    ap.add_argument("--force", action="store_true", help="re-run points already in results.csv")
    ap.add_argument("--dry-run", action="store_true", help="print the commands and stop")
    ap.add_argument("--tables-only", action="store_true", help="reprint tables from results.csv")
    args = ap.parse_args()

    for n in args.configs.split(","):
        if n not in CONFIGS:
            raise SystemExit(f"unknown config {n!r}; pick from {','.join(CONFIGS)}")
    if args.start > args.end:
        raise SystemExit("--start is larger than --end")

    prog = Program(args.program, args.ranks)
    if not prog.wide_input and args.end * prog.ranks > (1 << 32):
        raise SystemExit(f"{prog.name} declares --inputBytes as uint32_t, which wraps at 4 GB; "
                         f"{fmt_size(args.end)}/pair is {fmt_size(args.end * prog.ranks)}/rank. "
                         f"Widen it to uint64_t first.")
    if args.outdir is None:
        args.outdir = os.path.join(HERE, "sweep_results", prog.name)

    os.makedirs(args.outdir, exist_ok=True)
    results = os.path.join(args.outdir, "results.csv")
    # Appending rows under a header from an older FIELDS would write each row's columns against
    # the wrong names, silently corrupting every earlier point too. Refuse instead.
    if os.path.exists(results):
        with open(results) as f:
            header = next(csv.reader(f), [])
        if header != FIELDS:
            raise SystemExit(
                f"{results} was written with different columns ({','.join(header)}).\n"
                f"Expected: {','.join(FIELDS)}\n"
                "Delete it or pass a fresh --outdir; the old points have to be re-run anyway.")
    sweep = sizes(args.start, args.end)
    done = load(results)

    if not args.tables_only:
        print(f"{prog.name}: {prog.ranks} GPUs, {prog.nvswitches} NVSwitches")
        print(f"{len(sweep)} sizes x {len(args.configs.split(','))} configs, "
              f"{fmt_size(args.start)}..{fmt_size(args.end)} per pair "
              f"({fmt_size(args.start * prog.ranks)}..{fmt_size(args.end * prog.ranks)} per rank)\n")
        # The first run to actually execute carries the build; the rest never rebuild.
        built = args.skip_build
        print("build: " + ("assumed current (--skip-build), every run is --no-build"
                           if built else "on the first run only, then --no-build") + "\n")
        for s in sweep:
            print(f"{fmt_size(s)}/pair -> --inputBytes={s * prog.ranks}")
            for name in args.configs.split(","):
                if (s, name) in done and not args.force:
                    print(f"  [{name:8s}] already in results.csv, skipping")
                    continue
                row = run_one(s, name, CONFIGS[name], args, prog, no_build=built)
                built = True
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
           os.path.join(args.outdir, "tables.md"), args.nic_line_gbps)
    print(f"\nresults.csv, per-run logs and check_congestion reports in {args.outdir}")


if __name__ == "__main__":
    main()
