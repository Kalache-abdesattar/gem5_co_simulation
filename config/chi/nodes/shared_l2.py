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
    """An L2 slice"""

    def __init__(
        self,
        size: str,
        assoc: int,
        network: RubyNetwork,
        cache_line_size: int,
    ):
        super().__init__(network, cache_line_size)

        self.cache = RubyCache(
            size=size,
            assoc=assoc,
            # Can choose any replacement policy
            replacement_policy=RRIPRP(),
        )


        # Only used for L1 controllers
        self.send_evictions = False
        self.sequencer = NULL

        # No prefetcher (home nodes don't support prefetchers right now)
        self.use_prefetcher = False
        self.prefetcher = NULL

        # Set up home node that allows three hop protocols
        self.is_HN = False 
        self.enable_DMT = False
        self.enable_DCT = False
        self.allow_SD = True


        # Some reasonable default TBE params
        self.number_of_TBEs = 32
        self.number_of_repl_TBEs = 32
        self.number_of_snoop_TBEs = 1
        self.number_of_DVM_TBEs = 1  # should not receive any dvm
        self.number_of_DVM_snoop_TBEs = 1  # should not receive any dvm
        self.unify_repl_TBEs = False

        # MOESI / Mostly inclusive for shared / Exclusive for unique
        self.alloc_on_seq_acc = True
        self.alloc_on_seq_line_write = True
        self.alloc_on_readshared = True
        self.alloc_on_readunique = True
        self.alloc_on_readonce = True
        self.alloc_on_writeback = True
        self.alloc_on_atomic = True
        
        ## Avoid conflicting “alloc & dealloc on same request”
        self.dealloc_on_unique       = False
        self.dealloc_on_shared       = False

        ## Enforce inclusion via child evictions/downgrades (back-inv)
        self.dealloc_backinv_unique  = True
        self.dealloc_backinv_shared  = True
        
        # Latencies 
        self.read_hit_latency = 12
        self.read_miss_latency = 14
        self.atomic_op_latency = 12
        self.write_fe_latency = 12  # Front-end: Rcv req -> Snd req
        self.write_be_latency = 12  # Back-end: Rcv ack -> Snd data
        self.fill_latency = 12
        self.snp_latency = 12
        self.snp_inv_latency = 12     
