#!/bin/bash
# int_simdev.sh <run-name> <app>:<ncores> ...   re-run IntervalCPU only
NAME=${1:?run name}; shift
G=/home/dmk/fedora/gem5_co_simulation/gem5; T=/home/dmk/fedora/pintools/tracewk/parsec
cd $G || exit 1
for an in "$@"; do
  a=${an%%:*}; n=${an##*:}; R=$T/$a/simdev
  build/X86_CHI/gem5.opt --outdir=$R/$NAME configs/trace_sim/replay_chi.py \
    --trace-dir $R/elastic --num-cores $n --cpu-model interval --sync \
    > $R/$NAME.log 2>&1 &
done
wait
