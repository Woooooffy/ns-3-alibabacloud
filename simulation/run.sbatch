#!/bin/bash
#SBATCH --job-name=ns3
#SBATCH --output=logs/teccl-%j.out
#SBATCH --error=logs/teccl-%j.err
#SBATCH --time=05:00:00
#SBATCH --nodes=1 --ntasks=1 --cpus-per-task=16 --mem=64G
#SBATCH --account=commit
#SBATCH --qos=commit-main
#SBATCH --partition=lanka-v3
#SBATCH --exclusive

cd $SLURM_SUBMIT_DIR 
mkdir -p logs

source /data/commit/graphit/wangyj05/workspace/setup.sh

srun ./ns3 run [file, args]
