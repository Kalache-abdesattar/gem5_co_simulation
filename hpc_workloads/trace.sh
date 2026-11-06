#!/bin/bash
# Capture IntervalCPU traces: Pin + etrace (gem5 micro-ops), converted to
# elastic traces in $TRACE_ROOT/<wl>/<size>/elastic.
#   ./trace.sh [--size tiny|small] [-j N] WL...
exec "$(dirname "$0")/hpcw.py" trace "$@"
