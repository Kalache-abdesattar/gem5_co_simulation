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
Usage
-----
```

Script to run PARSEC benchmarks with gem5.
The script expects a benchmark program name and the simulation
size. The system is fixed with 2 CPU cores, MESI Two Level system
cache and 3 GiB DDR4 memory. It uses the x86 board.

This script will count the total number of instructions executed
in the ROI. It also tracks how much wallclock and simulated time.

Usage:
------

```
scons build/X86/gem5.opt
./build/X86/gem5.opt \
    configs/example/gem5_library/x86-parsec-benchmarks.py \
    --benchmark <benchmark_name> \
    --size <simulation_size>
```

## build/X86_CHI/gem5.opt configs/example/gem5_library/x86-parsec-benchmarks.py --benchmark blackscholes --size simsmall

```
"""


import argparse
import time

import m5
from m5.objects import Root

from gem5.components.boards.x86_board import X86Board
from gem5.components.memory import DualChannelDDR4_2400, SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import (
    obtain_resource,
    DiskImageResource,
    KernelResource,
    BootloaderResource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires
from gem5.components.processors.simple_switchable_processor import SimpleSwitchableProcessor



# Verify ISA
# We check for the required gem5 build.
requires(
    isa_required=ISA.X86,
    kvm_required=True,
)



# Following are the list of benchmark programs for parsec.

benchmark_choices = [
    "blackscholes",
    "bodytrack",
    "canneal",
    "dedup",
    "facesim",
    "ferret",
    "fluidanimate",
    "freqmine",
    "raytrace",
    "streamcluster",
    "swaptions",
    "vips",
    "x264",
]

# Following are the input size.
size_choices = ["simsmall", "simmedium", "simlarge"]


# -------------------------------------------------------
# Argument parsing
# -------------------------------------------------------
parser = argparse.ArgumentParser(description="Run RISCV Ubuntu FS simulation with CHI cache hierarchy")
parser.add_argument("--num-cores", type=int, default=4, help="Total number of cores")
parser.add_argument("--cores-per-cluster", type=int, default=1, help="Cores per CHI cluster, a cluster is the group of cores sharing an L2")
parser.add_argument(
    "--cache-class",
    type=str,
    default="chi",
    choices=["chi", "mesi-three-level", "no-cache"],
    help="Cache hierarchy class to use",
)
parser.add_argument(
    "--mem-size", type=str, default="3GiB", help="Memory size (e.g., 2GiB, 8GiB)"
)

# The arguments accepted are the benchmark name and the simulation size.

parser.add_argument(
    "--benchmark",
    type=str,
    required=True,
    help="Input the benchmark program to execute.",
    choices=benchmark_choices,
)

parser.add_argument(
    "--size",
    type=str,
    required=True,
    help="Simulation size the benchmark program.",
    choices=size_choices,
)

args = parser.parse_args()



# -------------------------------------------------------
# Cache hierarchy setup
# -------------------------------------------------------
if args.cache_class == "no-cache":
    from gem5.components.cachehierarchies.classic.no_cache import NoCache

    cache_hierarchy = NoCache()
elif args.cache_class == "chi":
    # Custom CHI-based hierarchy
    from config.chi.l3_cache_hierarchy import L3CacheHierarchy
    
    cache_hierarchy = L3CacheHierarchy(
        l1_size="16KiB",
        l1_assoc=8,
        l2_size="1MiB",
        l2_assoc=16,
        l3_size="16MiB",
        l3_assoc=32,
        cores_per_cluster=args.cores_per_cluster,
    )
elif args.cache_class == "mesi-three-level": 
    from gem5.components.cachehierarchies.ruby.mesi_three_level_cache_hierarchy import (
            MESIThreeLevelCacheHierarchy,
    )

    cache_hierarchy = MESIThreeLevelCacheHierarchy(
        l1i_size="32KiB",
        l1i_assoc=8,
        l1d_size="32KiB",
        l1d_assoc=8,
        l2_size="256KiB",
        l2_assoc=4,
        l3_size="16MiB",
        l3_assoc=16,
        num_l3_banks=1,
    )
else:
    raise ValueError(f"The cache class {args.cache_class} is not supported.")



# -------------------------------------------------------
# Memory setup
# -------------------------------------------------------
memory = SingleChannelDDR3_1600(size=args.mem_size)
# or: memory = DualChannelDDR4_2400(size="3GiB")


# -------------------------------------------------------
# Processor setup
# -------------------------------------------------------
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=args.num_cores,
)


# -------------------------------------------------------
# Default kernel arguments
# ---------------------------------------------------+----
default_args = [
    "earlyprintk=ttyS0",
    "console=ttyS0",
    "lpj=7999923",
    "root={root_value}",
    "disk_device={disk_device}",
]


# -------------------------------------------------------
# Board setup
# -------------------------------------------------------
board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    new_kernel_args=default_args
)

# -------------------------------------------------------
# Workload setup
# -------------------------------------------------------

# After the system boots, we execute the benchmark program and wait till the
# ROI `workbegin` annotation is reached (m5_work_begin()). We start collecting
# the number of committed instructions till ROI ends (marked by `workend`).
# We then finish executing the rest of the benchmark.

# Also, we sleep the system for some time so that the output is printed
# properly.


command = (
    f"cd /home/gem5/parsec-benchmark;"
    + "source env.sh;"
    + f"parsecmgmt -a run -p {args.benchmark} -c gcc-hooks -i {args.size}         -n 2;"
    + "sleep 5;"
    + "m5 exit;"
)



# obtain_resource methods download the images automatically from gem5_resource server
# the resources are downloaded by default 
board.set_kernel_disk_workload(
    kernel=obtain_resource(
        "x86-linux-kernel-4.19.83", resource_version="1.0.0"
    ),

    disk_image=obtain_resource("x86-parsec", resource_version="1.0.0"),

    readfile_contents=command,
)



# -------------------------------------------------------
# Exit event handler
# -------------------------------------------------------

# functions to handle different exit events during the simuation
def handle_workbegin():
    print("Done booting Linux")
    print("Resetting stats at the start of ROI!")
    m5.stats.reset()

    processor.switch()

    yield False


def handle_workend():
    print("Dump stats at the end of the ROI!")
    m5.stats.dump()
    yield True




# -------------------------------------------------------
# Simulator setup
# -------------------------------------------------------
simulator = Simulator(
    board=board,

    # checkpoint_path="/opt/gem5/parsec_cpt",
    on_exit_event={
        ExitEvent.WORKBEGIN: handle_workbegin(),
        ExitEvent.WORKEND: handle_workend(),
    },
)


# We maintain the wall clock time.

globalStart = time.time()

print("Running the simulation")
print("Using KVM cpu")

m5.stats.reset()

# We start the simulation
simulator.run()

print("All simulation events were successful.")

# We print the final simulation statistics.

print("Done with the simulation")
print()
print("Performance statistics:")

print("Simulated time in ROI: " + (str(simulator.get_roi_ticks()[0])))
print(
    "Ran a total of", simulator.get_current_tick() / 1e12, "simulated seconds"
)
print(
    "Total wallclock time: %.2fs, %.2f min"
    % (time.time() - globalStart, (time.time() - globalStart) / 60)
)





