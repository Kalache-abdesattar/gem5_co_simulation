# Copyright (c) 2022 The Regents of the Yonsei University
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

# Copyright (c) 2022 The Regents of the Yonsei University
# All rights reserved.
# (BSD 3-Clause License - See original for full text)

from typing import Optional
from m5.objects import (
    BadAddr,
    BaseXBar,
    Cache,
    L2XBar,
    Port,
    SystemXBar,
)

from gem5.isas import ISA
from gem5.utils.override import *
from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.abstract_cache_hierarchy import AbstractCacheHierarchy
from gem5.components.cachehierarchies.abstract_three_level_cache_hierarchy import AbstractThreeLevelCacheHierarchy
from gem5.components.cachehierarchies.classic.abstract_classic_cache_hierarchy import AbstractClassicCacheHierarchy
from gem5.components.cachehierarchies.classic.caches.l1dcache import L1DCache
from gem5.components.cachehierarchies.classic.caches.l1icache import L1ICache
from gem5.components.cachehierarchies.classic.caches.l2cache import L2Cache
from gem5.components.cachehierarchies.classic.caches.mmu_cache import MMUCache

class L3Cache(Cache):
    def __init__(self, size, assoc):
        super(L3Cache, self).__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 20
        self.data_latency = 20
        self.response_latency = 20
        self.mshrs = 20
        self.tgts_per_mshr = 12

class PrivateL1PrivateL2SharedL3CacheHierarchy(
    AbstractClassicCacheHierarchy, AbstractThreeLevelCacheHierarchy
):
    """
    A cache setup where each core has a private L1 and private L2.
    All private L2s connect to a shared L3 cache via a coherent bus.
    """

    def _get_default_membus(self) -> SystemXBar:
        membus = SystemXBar(width=64)
        membus.badaddr_responder = BadAddr()
        membus.default = membus.badaddr_responder.pio
        return membus

    def __init__(
        self,
        l1d_size: str,
        l1i_size: str,
        l2_size: str,
        l3_size: str,
        l1d_assoc: int = 8,
        l1i_assoc: int = 8,
        l2_assoc: int = 16,
        l3_assoc: int = 16,
        membus: Optional[BaseXBar] = None,
    ) -> None:
        AbstractClassicCacheHierarchy.__init__(self=self)
        AbstractThreeLevelCacheHierarchy.__init__(
            self,
            l1i_size=l1i_size,
            l1i_assoc=l1i_assoc,
            l1d_size=l1d_size,
            l1d_assoc=l1d_assoc,
            l2_size=l2_size,
            l2_assoc=l2_assoc,
            l3_size=l3_size,
            l3_assoc=l3_assoc,
        )
        self.membus = membus if membus else self._get_default_membus()
        

    @overrides(AbstractClassicCacheHierarchy)
    def get_mem_side_port(self) -> Port:
        return self.membus.mem_side_ports

    @overrides(AbstractClassicCacheHierarchy)
    def get_cpu_side_port(self) -> Port:
        return self.membus.cpu_side_ports

    @overrides(AbstractCacheHierarchy)
    def incorporate_cache(self, board: AbstractBoard) -> None:
        # 1. System-wide port connections
        board.connect_system_port(self.membus.cpu_side_ports)
        for _, port in board.get_mem_ports():
            self.membus.mem_side_ports = port

        num_cores = board.get_processor().get_num_cores()

        # 2. Instantiate all SimObjects
        self.l1icaches = [L1ICache(size=self._l1i_size, assoc=self._l1i_assoc) for _ in range(num_cores)]
        self.l1dcaches = [L1DCache(size=self._l1d_size, assoc=self._l1d_assoc) for _ in range(num_cores)]
        self.l2buses = [L2XBar() for _ in range(num_cores)] 
        self.l2caches = [L2Cache(size=self._l2_size, assoc=self._l2_assoc) for _ in range(num_cores)]
        self.iptw_caches = [MMUCache(size="8KiB") for _ in range(num_cores)]
        self.dptw_caches = [MMUCache(size="8KiB") for _ in range(num_cores)]

        # Shared Tier
        self.l3bus = L2XBar() 
        self.l3cache = L3Cache(size=self._l3_size, assoc=self._l3_assoc)

        if board.has_coherent_io():
            self._setup_io_cache(board)

        # 3. Connection Loop
        for i, core in enumerate(board.get_processor().get_cores()):
            # core = gem5 library SimpleCore wrapper; core.core = underlying SimObject
            
            # Core -> L1
            core.connect_icache(self.l1icaches[i].cpu_side)
            core.connect_dcache(self.l1dcaches[i].cpu_side)

            # L1s and MMU -> Private L2 Bus
            self.l1icaches[i].mem_side = self.l2buses[i].cpu_side_ports
            self.l1dcaches[i].mem_side = self.l2buses[i].cpu_side_ports
            self.iptw_caches[i].mem_side = self.l2buses[i].cpu_side_ports
            self.dptw_caches[i].mem_side = self.l2buses[i].cpu_side_ports

            core.connect_walker_ports(
                self.iptw_caches[i].cpu_side, self.dptw_caches[i].cpu_side
            )

            # Private L2 Bus -> Private L2 Cache
            self.l2caches[i].cpu_side = self.l2buses[i].mem_side_ports

            # Private L2 Cache -> SHARED L3 Bus
            self.l2caches[i].mem_side = self.l3bus.cpu_side_ports

            # --- x86 Interrupt Handling ---
            core.core.createInterruptController()
            if board.get_processor().get_isa() == ISA.X86:
                # IMPORTANT: These must be connected to the membus (SystemXBar)
                # 'pio' handles memory-mapped I/O for the APIC registers
                core.core.interrupts[0].pio = self.membus.mem_side_ports
                
                # 'int_requestor' sends interrupts out to the bus
                core.core.interrupts[0].int_requestor = self.membus.cpu_side_ports
                
                # 'int_responder' receives interrupts from the bus
                core.core.interrupts[0].int_responder = self.membus.mem_side_ports

        # 4. Final shared path: L3 Bus -> L3 Cache -> System Bus
        self.l3bus.mem_side_ports = self.l3cache.cpu_side
        self.l3cache.mem_side = self.membus.cpu_side_ports

    def _setup_io_cache(self, board: AbstractBoard) -> None:
        self.iocache = Cache(assoc=8, tag_latency=50, data_latency=50,
                             response_latency=50, mshrs=20, size="1KiB",
                             tgts_per_mshr=12, addr_ranges=board.mem_ranges)
        self.iocache.mem_side = self.membus.cpu_side_ports
        self.iocache.cpu_side = board.get_mem_side_coherent_io_port()