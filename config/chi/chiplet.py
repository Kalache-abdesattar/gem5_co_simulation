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



from itertools import chain
from typing import List

import m5
from m5.objects import (
    NULL,
    RubyPortProxy,
    RubySequencer,
    RubySystem,
    RubyNetwork,
    RubyCache,
    RRIPRP,
    AddrRange,
    StridePrefetcher,
    TaggedPrefetcher,
    BOPPrefetcher,
)
from m5.objects.SubSystem import SubSystem

from gem5.coherence_protocol import CoherenceProtocol
from gem5.utils.requires import requires

requires(coherence_protocol_required=CoherenceProtocol.CHI)

from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.abstract_cache_hierarchy import (
    AbstractCacheHierarchy,
)
from gem5.components.cachehierarchies.ruby.abstract_ruby_cache_hierarchy import (
    AbstractRubyCacheHierarchy,
)
from gem5.components.cachehierarchies.chi.nodes.abstract_node import (
    AbstractNode,
)

from gem5.components.cachehierarchies.ruby.topologies.simple_pt2pt import (
    SimplePt2Pt,
)
from gem5.components.processors.abstract_core import AbstractCore
from gem5.isas import ISA
from gem5.utils.override import overrides


from config.chi.nodes.directory import SimpleDirectory
from config.chi.nodes.dma_requestor import DMARequestor
from config.chi.nodes.memory_controller import MemoryController
from config.chi.nodes.private_l1_moesi_cache import PrivateL1MOESICache
from config.chi.nodes.shared_l2 import SharedL2
from config.chi.nodes.shared_l3 import SharedL3
from config.chi.nodes.private_l3 import PrivateL3


from config.chi.network.chi_global_noc import (
    ChiGlobalNoC,
)



