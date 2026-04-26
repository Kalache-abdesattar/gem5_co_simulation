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
Minimal SE-mode CHI simulation with optional VIPController co-simulation.

Runs an x86 binary through a CHI Ruby network.  When --vip is given,
an extra VIPController node is created that opens a POSIX shared-memory
segment for Questa IPC.  Run questa_top_ipc in a separate process; it
will attach to the shm automatically.

Usage — baseline (CHI sanity check, no VIP):
    gem5.opt config/chi/chi_vip_se.py -c /path/to/binary

Usage — with VIPController IPC:
    gem5.opt config/chi/chi_vip_se.py -c /path/to/binary --vip

Arguments:
    -c / --cmd       Binary to execute (default: gem5 hello binary)
    --options        Space-separated args for the binary
    --vip            Enable VIPController node
    --shm-name       POSIX shm name passed to VIPController (default: /chi_vip_0)
    --num-cpus       CPU cores (default: 1)
    --num-l3caches   HNF / L3 slices (default: 1)
    --num-dirs       Memory controllers (default: same as --num-l3caches)
    --mem-size       DRAM size (default: 512MB)
    --cpu-type       TimingSimpleCPU | AtomicSimpleCPU | O3CPU (default: TimingSimpleCPU)
"""

import argparse
import os
import sys

# ── Path setup ────────────────────────────────────────────────────────────────
_here      = os.path.abspath(os.path.dirname(__file__))
_base_dir  = os.path.abspath(os.path.join(_here, '../../'))
_gem5_cfgs = os.path.join(_base_dir, 'gem5', 'configs')

# gem5 'configs/' must be on the path so 'common', 'ruby' etc. resolve
for _p in [_gem5_cfgs,
           os.path.join(_gem5_cfgs, 'common'),
           _base_dir]:
    if _p not in sys.path:
        sys.path.insert(0, _p)

import m5
from m5.objects import *
from m5.util import addToPath
from m5.defines import buildEnv

addToPath(_gem5_cfgs)
addToPath(os.path.join(_gem5_cfgs, 'common'))

# Force Ruby dispatch to CHI even in multi-protocol builds
buildEnv['PROTOCOL'] = 'CHI'

from common import Options, Simulation
from ruby import Ruby

# ── Argument parsing ──────────────────────────────────────────────────────────
parser = argparse.ArgumentParser(
    description='Minimal CHI SE sim with optional VIPController'
)

_hello = os.path.join(
    _base_dir, 'gem5', 'tests', 'test-progs', 'hello',
    'bin', 'x86', 'linux', 'hello'
)
parser.add_argument('-c', '--cmd', type=str, default=_hello,
                    metavar='CMD', help='Binary to run')
parser.add_argument('--options', type=str, default='',
                    help='Args for the binary (space-separated)')
parser.add_argument('--vip', action='store_true',
                    help='Enable VIPController co-simulation node')
parser.add_argument('--shm-name', type=str, default='/chi_vip_0',
                    help='POSIX shm name for VIPController')

# Standard Ruby / NoC options (defines --num-cpus, --num-dirs, --num-l3caches,
# --mem-size, --cpu-type, --topology, --network, --cacheline_size, etc.)
Options.addNoISAOptions(parser)
Ruby.define_options(parser)

args = parser.parse_args()

# CHI topology selection (Ruby.define_options may leave these as None)
if not getattr(args, 'topology', None):
    args.topology = 'Pt2Pt'
if not getattr(args, 'network', None):
    args.network = 'simple'
if not getattr(args, 'cacheline_size', None):
    args.cacheline_size = 64
# Default cache sizes (used by CHI_config defaults if not overridden)
for attr, val in [('l1d_size', '32kB'), ('l1i_size', '32kB'),
                  ('l2_size', '256kB'), ('l3_size', '4MB'),
                  ('l1d_assoc', 8), ('l1i_assoc', 8),
                  ('l2_assoc', 4), ('l3_assoc', 16)]:
    if not hasattr(args, attr) or getattr(args, attr) is None:
        setattr(args, attr, val)

# ── System ────────────────────────────────────────────────────────────────────
system = System()
system.clk_domain     = SrcClockDomain(
    clock='3GHz', voltage_domain=VoltageDomain()
)
system.mem_mode   = 'timing'
system.mem_ranges = [AddrRange(args.mem_size)]
system.cache_line_size = args.cacheline_size

# ── CPUs ──────────────────────────────────────────────────────────────────────
from common import ObjectList
CPUClass, test_mem_mode = ObjectList.cpu_list.get(args.cpu_type), 'timing'
# getCPUClass returns (class, mem_mode); use it properly
from common.Simulation import getCPUClass
CPUClass, test_mem_mode = getCPUClass(args.cpu_type)
system.mem_mode = test_mem_mode

system.cpu = [
    CPUClass(cpu_id=i, clk_domain=system.clk_domain)
    for i in range(args.num_cpus)
]

for cpu in system.cpu:
    cpu.createInterruptController()

# ── VIP hook: set _vip_shm_names before Ruby.create_system ───────────────────
if args.vip:
    system._vip_shm_names = [args.shm_name]
    print(f"[chi_vip_se] VIPController enabled: shm='{args.shm_name}'")
else:
    system._vip_shm_names = []

# ── Ruby CHI network ──────────────────────────────────────────────────────────
Ruby.create_system(args, False, system)

assert args.num_cpus == len(system.ruby._cpu_ports), \
    f"Ruby created {len(system.ruby._cpu_ports)} ports for {args.num_cpus} CPUs"

system.ruby.clk_domain = SrcClockDomain(
    clock='3GHz', voltage_domain=system.clk_domain.voltage_domain
)

for i in range(args.num_cpus):
    ruby_port = system.ruby._cpu_ports[i]
    ruby_port.connectCpuPorts(system.cpu[i])

# ── Workload ──────────────────────────────────────────────────────────────────
process = Process(pid=100)
process.executable = args.cmd
process.cwd        = os.getcwd()
process.cmd        = [args.cmd] + (args.options.split() if args.options else [])

for cpu in system.cpu:
    cpu.workload = process
    cpu.createThreads()

system.workload = SEWorkload.init_compatible(args.cmd)

# ── Simulate ──────────────────────────────────────────────────────────────────
root = Root(full_system=False, system=system)
m5.instantiate()

print(f"[chi_vip_se] Starting simulation: {args.cmd}")
exit_event = m5.simulate()
print(f"[chi_vip_se] Exited: '{exit_event.getCause()}' at tick {m5.curTick()}")

vip_nodes = getattr(system, '_vip_nodes', [])
if vip_nodes:
    print(f"[chi_vip_se] VIP nodes active: {len(vip_nodes)}")
    for i, vip in enumerate(vip_nodes):
        print(f"  VIP[{i}]: shm='{vip.shm_name}'")
