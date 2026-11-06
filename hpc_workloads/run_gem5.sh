#!/bin/bash
# Run workloads in gem5: IntervalCPU (from the traces) and the O3 reference,
# then compare them and check the program outputs.
#   ./run_gem5.sh [--size tiny|small] [--mem ruby|classic] [-j N] WL...
#   INTERVAL_ONLY=1 ./run_gem5.sh ...   skip O3, compare and check
set -e
H="$(dirname "$0")"
"$H/hpcw.py" interval "$@"
[ -n "${INTERVAL_ONLY:-}" ] && exit 0
"$H/hpcw.py" o3 "$@"
"$H/hpcw.py" compare "$@"
"$H/hpcw.py" check "$@"
