# BSD 3-Clause License
#
# Copyright (c) 2025 Tampere University, Finland
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
HN-F only CHI simulation — no CPUs.

gem5 runs the CHI HN-F (Ruby) + VIPController indefinitely, waiting for
Questa (questa_top_ipc) to drive transactions via the shm IPC ring buffers.
Terminates when Questa writes a sentinel address (0xDEAD_BEEF_0000) into
a ReadNoSnp request, or after --max-ticks ticks.

Usage:
    gem5.opt config/chi/chi_hnf_only.py [--shm-name /chi_vip_0] [--max-ticks N]

Run alongside Questa:
    # Terminal 1:
    gem5.opt config/chi/chi_hnf_only.py --max-ticks 10000000000

    # Terminal 2:
    vsim -c -lib questa_build/work questa_top_ipc_wrap tb_gem5_cosim ...
"""

import argparse
import os
import sys

_here      = os.path.abspath(os.path.dirname(__file__))
_base_dir  = os.path.abspath(os.path.join(_here, '../../'))
_gem5_cfgs = os.path.join(_base_dir, 'gem5', 'configs')

for _p in [_gem5_cfgs, os.path.join(_gem5_cfgs, 'common'), _base_dir]:
    if _p not in sys.path:
        sys.path.insert(0, _p)

import m5
from m5.objects import *
from m5.util import addToPath
from m5.defines import buildEnv

addToPath(_gem5_cfgs)
addToPath(os.path.join(_gem5_cfgs, 'common'))

buildEnv['PROTOCOL'] = 'CHI'

from common import Options
from ruby import Ruby

parser = argparse.ArgumentParser(description='CHI HN-F only co-sim (no CPUs)')
parser.add_argument('--shm-name', type=str, default='/chi_vip_0')
parser.add_argument('--max-ticks', type=int, default=10_000_000_000,
                    help='Stop after this many ticks (default 10 s at 1 THz)')
Options.addNoISAOptions(parser)
Ruby.define_options(parser)
args = parser.parse_args()

# CHI defaults
if not getattr(args, 'topology', None):    args.topology = 'Pt2Pt'
if not getattr(args, 'network', None):     args.network = 'simple'
if not getattr(args, 'cacheline_size', None): args.cacheline_size = 64
args.num_cpus = 0    # no CPUs
# Use 64-byte data channel so each cache line is sent in a single DAT beat.
# Default is 32 bytes (2 beats per 64-byte line), which complicates co-sim.
# Override via the chi_config mechanism.
import importlib as _il
_chi = _il.import_module('ruby.CHI_config')
_chi.NoC_Params.data_width = 64   # single-beat for 64-byte cache lines
if not getattr(args, 'num_dirs', None) or args.num_dirs < 1:
    args.num_dirs = 1
if not getattr(args, 'num_l3caches', None) or args.num_l3caches < 1:
    args.num_l3caches = 1
for attr, val in [('l1d_size','32kB'),('l1i_size','32kB'),('l2_size','256kB'),
                  ('l3_size','4MB'),('l1d_assoc',8),('l1i_assoc',8),
                  ('l2_assoc',4),('l3_assoc',16)]:
    if not hasattr(args, attr) or getattr(args, attr) is None:
        setattr(args, attr, val)

system = System()
system.clk_domain    = SrcClockDomain(clock='3GHz', voltage_domain=VoltageDomain())
system.mem_mode      = 'timing'
system.mem_ranges    = [AddrRange('512MB')]
system.cache_line_size = args.cacheline_size

system._vip_shm_names = [args.shm_name]
print(f"[chi_hnf_only] VIPController shm='{args.shm_name}'")

# Pass cpus=[] explicitly — no CPUs, only VIPController as requestor
Ruby.create_system(args, False, system, cpus=[])

system.ruby.clk_domain = SrcClockDomain(
    clock='3GHz', voltage_domain=system.clk_domain.voltage_domain)

# No CPU ports to connect.  Ruby still needs a system port for binary loading
# but in no-CPU mode we skip that.

root = Root(full_system=False, system=system)
m5.instantiate()

import time as _time

# Grant gem5 time in small slices so we stay roughly wall-clock synchronised
# with Questa.  Without CPUs, gem5 would race through billions of idle ticks
# in milliseconds, finishing before Questa connects.
#
# Each slice = GRANT_PS ps of simulation time.  After each slice we sleep
# SLEEP_S seconds in real time so Questa can catch up.
GRANT_PS  = 1_000_000_000   # 1 µs per grant (at 1 THz = 1M ticks)
SLEEP_S   = 0.001            # 1 ms real-time sleep between grants

print(f"[chi_hnf_only] Running up to {args.max_ticks} ticks — waiting for Questa")
ticks_left = args.max_ticks
while ticks_left > 0:
    grant = min(GRANT_PS, ticks_left)
    exit_event = m5.simulate(grant)
    ticks_left -= grant
    if exit_event.getCause() != 'simulate() limit reached':
        break
    _time.sleep(SLEEP_S)

print(f"[chi_hnf_only] Exited: '{exit_event.getCause()}' at tick {m5.curTick()}")
