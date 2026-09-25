#!/usr/bin/env bash
# Correctness gate for the partial-participation scenarios, to run BEFORE sweeping them.
#
# The sweep itself runs with --correctness=0 (the tester allocates every rank's full expected
# output, which at the top of a size range is the largest allocation in the run and measures
# nothing the tables report). So every schedule gets verified exactly once, here, at a size
# small enough to be quick and large enough that no chunk rounds to zero elements.
#
# What is actually being checked, beyond "the bytes arrived": these solves are the first ones
# where a GPU can carry threadblocks WITHOUT carrying a slice of the collective. A solver
# handed a partial problem will route a chunk through an idle GPU, and that relay runs
# send/recv steps staging through scratch while owning no input or output partition. If the
# relay were counted as a participant, every rank's output slice would land at the wrong
# offset and this check is what would catch it -- so a "verified" here is load-bearing, not a
# formality. 3A's and 2B-milp's solves both relay; 1A's, 2B-lp's and 2C's do not.
#
# Usage:  ./check_partial_scenarios.sh [pair_bytes]        (default 1 MiB per GPU pair)

set -u
cd "$(dirname "$0")"
mkdir -p logs

PAIR=${1:-1048576}
FAIL=0

# schedule stem for a scratch, mirroring mini::Options::For in each .cc
ls_stem () {
    case "$1" in
        mini_1gpu_1nic) echo mini_1g1n ;;
        mini_2gpu_1nic) echo mini_2g1n ;;
        mini_2gpu_2nic) echo mini_2g2n ;;
        *) echo "$1" ;;
    esac
}

# tag  program          participants   variants (sched suffixes; "" = the default lp solve)
run_scenario () {
    local tag=$1 prog=$2 parts=$3; shift 3
    local bytes=$(( PAIR * parts ))
    for sched in "$@"; do
        local what="${sched:-lp}"
        echo
        echo "===== $tag / $what   $prog   ${parts} participants, $(( PAIR >> 10 )) KiB per pair ====="
        local xml="xml_input/$(ls_stem "$prog")_${tag}_a2a${sched:+_$sched}.xml"
        if [ ! -f "$xml" ]; then
            echo "  SKIPPED: no $xml (run ./import_teccl_partial.py after solving it)"
            continue
        fi
        # --sched is omitted, not passed empty: ns-3 parses a string option with
        # `istringstream >> val`, which fails on "", so `--sched=` is rejected outright.
        local out
        out=$( cd .. && ./ns3 run "$prog --scenario=$tag ${sched:+--sched=$sched} --inputBytes=$bytes \
                 --correctness=1 --checkLog=minimal --qlenRows=0 --nicBwInterval=0 \
                 --label=check_${tag}${sched:+_$sched}" 2>&1 )
        echo "$out" | grep -E "Participants:|relay GPU|Total simulated time|algorithm bandwidth|verified|incorrect"
        if ! echo "$out" | grep -q "alltoall verified"; then
            echo "  !!! NOT VERIFIED"
            echo "$out" | tail -25
            FAIL=1
        fi
    done
}

run_scenario 1A mini_1gpu_1nic 2 "" milp
run_scenario 2B mini_2gpu_1nic 4 "" milp
run_scenario 2C mini_2gpu_1nic 6 ""
run_scenario 3A mini_2gpu_2nic 6 ""

echo
if [ "$FAIL" = 0 ]; then
    echo "All schedules verified. Safe to sweep:  ./sweep_dual_plane_features.py partial --start 4KB --end 64MB"
else
    echo "!!! At least one schedule did not verify -- do not sweep until it does."
fi
exit $FAIL
