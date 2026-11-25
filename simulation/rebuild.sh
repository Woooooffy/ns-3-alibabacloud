#!/usr/bin/env bash
# Clean rebuild against the conda toolchain. Works in any worktree: every path below is
# either relative to this script or comes from $CONDA_PREFIX.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

if [[ -z "${CONDA_PREFIX:-}" ]]; then
    echo "CONDA_PREFIX is empty -- activate the env first (conda activate new_ns3)." >&2
    echo "Without it the -D flags below expand to /include and /lib and the build" >&2
    echo "silently picks up the system libxml2." >&2
    exit 1
fi

echo "tree:   $PWD"
echo "branch: $(git rev-parse --abbrev-ref HEAD)"
echo "conda:  $CONDA_PREFIX"

export PKG_CONFIG_PATH="$CONDA_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LIBRARY_PATH="$CONDA_PREFIX/lib:${LIBRARY_PATH:-}"

./ns3 clean
./ns3 configure -d="${1:-debug}" -- \
  -DLIBXML2_INCLUDE_DIR="$CONDA_PREFIX/include/libxml2" \
  -DLIBXML2_LIBRARY="$CONDA_PREFIX/lib/libxml2.so" \
  -DCMAKE_CXX_FLAGS="-I$CONDA_PREFIX/include" \
  -DCMAKE_C_FLAGS="-I$CONDA_PREFIX/include" \
  -DCMAKE_INSTALL_RPATH="$CONDA_PREFIX/lib" \
  -DCMAKE_BUILD_RPATH="$CONDA_PREFIX/lib"
./ns3 build
