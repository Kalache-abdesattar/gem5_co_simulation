"""
LR/SC exclusive-monitor stress test for the gem5 CHI HN-F.

Topology
--------
  N RISC-V TIMING cores
  L1 dcache per core  (sc_lock_enabled = True)
  L1 icache per core
  L2 per core         (cores_per_cluster = 1 -> every L2 is its own cluster)
  1 L3 slice          (the HN-F / directory)
  1 DDR3 memory channel

All cores run the same RISC-V binary (lrsc_stress) which hammers
Load-Reserved / Store-Conditional on a single shared cache line.  With
sc_lock_enabled disabled (default) the L1 may silently retain exclusive
ownership after a failed SC, keeping the HN-F exclusive monitor stale and
potentially causing incorrect behaviour or a hang.

Usage
-----
    gem5/build/RISCV_CHI/gem5.opt lrsc_bug/config.py [--num-cores N]
    gem5/build/RISCV_CHI/gem5.opt --debug-flags=LLSC lrsc_bug/config.py
    gem5/build/RISCV_CHI/gem5.opt --debug-flags=RubySlicc lrsc_bug/config.py
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

from config.chi.l3_cache_hierarchy import L3CacheHierarchy
from chi_hierarchy import LRSCHierarchy

requires(coherence_protocol_required=CoherenceProtocol.CHI)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(description="LR/SC CHI HN-F stress test")
parser.add_argument(
    "--num-cores",
    type=int,
    default=4,
    help="Number of RISC-V cores competing on the shared cache line (default: 4)",
)
parser.add_argument(
    "--sc-lock-off",
    action="store_true",
    default=False,
    help="Disable sc_lock_enabled on dcaches (reproduces the bug; default: enabled)",
)
args = parser.parse_args()

# ---------------------------------------------------------------------------
# Cache configuration (kept small so the shared line stays in L1/L2 and
# all coherence traffic goes through the single L3/HN-F slice)
# ---------------------------------------------------------------------------
L1_CFG = {
    "size": "32KiB",
    "assoc": 8,
    "latencies": {
        "tag_access_Latency":  2,
        "data_access_latency": 4,
        "atomic_latency":      4,   # RubyCache.atomicLatency (hardware)
        "read_hit_latency":    4,
        "read_miss_latency":   6,
        "atomic_op_latency":   6,   # SLICC state-machine timing
        "write_fe_latency":    4,
        "write_be_latency":    4,
        "fill_latency":        6,
        "snp_latency":         5,
        "snp_inv_latency":     5,
    },
    "TBE_params": {
        "number_of_TBEs":           16,
        "number_of_repl_TBEs":      16,
        "number_of_snoop_TBEs":      4,
        "number_of_DVM_TBEs":        1,
        "number_of_DVM_snoop_TBEs":  1,
        "unify_repl_TBEs":       False,
    },
    "clusivity": {
        "allow_SD":                True,
        "alloc_on_seq_acc":        True,
        "alloc_on_seq_line_write": False,
        "alloc_on_readshared":     True,
        "alloc_on_readunique":     True,
        "alloc_on_readonce":       True,
        "alloc_on_writeback":      False,
        "alloc_on_atomic":         False,
        "dealloc_on_unique":       False,
        "dealloc_on_shared":       False,
        "dealloc_backinv_unique":  True,
        "dealloc_backinv_shared":  True,
    },
}

L2_CFG = {
    "size": "64KiB",
    "assoc": 8,
    "latencies": {
        "tag_access_Latency":  2,
        "data_access_latency": 8,
        "atomic_latency":      8,
        "read_hit_latency":    8,
        "read_miss_latency":  12,
        "atomic_op_latency":  12,
        "write_fe_latency":    8,
        "write_be_latency":    8,
        "fill_latency":       12,
        "snp_latency":        10,
        "snp_inv_latency":    10,
    },
    "TBE_params": {
        "number_of_TBEs":           16,
        "number_of_repl_TBEs":      16,
        "number_of_snoop_TBEs":      4,
        "number_of_DVM_TBEs":        1,
        "number_of_DVM_snoop_TBEs":  1,
        "unify_repl_TBEs":       False,
    },
    "clusivity": {
        "allow_SD":                True,
        "alloc_on_seq_acc":        True,
        "alloc_on_seq_line_write": False,
        "alloc_on_readshared":     True,
        "alloc_on_readunique":     True,
        "alloc_on_readonce":       True,
        "alloc_on_writeback":      False,
        "alloc_on_atomic":         False,
        "dealloc_on_unique":       False,
        "dealloc_on_shared":       False,
        "dealloc_backinv_unique":  True,
        "dealloc_backinv_shared":  True,
    },
}

L3_CFG = {
    "size": "256KiB",
    "assoc": 16,
    "latencies": {
        "tag_access_Latency":  3,
        "data_access_latency": 20,
        "atomic_latency":      20,
        "read_hit_latency":    20,
        "read_miss_latency":   40,
        "atomic_op_latency":   40,
        "write_fe_latency":    20,
        "write_be_latency":    20,
        "fill_latency":        40,
        "snp_latency":         20,
        "snp_inv_latency":     20,
    },
    "TBE_params": {
        "number_of_TBEs":           64,
        "number_of_repl_TBEs":      64,
        "number_of_snoop_TBEs":      8,
        "number_of_DVM_TBEs":        1,
        "number_of_DVM_snoop_TBEs":  1,
        "unify_repl_TBEs":       False,
    },
    "clusivity": {
        "allow_SD":                True,
        "alloc_on_seq_acc":        False,
        "alloc_on_seq_line_write": False,
        "alloc_on_readshared":     True,
        "alloc_on_readunique":     True,
        "alloc_on_readonce":       True,
        "alloc_on_writeback":      True,
        "alloc_on_atomic":         True,
        "dealloc_on_unique":       False,
        "dealloc_on_shared":       False,
        "dealloc_backinv_unique":  True,
        "dealloc_backinv_shared":  True,
    },
}

# ---------------------------------------------------------------------------
# System assembly
# ---------------------------------------------------------------------------
HierarchyClass = L3CacheHierarchy if args.sc_lock_off else LRSCHierarchy
print(
    f"[config] sc_lock_enabled = {not args.sc_lock_off} "
    f"({'bug reproduced' if args.sc_lock_off else 'fix applied'})"
)

cache_hierarchy = HierarchyClass(
    l1_config=L1_CFG,
    l2_config=L2_CFG,
    l3_config=L3_CFG,
    cores_per_cluster=1,   # one L2 per core; maximises L3/HN-F contention
    num_l3_slices=1,       # single HN-F so all cores share one directory
)

memory = SingleChannelDDR3_1600(size="512MiB")

processor = SimpleProcessor(
    cpu_type=CPUTypes.TIMING,
    isa=ISA.RISCV,
    num_cores=args.num_cores,
)

board = SimpleBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

binary_path = os.path.join(os.path.dirname(__file__), "sw", "lrsc_stress")
board.set_se_binary_workload(BinaryResource(local_path=binary_path))

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
simulator = Simulator(board=board)
simulator.run()
print(f"Done at tick {simulator.get_current_tick()}")
