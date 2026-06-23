#!/bin/bash
# Sweep dgx1 simulation over input sizes from 0.75KB to 786432KB, doubling each step.
# Records simulated time (ns) for each input size to an output file.

set -e

cd "$(dirname "$0")/.."

OUTPUT_FILE="scratch/dgx1_sweep_results.txt"
START_BYTES=$((768))      # 0.75KB
END_BYTES=$((3 * 512 * 1024 * 1024))  # 1.5GB

> "$OUTPUT_FILE"
echo "input_bytes simulated_time_ns" >> "$OUTPUT_FILE"

input_bytes=$START_BYTES
while [ "$input_bytes" -le "$END_BYTES" ]; do
    echo "Running dgx1 with inputBytes=$input_bytes"
    output=$(./ns3 run "scratch/dgx1_tmp --inputBytes=$input_bytes" 2>&1)
    sim_time=$(echo "$output" | grep "Total simulated time:" | grep -o '[0-9]\+' | head -1)
    echo "$input_bytes $sim_time" >> "$OUTPUT_FILE"
    input_bytes=$((input_bytes * 2))
done

echo "Results written to $OUTPUT_FILE"
