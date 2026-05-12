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
parser.add_argument('--num-rnf', type=int, default=1,
                    help='Number of RTL RN-Fs (VIPController instances)')
parser.add_argument('--rnf-src-ids', type=str, default=None,
                    help='Comma-separated RTL CHI NodeIDs for each RN-F '
                         '(e.g. "0,1"); defaults to "0,1,...,num-rnf-1"')
parser.add_argument('--max-ticks', type=int, default=10_000_000_000,
                    help='Stop after this many ticks (default 10 s at 1 THz)')
Options.addNoISAOptions(parser)
Ruby.define_options(parser)
args = parser.parse_args()

# Resolve RN-F NodeID list
if args.rnf_src_ids is not None:
    _rnf_src_ids = [int(x.strip()) for x in args.rnf_src_ids.split(',')]
else:
    _rnf_src_ids = list(range(args.num_rnf))

# CHI defaults
if not getattr(args, 'topology', None):    args.topology = 'Pt2Pt'
if not getattr(args, 'network', None):     args.network = 'simple'
if not getattr(args, 'cacheline_size', None): args.cacheline_size = 64
args.num_cpus = 0    # no CPUs
# Use SimpleMemory for DRAM so we can pre-load firmware via image_file.
# DDR timing is irrelevant for CHI protocol correctness testing.
if not getattr(args, 'mem_type', None) or args.mem_type == 'DDR3_1600_8x8':
    args.mem_type = 'SimpleMemory'
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

import struct as _struct
import os as _os

# CVA6 memory layout:
#   0x80000000 — DRAM base (CVA6 bootrom jumps here)
#   0xFFF1010000 — boot ROM (CVA6 reset vector)
_DRAM_BASE    = 0x80000000
_BOOTROM_BASE = 0xFFF1010000
_BOOTROM_HEX  = '/home/dmk/fedora/work/chi-rnf/sw/bootrom.hex'
_BOOTROM_BIN  = _os.path.join(_os.path.dirname(__file__), '../../chi_vip/bootrom.bin')

# Convert hex → little-endian binary (re-generate if hex is newer)
if (not _os.path.exists(_BOOTROM_BIN) or
        _os.path.getmtime(_BOOTROM_HEX) > _os.path.getmtime(_BOOTROM_BIN)):
    with open(_BOOTROM_HEX) as _f:
        _words = [int(_l.strip(), 16) for _l in _f if _l.strip()]
    _data = _struct.pack(f'<{len(_words)}I', *_words)
    with open(_BOOTROM_BIN, 'wb') as _f:
        _f.write(_data)
    print(f"[chi_hnf_only] Generated bootrom binary: {len(_data)} bytes → {_BOOTROM_BIN}")

_bootrom_size = _os.path.getsize(_BOOTROM_BIN)
_BOOTROM_BIN  = _os.path.abspath(_BOOTROM_BIN)

system = System()
system.clk_domain    = SrcClockDomain(clock='3GHz', voltage_domain=VoltageDomain())
system.mem_mode      = 'timing'
system.mem_ranges    = [AddrRange(start=_DRAM_BASE, size='512MB')]
system.cache_line_size = args.cacheline_size

# Boot ROM — served by CHI_SNF_BootMem via SimpleMemory
system.bootmem = SimpleMemory(
    image_file=_BOOTROM_BIN,
    range=AddrRange(start=_BOOTROM_BASE, size=_bootrom_size),
)
print(f"[chi_hnf_only] Boot ROM: {_bootrom_size} bytes @ 0x{_BOOTROM_BASE:x} ({_BOOTROM_BIN})")

system._vip_shm_names   = [args.shm_name] * args.num_rnf
system._vip_rnf_src_ids = _rnf_src_ids
print(f"[chi_hnf_only] VIPController shm='{args.shm_name}'"
      f" num_rnf={args.num_rnf} rtl_src_ids={_rnf_src_ids}")

# Pass cpus=[] explicitly — no CPUs, only VIPController as requestor
Ruby.create_system(args, False, system, bootmem=system.bootmem, cpus=[])

# Pre-load DRAM firmware so CVA6 can execute after bootrom jumps to 0x80000000.
# The ELF segments (code at 0x80000000, data at 0x8000d000) are pre-extracted
# into a raw flat binary.  SimpleMemory.image_file loads it at range start.
_DRAM_FW_BIN = _os.path.join(_os.path.dirname(__file__),
                              '../../chi_vip/dram_fw.bin')
if _os.path.exists(_DRAM_FW_BIN) and hasattr(system, 'mem_ctrls') and system.mem_ctrls:
    system.mem_ctrls[0].image_file = _os.path.abspath(_DRAM_FW_BIN)
    print(f"[chi_hnf_only] DRAM firmware: {_os.path.getsize(_DRAM_FW_BIN)} bytes"
          f" @ 0x{_DRAM_BASE:x} ({_DRAM_FW_BIN})")

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
