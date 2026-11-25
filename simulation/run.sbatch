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
#
# Set NO_BUILD=1 to skip the build step, e.g.
#   NO_BUILD=1 sbatch run.sbatch 3nodes
# Syncing scratch/*.cc invalidates CMake's CONFIGURE_DEPENDS glob ("-- GLOB mismatch!"),
# and the next ./ns3 run reconfigures and relinks every scratch target -- minutes to
# hours of build inside what should be a short job. Build once interactively after a
# sync (./ns3 build <target>), then submit runs with NO_BUILD=1.

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

BUILD_FLAG=""
if [ "${NO_BUILD:-0}" != "0" ]; then
    BUILD_FLAG="--no-build"
fi

echo "=== ./ns3 run $BUILD_FLAG \"$TARGET $*\" ==="
# stdout/stderr are files now, so the simulator's streams would be block-buffered and
# the logs would stay empty until the job ends. stdbuf's LD_PRELOAD is inherited by
# ./ns3's child process, so line buffering reaches the actual ns-3 binary.
srun --unbuffered stdbuf -oL -eL ./ns3 run $BUILD_FLAG "$TARGET $*"
