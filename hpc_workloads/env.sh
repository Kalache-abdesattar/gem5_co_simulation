# Tool locations for hpcw.py. Source or edit; every value can be overridden
# from the environment.
#
#   GEM5_DIR     gem5 tree with build/X86_CHI/gem5.opt (IntervalCPU built in)
#   PIN          Intel Pin launcher
#   ETRACE       etrace pintool (pintools/etrace, built with make)
#   UOPTABLE     gem5 micro-op table for etrace (gen_gem5_uops.py output)
#   CONV_PY      python with protobuf for configs/trace_sim/etrace_to_elastic.py
#   PB2_DIR      generated protobuf modules for the converter
#   INSCOUNT     Pin instruction-count tool (./hpcw.py count)
#   TRACE_ROOT   where traces go: they are large, keep them off /tmp

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export GEM5_DIR=${GEM5_DIR:-$REPO/gem5}
export PIN=${PIN:-/home/dmk/fedora/pin/pin}
export ETRACE=${ETRACE:-/home/dmk/fedora/pintools/etrace/obj-intel64/etrace.so}
export UOPTABLE=${UOPTABLE:-/home/dmk/fedora/pintools/etrace/gem5_uops.tbl}
export CONV_PY=${CONV_PY:-/home/dmk/fedora/pintools/pbenv/venv/bin/python}
export PB2_DIR=${PB2_DIR:-/home/dmk/fedora/pintools/pbenv/pb2}
export INSCOUNT=${INSCOUNT:-/home/dmk/fedora/pintools/bench/obj-intel64/inscount0.so}
export TRACE_ROOT=${TRACE_ROOT:-/home/dmk/fedora/pintools/tracewk/hpc}
