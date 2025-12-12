# BSD 3-Clause License

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



from m5.objects import (
    NULL,
    ClockDomain,
    RubyCache,
    RubyNetwork,
    RRIPRP,
)


from gem5.components.cachehierarchies.chi.nodes.abstract_node import AbstractNode


class SharedL2(AbstractNode):
    """An L2 slice (mid-level cache)"""

    def __init__(self, network: RubyNetwork, cache_line_size: int, config: dict, prefetcher_):
        """
        Args:
            config (dict): Configuration dictionary containing:
                {
                    "size": "512kB",
                    "assoc": 8,
                    "replacement_policy": RRIPRP(),
                    "prefetcher": None or <Prefetcher>,
                    "clusivity": {...},     # inclusive/exclusive config
                    "latencies": {...},
                    "TBE_params": {...},
                    "enable_DMT": False,
                    "enable_DCT": False,
                    "allow_SD": True,
                }
        """

        super().__init__(network, cache_line_size)


        # ---------- Cache ----------
        self.cache = RubyCache(
            size=config["size"],
            assoc=config["assoc"],
            replacement_policy=config.get("replacement_policy", RRIPRP()),
        )


        # ---------- Prefetcher ----------
        # if config.get("use_prefetcher"):
        #     self.use_prefetcher = True
        #     self.prefetcher = prefetcher_
        # else:
        self.use_prefetcher = False
        self.prefetcher = NULL


        # ---------- General Node Flags ----------
        self.is_HN = False  # L2s are not home nodes
        self.enable_DMT = config.get("enable_DMT", False)
        self.enable_DCT = config.get("enable_DCT", False)
        self.allow_SD = config.get("allow_SD", True)
        self.send_evictions = False
        self.sequencer = NULL


        # ---------- TBE Configuration ----------
        tbe_cfg = config.get("TBE_params", {})
        self.number_of_TBEs = tbe_cfg.get("number_of_TBEs", 32)
        self.number_of_repl_TBEs = tbe_cfg.get("number_of_repl_TBEs", 32)
        self.number_of_snoop_TBEs = tbe_cfg.get("number_of_snoop_TBEs", 1)
        self.number_of_DVM_TBEs = tbe_cfg.get("number_of_DVM_TBEs", 1)
        self.number_of_DVM_snoop_TBEs = tbe_cfg.get("number_of_DVM_snoop_TBEs", 1)
        self.unify_repl_TBEs = tbe_cfg.get("unify_repl_TBEs", False)


        # ---------- Clusivity Configuration ----------
        # Determines whether the cache is inclusive / exclusive / mostly-inclusive
        clusivity = config.get("clusivity", {})


        # Allocation behavior
        self.alloc_on_seq_acc = clusivity.get("alloc_on_seq_acc", True)
        self.alloc_on_seq_line_write = clusivity.get("alloc_on_seq_line_write", True)
        self.alloc_on_readshared = clusivity.get("alloc_on_readshared", True)
        self.alloc_on_readunique = clusivity.get("alloc_on_readunique", True)
        self.alloc_on_readonce = clusivity.get("alloc_on_readonce", True)
        self.alloc_on_writeback = clusivity.get("alloc_on_writeback", True)
        self.alloc_on_atomic = clusivity.get("alloc_on_atomic", True)


        # Deallocation behavior (default mostly-inclusive)
        self.dealloc_on_unique = clusivity.get("dealloc_on_unique", False)
        self.dealloc_on_shared = clusivity.get("dealloc_on_shared", False)
        self.dealloc_backinv_unique = clusivity.get("dealloc_backinv_unique", True)
        self.dealloc_backinv_shared = clusivity.get("dealloc_backinv_shared", True)


        # ---------- Latency Configuration ----------
        lat_cfg = config.get("latencies", {})
        self.read_hit_latency = lat_cfg.get("read_hit_latency", 12)
        self.read_miss_latency = lat_cfg.get("read_miss_latency", 14)
        self.atomic_op_latency = lat_cfg.get("atomic_op_latency", 12)
        self.write_fe_latency = lat_cfg.get("write_fe_latency", 12)
        self.write_be_latency = lat_cfg.get("write_be_latency", 12)
        self.fill_latency = lat_cfg.get("fill_latency", 12)
        self.snp_latency = lat_cfg.get("snp_latency", 12)
        self.snp_inv_latency = lat_cfg.get("snp_inv_latency", 12)