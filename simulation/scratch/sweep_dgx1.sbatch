#!/bin/bash
#SBATCH --job-name=sweep_dgx1
#SBATCH --account=commit
#SBATCH --qos=commit-main
#SBATCH --partition=lanka-v3
#SBATCH --exclusive
#SBATCH --mem=102400
#SBATCH --time=12:00:00
#SBATCH --output=sweep_dgx1.%j.out
#SBATCH --error=sweep_dgx1.%j.err

set -e

cd "$(dirname "$0")"
bash sweep_dgx1.sh