class Chiplet(AbstractRubyCacheHierarchy):
    """
        A three level cache based on CHI
    """

    def __init__(
        self,
        l1_config: dict,
        l2_config: dict,
        l3_config: dict,
        cores_per_cluster: int,
        cores_per_chiplet: int
    ):
        """
        Args:
            l1_config (dict): L1 configuration (size, assoc, latencies, clusivity, etc.)
            l2_config (dict): L2 configuration (size, assoc, latencies, clusivity, etc.)
            l3_config (dict): L3 configuration (size, assoc, latencies, clusivity, etc.)
            cores_per_cluster (int): Number of cores per L2 cluster.
            enable_l1_prefetch (bool): Enables prefetchers on private L1s.
            enable_l2_prefetch (bool): Enables prefetchers on shared L2s.
        """
        super().__init__()

        self._l1_config = l1_config
        self._l2_config = l2_config
        self._l3_config = l3_config
        self._cores_per_cluster = cores_per_cluster
        self._cores_per_chiplet = cores_per_chiplet

        # Cache network reference (will be provided later by the system)
        self._network = None


    def incorporate_cache(self, board: AbstractBoard):

        # Create the Ruby System. This is a singleton that is required for
        # all ruby protocols. Must be exactly 1 in the simulation.
        # Most Ruby controllers, etc. need a pointer to this.
        self.ruby_system = RubySystem()

        num_cores = len(board.get_processor().get_cores())
        num_chiplets = num_cores // self._cores_per_chiplet

        # Ruby's global network.
        self.ruby_system.network = ChiGlobalNoC(
            self.ruby_system, 
            num_cores, 
            self._cores_per_cluster,
            board.has_dma_ports(), 
            self._cores_per_chiplet
        )

        # Network configurations
        # virtual networks: 0=request, 1=snoop, 2=response, 3=data
        self.ruby_system.number_of_virtual_networks = 4
        self.ruby_system.network.number_of_virtual_networks = 4

        # Create the coherent side of the memory controllers
        self.memory_controllers = self._create_memory_controllers(board)

        # Construct a HN-F Standalone Directory within the NoC
        # hn_directory = SimpleDirectory(
        #     network=self.ruby_system.network,
        #     cache_line_size=board.get_cache_line_size(),
        #     clk_domain=board.get_clock_domain())
        # hn_directory.ruby_system = self.ruby_system
        # hn_directory.downstream_destinations = self.memory_controllers
        
        l3_caches = []
        # Create multiple per-chiplet-private L3s
        for i in range(num_chiplets):
            if i != num_chiplets-1:
                l3_cache = PrivateL3(
                    config=self._l3_config,
                    network=self.ruby_system.network,
                    cache_line_size=board.get_cache_line_size()
                )
            else:
                l3_cache = SharedL3(
                    config=self._l3_config,
                    network=self.ruby_system.network,
                    cache_line_size=board.get_cache_line_size()
                ) 

            l3_caches.append(l3_cache)
            l3_caches[i].ruby_system = self.ruby_system
            l3_caches[i].downstream_destinations = self.memory_controllers #+ [hn_directory]

        
        num_l2_caches = num_cores // self._cores_per_cluster 
        l2_caches = [[] for _ in range(num_chiplets)]
        l2_prefetchers = [[] for _ in range(num_chiplets)]
        
        for c in range(num_chiplets):
            for i in range(num_l2_caches): 
                # Create l2_prefetchers objects to be passed to the L2 objects
                l2_prefetcher = BOPPrefetcher(latency=1, cache_snoop=False)
                l2_prefetchers[c].append(l2_prefetcher)

                # Create an L2 node 
                l2_cache = SharedL2( 
                    config=self._l2_config,
                    network=self.ruby_system.network, 
                    cache_line_size=board.get_cache_line_size(),
                    prefetcher_=l2_prefetchers[c][i]
                ) 
                l2_caches[c].append(l2_cache) 

                l2_caches[c][i].ruby_system = self.ruby_system

                # In CHI, you must explicitly set downstream controllers
                l2_caches[c][i].downstream_destinations = [l3_caches[c]]


        # Construct L1 prefetchers' dictionary lists
        l1_prefetchers = []

        for i in range(num_cores):
            l1_prefetcher_dict = {
                "L1i" : TaggedPrefetcher(degree=1, latency=1),
                "L1d" : StridePrefetcher(degree=1, latency=1)
            }
            l1_prefetchers.append(l1_prefetcher_dict)
        

        # Create one core cluster with a split I/D cache for each core
        self.core_clusters = [
            self._create_core_cluster(core, i, board, list(chain.from_iterable(l2_caches)), l1_prefetchers, self._cores_per_cluster)
            for i, core in enumerate(board.get_processor().get_cores())
        ]



        # Create the DMA Controllers, if required as in FS mode
        if board.has_dma_ports():
            dma_controllers = self._create_dma_controllers(board, l3_caches[0])
            self.ruby_system.num_of_sequencers = len(
                self.core_clusters
            ) * 2 + len(dma_controllers)
        else:
            dma_controllers = [] 
            self.ruby_system.num_of_sequencers = len(self.core_clusters) * 2


        # Connect the controllers within the network. Note that this function
        # makes assumptions on the order of the controllers. If you want to
        # use more complex topologies like mesh it would be a good idea to
        # tightly couple the network with the cache hierarchy.
        controllers = list(chain.from_iterable([(cluster.dcache, cluster.icache) for cluster in self.core_clusters])) + list(chain.from_iterable(l2_caches)) + l3_caches + self.memory_controllers + dma_controllers
        
        self.ruby_system.network.connectControllers(controllers)

        self.ruby_system.network.setup_buffers()

        # Set up a proxy port for the system_port. Used for load binaries and
        # other functional-only things.
        self.ruby_system.sys_port_proxy = RubyPortProxy()
        self.ruby_system.sys_port_proxy.ruby_system = self.ruby_system
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)



    def _create_core_cluster(
        self, core, core_num: int, board: AbstractBoard, l2_caches: List, l1_prefetchers: List[dict], cores_per_cluster: int
    ) -> SubSystem:
        """Given the core and the core number this function creates a cluster
        for the core with a split I/D cache.
        """
        # Create a cluster for each core.
        cluster = SubSystem()

        # Create the caches
        cluster.dcache = PrivateL1MOESICache(
            config=self._l1_config,
            network=self.ruby_system.network,
            core=core,
            cache_line_size=board.get_cache_line_size(),
            target_isa=board.get_processor().get_isa(),
            clk_domain=board.get_clock_domain(),
            prefetcher_=l1_prefetchers[core_num].get("L1d"),
            sc_lock_enabled_=False
        )
        cluster.icache = PrivateL1MOESICache(
            config=self._l1_config,
            network=self.ruby_system.network,
            core=core,
            cache_line_size=board.get_cache_line_size(),
            target_isa=board.get_processor().get_isa(),
            clk_domain=board.get_clock_domain(),
            prefetcher_=l1_prefetchers[core_num].get("L1i"),
            sc_lock_enabled_=False
        )

        # The sequencers are used to connect the core to the cache
        cluster.icache.sequencer = RubySequencer(
            version=core_num, dcache=NULL, clk_domain=cluster.icache.clk_domain, ruby_system=self.ruby_system
        )
        cluster.dcache.sequencer = RubySequencer(
            version=core_num,
            dcache=cluster.dcache.cache,
            clk_domain=cluster.dcache.clk_domain,
            ruby_system=self.ruby_system
        )

        # If full system, connect the IO bus to the sequencer
        if board.has_io_bus():
            cluster.dcache.sequencer.connectIOPorts(board.get_io_bus())

        cluster.dcache.ruby_system = self.ruby_system
        cluster.icache.ruby_system = self.ruby_system

        # Connect the core "classic" ports to the sequencers
        core.connect_icache(cluster.icache.sequencer.in_ports)
        core.connect_dcache(cluster.dcache.sequencer.in_ports)

        # Same thing for the page table walkers
        core.connect_walker_ports(
            cluster.dcache.sequencer.in_ports,
            cluster.icache.sequencer.in_ports,
        )

        # Connect the interrupt ports
        if board.get_processor().get_isa() == ISA.X86:
            int_req_port = cluster.dcache.sequencer.interrupt_out_port
            int_resp_port = cluster.dcache.sequencer.in_ports
            core.connect_interrupt(int_req_port, int_resp_port)
        else:
            core.connect_interrupt()

        
        # Set the downstream destinations for the caches
        l2_idx = core_num // cores_per_cluster
        
        cluster.dcache.downstream_destinations = [l2_caches[l2_idx]]
        cluster.icache.downstream_destinations = [l2_caches[l2_idx]]
        
        return cluster


    def _create_memory_controllers(
        self, board
    ):
        """This creates the CHI objects that interact with gem5's memory
        controllers
        """
        memory_controllers = []
        for rng, port in board.get_mem_ports():
            mc = MemoryController(self.ruby_system.network, rng, port)
            mc.ruby_system = self.ruby_system
            memory_controllers.append(mc)
        return memory_controllers


    def _create_dma_controllers(
        self, board, l3_cache
    ):
        dma_controllers = []
        for i, port in enumerate(board.get_dma_ports()):
            ctrl = DMARequestor(
                self.ruby_system.network,
                board.get_cache_line_size(),
                board.get_clock_domain(),
            )
            version = len(board.get_processor().get_cores()) + i
            ctrl.sequencer = RubySequencer(version=version, in_ports=port)
            ctrl.sequencer.dcache = NULL

            ctrl.ruby_system = self.ruby_system
            ctrl.sequencer.ruby_system = self.ruby_system

            ctrl.downstream_destinations = [l3_cache]

            dma_controllers.append(ctrl)

        return dma_controllers