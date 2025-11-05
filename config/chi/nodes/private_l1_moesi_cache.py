# Copyright (c) 2021 The Regents of the University of California
# All Rights Reserved.
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


from m5.objects import (
    NULL,
    ClockDomain,
    RubyCache,
    RubyNetwork,
)

from gem5.components.processors.abstract_core import AbstractCore
from gem5.isas import ISA

from gem5.components.cachehierarchies.chi.nodes.abstract_node import AbstractNode



class PrivateL1MOESICache(AbstractNode):
    """A private L1 cache with MOESI states"""
    def __init__(
        self,
        config: dict,
        network: RubyNetwork,
        core: AbstractCore,
        cache_line_size,
        target_isa: ISA,
        clk_domain: ClockDomain,
        prefetcher_,
        sc_lock_enabled_,
    ):
        """
        Args:
            config (dict): Configuration dictionary containing:
                {
                    "size": "32kB",
                    "assoc": 8,
                    "core": <AbstractCore>,
                    "target_isa": <ISA>,
                    "clk_domain": <ClockDomain>,
                    "prefetcher": None or <Prefetcher>,
                    "clusivity": {...},      # Allocation/deallocation policies
                    "TBE_params": {...},
                    "latencies": {...},      # Optional latencies
                }
        """

        super().__init__(network, cache_line_size)

        # ---------- Cache ----------
        self.cache = RubyCache(
            size=config["size"],
            assoc=config["assoc"],
            start_index_bit=self.getBlockSizeBits(),
        )


        # ---------- Clock domain ----------
        self.clk_domain = clk_domain

        # This should be set to true in the data cache controller to enable
        # timeouts on unique lines when a store conditional fails
        self.sc_lock_enabled = sc_lock_enabled_
        print(f"SC LOCKED? {self.sc_lock_enabled}")

        # ---------- Prefetcher ----------
        if config.get("use_prefetcher"):
            self.use_prefetcher = True
            self.prefetcher = prefetcher_
        else:
            self.use_prefetcher = False
            self.prefetcher = NULL


        # ---------- Core-specific flags ----------
        self.send_evictions = core.requires_send_evicts()


        # ---------- Home node flags ----------
        self.is_HN = False
        self.enable_DMT = False
        self.enable_DCT = False


        # ---------- Clusivity configuration (MOESI states) ----------
        clusivity = config.get("clusivity", {})
        self.allow_SD = clusivity.get("allow_SD", True)
        self.alloc_on_seq_acc = clusivity.get("alloc_on_seq_acc", True)
        self.alloc_on_seq_line_write = clusivity.get("alloc_on_seq_line_write", False)
        self.alloc_on_readshared = clusivity.get("alloc_on_readshared", True)
        self.alloc_on_readunique = clusivity.get("alloc_on_readunique", True)
        self.alloc_on_readonce = clusivity.get("alloc_on_readonce", True)
        self.alloc_on_writeback = clusivity.get("alloc_on_writeback", False)
        self.alloc_on_atomic = clusivity.get("alloc_on_atomic", False)
        self.dealloc_on_unique = clusivity.get("dealloc_on_unique", False)
        self.dealloc_on_shared = clusivity.get("dealloc_on_shared", False)
        self.dealloc_backinv_unique = clusivity.get("dealloc_backinv_unique", True)
        self.dealloc_backinv_shared = clusivity.get("dealloc_backinv_shared", True)


        # ---------- TBE parameters ----------
        tbe_cfg = config.get("TBE_params", {})
        self.number_of_TBEs = tbe_cfg.get("number_of_TBEs", 16)
        self.number_of_repl_TBEs = tbe_cfg.get("number_of_repl_TBEs", 16)
        self.number_of_snoop_TBEs = tbe_cfg.get("number_of_snoop_TBEs", 4)
        self.number_of_DVM_TBEs = tbe_cfg.get("number_of_DVM_TBEs", 16)
        self.number_of_DVM_snoop_TBEs = tbe_cfg.get("number_of_DVM_snoop_TBEs", 4)
        self.unify_repl_TBEs = tbe_cfg.get("unify_repl_TBEs", False)


        # ---------- Latency configuration ----------
        lat_cfg = config.get("latencies", {})
        self.read_hit_latency = lat_cfg.get("read_hit", 3)
        self.read_miss_latency = lat_cfg.get("read_miss", 4)
        self.atomic_op_latency = lat_cfg.get("atomic_op", 4)
        self.write_fe_latency = lat_cfg.get("write_fe", 3)
        self.write_be_latency = lat_cfg.get("write_be", 3)
        self.fill_latency = lat_cfg.get("fill", 4)
        self.snp_latency = lat_cfg.get("snp", 4)
        self.snp_inv_latency = lat_cfg.get("snp_inv", 4)
