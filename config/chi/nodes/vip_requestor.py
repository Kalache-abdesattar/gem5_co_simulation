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
    MessageBuffer,
    RubyCache,
    RubyNetwork,
)

from gem5.components.cachehierarchies.chi.nodes.abstract_node import AbstractNode

# Try to import the real co-simulation controller (only present when gem5 was
# built with BUILD_VIP=1 / CHI_VIP_IPC_PATH set).
try:
    from m5.objects.VIPController import VIPController as _VIPController
    _HAS_VIP_CONTROLLER = True
except ImportError:
    _HAS_VIP_CONTROLLER = False


# ─────────────────────────────────────────────────────────────────────────────
# Standalone mode (no gem5 build dependency)
# ─────────────────────────────────────────────────────────────────────────────

class VIPRequestor(AbstractNode):
    """CHI RN-F proxy node for the chi_vip standalone bridge.

    Registers in the Ruby CHI network so MachineID version numbers are
    consistent.  The actual CHI_VIP_Controller runs inside questa_cosim_top.cc
    and reads/writes the Ruby MessageBuffers through the shared shim headers.

    Use VIPRequestorCoSim for the full gem5 ↔ Questa IPC co-simulation.
    """

    def __init__(self, network, cache_line_size, clk_domain: ClockDomain,
                 num_tbes: int = 256, num_snoop_tbes: int = 64):
        super().__init__(network, cache_line_size)

        self.cache = RubyCache(
            dataAccessLatency=0, tagAccessLatency=1, size="128", assoc=1
        )
        self.clk_domain = clk_domain

        self.is_HN        = False
        self.enable_DMT   = False
        self.enable_DCT   = False

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

        self.number_of_TBEs            = num_tbes
        self.number_of_repl_TBEs       = 1
        self.number_of_snoop_TBEs      = num_snoop_tbes
        self.number_of_DVM_TBEs        = 1
        self.number_of_DVM_snoop_TBEs  = 1
        self.unify_repl_TBEs           = False


# ─────────────────────────────────────────────────────────────────────────────
# Full co-simulation mode (requires BUILD_VIP=1 gem5 build)
# ─────────────────────────────────────────────────────────────────────────────

if _HAS_VIP_CONTROLLER:

    class VIPRequestorCoSim(_VIPController):
        """CHI RN-F proxy node for the full gem5 ↔ Questa IPC co-simulation.

        Inherits VIPController (CHIGenericController subclass) and sets up
        the 8 CHIGenericController MessageBuffers connected to the Ruby network.
        Also assigns a unique version number from the AbstractNode version counter.

        Usage:
            node = VIPRequestorCoSim(
                network        = ruby_system.network,
                cache_line_size = board.get_cache_line_size(),
                ruby_system    = ruby_system,
                shm_name       = "/chi_vip_0",
            )
        """

        _version = 0

        @classmethod
        def versionCount(cls):
            cls._version += 1
            return cls._version - 1

        def __init__(self, network: RubyNetwork, cache_line_size: int,
                     ruby_system, shm_name: str = "/chi_vip_0",
                     num_tbes: int = 256, num_snoop_tbes: int = 64):
            super().__init__()

            self.version    = VIPRequestorCoSim.versionCount()
            self.shm_name   = shm_name
            self.ruby_system = ruby_system

            # CHIGenericController needs data_channel_size.
            self.data_channel_size = cache_line_size

            # ── MessageBuffers ─────────────────────────────────────────────────
            self.reqOut = MessageBuffer()
            self.rspOut = MessageBuffer()
            self.snpOut = MessageBuffer()
            self.datOut = MessageBuffer()
            self.reqIn  = MessageBuffer()
            self.rspIn  = MessageBuffer()
            self.snpIn  = MessageBuffer()
            self.datIn  = MessageBuffer()
            self.reqOut.out_port = network.in_port
            self.rspOut.out_port = network.in_port
            self.snpOut.out_port = network.in_port
            self.datOut.out_port = network.in_port
            self.reqIn.in_port   = network.out_port
            self.rspIn.in_port   = network.out_port
            self.snpIn.in_port   = network.out_port
            self.datIn.in_port   = network.out_port

            # Generous TBE counts for a high-performance RTL DUT.
            self.transitions_per_cycle = 1024

            print(f"[VIPRequestorCoSim] version={self.version} shm='{shm_name}'")

else:
    # Provide a stub so imports always succeed; raise at instantiation time.
    class VIPRequestorCoSim:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs):
            raise RuntimeError(
                "VIPRequestorCoSim requires gem5 built with BUILD_VIP=1. "
                "Set CHI_VIP_IPC_PATH=/path/to/chi_vip/ipc when running scons."
            )
