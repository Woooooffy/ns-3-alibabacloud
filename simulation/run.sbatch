#!/bin/bash
#SBATCH --job-name=ns3
#SBATCH --output=logs/ns3-%j.out
#SBATCH --error=logs/ns3-%j.err
#SBATCH --time=05:00:00
#SBATCH --nodes=1 --ntasks=1 --cpus-per-task=16 --mem=64G
#SBATCH --account=commit
#SBATCH --qos=commit-main
#SBATCH --partition=lanka-v3
#SBATCH --exclusive

# Usage: sbatch run.sbatch <scratch-target> [ns-3 args...]
#   e.g. sbatch run.sbatch rail --inputBytes=1048576 --label=with_rate
#        sbatch run.sbatch hetero_cluster --inputBytes=4194304 --label=baseline
# Runs the equivalent of: ./ns3 run "rail --inputBytes=... --label=..."

cd $SLURM_SUBMIT_DIR
mkdir -p logs

if [ $# -lt 1 ]; then
    echo "usage: sbatch run.sbatch <scratch-target> [ns-3 args...]" >&2
    exit 1
fi

TARGET=$1
shift

# Slurm resolved the --output/--error paths before this script ran, so tag the run
# with a readable alias alongside the raw ns3-<jobid> files.
LABEL=""
for arg in "$@"; do
    case "$arg" in
        --label=*) LABEL="-${arg#--label=}" ;;
    esac
done
ln -sf "ns3-${SLURM_JOB_ID}.out" "logs/${TARGET}${LABEL}-${SLURM_JOB_ID}.out"
ln -sf "ns3-${SLURM_JOB_ID}.err" "logs/${TARGET}${LABEL}-${SLURM_JOB_ID}.err"

source /data/commit/graphit/wangyj05/workspace/setup.sh

echo "=== ./ns3 run \"$TARGET $*\" ==="
srun ./ns3 run "$TARGET $*"
