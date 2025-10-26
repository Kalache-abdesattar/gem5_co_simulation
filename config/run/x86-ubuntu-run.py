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

Run full-system Ubuntu simulation with configurable CPU type, cache hierarchy, and checkpoints.

Usage
-----
```


# Default (4 cores, 1 per cluster, 16GiB memory)
    .gem5/build/X86_CHI/gem5.opt \
        gem5_co_simulation/config/run/riscv-ubuntu-run.py

# Custom cores and memory
    .gem5/build/X86_CHI/gem5.opt \
        gem5_co_simulation/config/run/x86-ubuntu-run.py \
        --num-cores 8 \
        --cores-per-cluster 2 \
        --mem-size 8GiB
        --disk-image gem5_co_simulation/images/disk/x86-ubuntu-22.04-custom.img

# Run and save a checkpoint at the end
    .gem5/build/X86_CHI/gem5.opt \
        gem5_co_simulation/config/run/x86-ubuntu-run.py \
        --save-checkpoint \
        --checkpoint-path gem5_co_simulation/checkpoints/checkpoints/ubuntu_boot

# Load from a checkpoint
    .gem5/build/X86_CHI/gem5.opt \
        configs/example/gem5_library/riscv-ubuntu-run.py \
        --load-checkpoint \
        --checkpoint-path gem5_co_simulation/checkpoints/ubuntu_boot

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

import os 


# PATHS
BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../"))
CHECKPOINT_DEFAULT = os.path.join(BASE_DIR, "checkpoints", "riscv_ubuntu_checkpoint")
DISK_IMAGE_DEFAULT = os.path.join(BASE_DIR, "images", "disk", "x86-ubuntu-22.04-img")
KERNEL_DEFAULT = os.path.join(BASE_DIR, "images", "kernel", "x86-linux-6.8.0-52")


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
    "--cpu-type",
    type=str,
    default="timing",
    choices=["timing", "o3", "minor"],
    help="Type of CPU model to use: timing (TimingSimpleCPU), o3 (O3CPU), or minor (MinorCPU)",
)

parser.add_argument(
    "--mem-size", type=str, default="3GiB", help="Memory size (e.g., 2GiB, 8GiB)"
)

parser.add_argument(
    "--disk-image",
    type=str,
    default=DISK_IMAGE_DEFAULT,
    help="Path to the root disk image",
)
parser.add_argument("--kernel", type=str, default=KERNEL_DEFAULT, help="Path to the kernel image")

parser.add_argument("--save-checkpoint", action="store_true",
                    help="Save a checkpoint at the end of the simulation")
parser.add_argument("--load-checkpoint", action="store_true",
                    help="Load from an existing checkpoint instead of booting fresh")
parser.add_argument("--checkpoint-path", type=str, default=CHECKPOINT_DEFAULT,
                    help="Path to the checkpoint directory")


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
if args.cpu_type == "timing":
    core_type = CPUTypes.TIMING
elif args.cpu_type == "o3":
    core_type = CPUTypes.O3
elif args.cpu_type == "minor":
    core_type = CPUTypes.MINOR
else:
    raise ValueError(f"Unsupported CPU type: {args.cpu_type}")


processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=core_type,
    isa=ISA.X86,
    num_cores=args.num_cores,
)

# -------------------------------------------------------
# Default kernel arguments
# ---------------------------------------------------+----
kernel_args = [
    "root=/dev/sda2",
    "device=/dev/sda",
    "console=ttyS0",
    "earlyprintk=ttyS0",
    "mce=off",           # << add this
    #"no_systemd=true",
    "interactive=true",
]


# -------------------------------------------------------
# Board setup
# -------------------------------------------------------
board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    new_kernel_args=kernel_args
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



board.set_kernel_disk_workload(
    kernel=KernelResource(
        args.kernel
    ),

    disk_image=DiskImageResource(
        local_path=args.disk_image
    ),

    readfile_contents=(),
)


# -------------------------------------------------------
# Exit event handler
# -------------------------------------------------------

if args.load_checkpoint:
    def exit_event_handler():

        print("Second exit: Entering benchamrk ROI")

        print("Switching from Atomic Cores to SimpleTiming Cores...")
        processor.switch()

        yield False
        
        print("Third exit: Final Exit")
        yield True

else:
    def exit_event_handler():
        print("First exit: kernel booted")

        if args.save_checkpoint:
            print(f"Saving checkpoint to {args.checkpoint_path} ...")
            simulator.save_checkpoint(args.checkpoint_path)
        else:
            print("Checkpoint saving disabled (skipping).")

        yield False

        print("Second exit: finished executing after_boot.sh")

        yield False 

        print("Third exit: Entering benchamrk ROI")

        print("Switching from Atomic Cores to SimpleTiming Cores...")
        processor.switch()

        yield False
        
        print("Third exit: Final Exit")
        yield True




# -------------------------------------------------------
# Simulator setup
# -------------------------------------------------------
if args.load_checkpoint:
    print(f"Loading checkpoint from {args.checkpoint_path} ...")
    simulator = Simulator(
        board=board,
        checkpoint_path=args.checkpoint_path,
        on_exit_event={ExitEvent.EXIT: exit_event_handler()},
    )
else:
    simulator = Simulator(
        board=board,
        on_exit_event={ExitEvent.EXIT: exit_event_handler()},
    )



simulator.run()




