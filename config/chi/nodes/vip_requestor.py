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


from m5.objects import (
    NULL,
    ClockDomain,
    RubyCache,
)

from gem5.components.cachehierarchies.chi.nodes.abstract_node import AbstractNode


class VIPRequestor(AbstractNode):
    """CHI RN-F proxy node for Questa co-simulation.

    This node participates in gem5's Ruby CHI network as an RN-F requestor
    whose transactions originate from an RTL DUT running in Questa vsim.
    The actual request injection is performed by questa_top (a SystemC module)
    via TLM2 b_transport calls into the CHI_VIP_Controller, which in turn
    reads/writes the Ruby message buffers that this node owns.

    Connectivity:
        gem5 Ruby network  <-- MessageBuffers  --> CHI_VIP_Controller (SC)
                                                         <-- TLM2 -->
                                                   questa_top (SC pin bridge)
                                                         <-- CHI wires -->
                                                      Questa RTL DUT (RN-F)

    Node ID:
        The version number assigned by AbstractNode.versionCount() is used as
        the MachineID.num when constructing questa_top in C++:

            MachineID vip_id  {MachineType_Cache, vip_node.version}
            MachineID hnf_id  = ... (from your downstream controller)

        Print self.version after construction to confirm the assignment.

    TBE sizing:
        number_of_TBEs and number_of_snoop_TBEs should be set to at least the
        maximum number of outstanding transactions and snoops your RTL DUT can
        have in flight simultaneously.  The defaults below are conservative;
        increase them for high-performance DUTs.

    Note on DMT/DCT:
        enable_DMT and enable_DCT are left False.  If your DUT supports Direct
        Memory Transfer or Direct Cache Transfer, set them to True here and
        ensure questa_top routes the corresponding TLM2 transactions correctly.
    """

    def __init__(self, network, cache_line_size, clk_domain: ClockDomain,
                 num_tbes: int = 256, num_snoop_tbes: int = 64):
        """
        Args:
            network:          Ruby network object (passed to AbstractNode).
            cache_line_size:  System cache-line size in bytes.
            clk_domain:       Clock domain for this controller.
            num_tbes:         Maximum outstanding request TBEs (default 256).
            num_snoop_tbes:   Maximum outstanding snoop TBEs (default 64).
        """
        super().__init__(network, cache_line_size)

        # A minimal dummy RubyCache is required by CHI_Cache_Controller even
        # for nodes with no actual cache.  size="128" is the smallest legal value.
        self.cache = RubyCache(
            dataAccessLatency=0, tagAccessLatency=1, size="128", assoc=1
        )

        self.clk_domain = clk_domain

        # ── CHI node type ─────────────────────────────────────────────────────
        # VIPRequestor acts as an RN-F: it is not a home node and does not
        # participate in direct memory or cache-to-cache transfers.
        self.is_HN        = False
        self.enable_DMT   = False
        self.enable_DCT   = False

        # ── Cache allocation policy: none ─────────────────────────────────────
        # All data passes through to the downstream HN-F / L3.  The RTL DUT
        # manages its own internal cache; gem5 only sees the CHI messages.
        self.allow_SD                  = False
        self.alloc_on_seq_acc          = False
        self.alloc_on_seq_line_write   = False
        self.alloc_on_readshared       = False
        self.alloc_on_readunique       = False
        self.alloc_on_readonce         = False
        self.alloc_on_writeback        = False
        self.alloc_on_atomic           = False
        self.dealloc_on_unique         = False
        self.dealloc_on_shared         = False
        self.dealloc_backinv_unique    = True
        self.dealloc_backinv_shared    = True

        self.send_evictions = False
        self.use_prefetcher = False
        self.prefetcher     = NULL

        # ── TBE counts ────────────────────────────────────────────────────────
        # An RTL DUT may have many simultaneous in-flight transactions.
        # Tune num_tbes to match your DUT's maximum outstanding request count.
        # num_snoop_tbes should be >= the DUT's snoop buffer depth.
        self.number_of_TBEs            = num_tbes
        self.number_of_repl_TBEs       = 1     # no eviction engine
        self.number_of_snoop_TBEs      = num_snoop_tbes
        self.number_of_DVM_TBEs        = 1     # DVM not expected from RTL DUT
        self.number_of_DVM_snoop_TBEs  = 1
        self.unify_repl_TBEs           = False
