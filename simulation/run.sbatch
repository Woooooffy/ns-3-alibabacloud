#!/bin/bash
#SBATCH --job-name=ns3
#SBATCH --output=/dev/null
#SBATCH --error=/dev/null
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

# The #SBATCH --output/--error paths are resolved before this script runs, so they
# can't contain the target name -- they point at /dev/null and we redirect to the
# real, named files here instead. One pair of files per job, named after the run.
LABEL=""
for arg in "$@"; do
    case "$arg" in
        --label=*) LABEL="-${arg#--label=}" ;;
    esac
done
RUN="logs/${TARGET}${LABEL}-${SLURM_JOB_ID}"
exec > "${RUN}.out" 2> "${RUN}.err"

source /data/commit/graphit/wangyj05/workspace/setup.sh
conda activate new_ns3

echo "=== ./ns3 run \"$TARGET $*\" ==="
srun ./ns3 run "$TARGET $*"
