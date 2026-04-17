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


"""
VIPHierarchy — L3CacheHierarchy extended with a Questa VIP requestor node.

Usage
-----
Add to your run script in place of (or alongside) the existing hierarchy:

    from config.chi.vip_hierarchy import VIPHierarchy

    cache_hierarchy = VIPHierarchy(
        l1_config=l1_config,
        l2_config=l2_config,
        l3_config=l3_config,
        cores_per_cluster=args.cores_per_cluster,
        num_l3_slices=args.num_l3_slices,
        num_vip_nodes=1,           # number of Questa DUTs
        vip_num_tbes=256,          # outstanding TBEs per VIP node
        vip_num_snoop_tbes=64,     # snoop TBEs per VIP node
    )

After incorporate_cache() runs, read back the VIP node IDs for questa_top:

    for i, vip in enumerate(cache_hierarchy.vip_nodes):
        print(f"VIP[{i}]: MachineType_Cache, num={vip.version}")
        print(f"  downstream (L3[0]): MachineType_Cache, num={cache_hierarchy._l3_caches[0].version}")

These MachineID values must be passed to questa_top's C++ constructor:
    questa_top qt("qt",
                  MachineID{MachineType_Cache, vip.version},
                  MachineID{MachineType_Cache, l3_slice.version},
                  &ruby_system_ptr, &network_ptr);

Integration note:
    questa_top uses the chi_vip standalone shim (no gem5 build dependency).
    The VIPRequestor Python class registers the node in gem5's Ruby network so
    the node ID is consistent.  The actual CHI_VIP_Controller C++ object is
    created in questa_cosim_top.cc's sc_main and connects to gem5's Ruby
    message buffers at simulation start.
"""


import math
from itertools import chain
from typing import List

import m5
from m5.objects import (
    NULL,
    RubyPortProxy,
    RubySequencer,
    RubySystem,
    RubyCache,
    AddrRange,
    SrcClockDomain,
    VoltageDomain,
)

from gem5.coherence_protocol import CoherenceProtocol
from gem5.utils.requires import requires

requires(coherence_protocol_required=CoherenceProtocol.CHI)

from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.ruby.abstract_ruby_cache_hierarchy import (
    AbstractRubyCacheHierarchy,
)
from gem5.components.cachehierarchies.ruby.topologies.simple_pt2pt import (
    SimplePt2Pt,
)
from gem5.isas import ISA
from gem5.utils.override import overrides

from config.chi.nodes.directory import SimpleDirectory
from config.chi.nodes.dma_requestor import DMARequestor
from config.chi.nodes.memory_controller import MemoryController
from config.chi.nodes.private_l1_moesi_cache import PrivateL1MOESICache
from config.chi.nodes.shared_l2 import SharedL2
from config.chi.nodes.shared_l3 import SharedL3
from config.chi.nodes.vip_requestor import VIPRequestor, VIPRequestorCoSim
from config.chi.network.chi_noc import ChiNoC

from m5.objects.SubSystem import SubSystem


class VIPHierarchy(AbstractRubyCacheHierarchy):
    """Three-level CHI hierarchy with one or more Questa VIP requestor nodes.

    The VIP nodes are RN-F proxies connected downstream to the L3 slices,
    just like CPU-side L2 caches.  They receive no CPU traffic — all requests
    come from the questa_top SystemC bridge connected to the Questa RTL DUT.

    After incorporate_cache():
        self.vip_nodes  — list of VIPRequestor SimObjects
        self._l3_caches — list of SharedL3 SimObjects
    """

    def __init__(
        self,
        l1_config: dict,
        l2_config: dict,
        l3_config: dict,
        cores_per_cluster: int,
        num_l3_slices: int = 1,
        num_vip_nodes: int = 1,
        vip_num_tbes: int = 256,
        vip_num_snoop_tbes: int = 64,
        vip_shm_names: list = None,
    ):
        super().__init__()

        self._l1_config          = l1_config
        self._l2_config          = l2_config
        self._l3_config          = l3_config
        self._cores_per_cluster  = cores_per_cluster
        self._num_l3_slices      = num_l3_slices
        self._num_vip_nodes      = num_vip_nodes
        self._vip_num_tbes       = vip_num_tbes
        self._vip_num_snoop_tbes = vip_num_snoop_tbes
        # shm_names[i] is the POSIX shm name for VIP node i.
        # Defaults to /chi_vip_0, /chi_vip_1, ... if not provided.
        if vip_shm_names is None:
            vip_shm_names = [f"/chi_vip_{i}" for i in range(num_vip_nodes)]
        self._vip_shm_names = vip_shm_names

        self._l3_caches = []
        self.vip_nodes  = []

    def incorporate_cache(self, board: AbstractBoard):
        self.ruby_system = RubySystem()

        num_cores     = len(board.get_processor().get_cores())
        core_period   = board.get_clock_domain().clock[0].value
        sys_freq      = 1 / (core_period * 2)

        sys_voltage_domain = VoltageDomain(voltage='1.0V')
        self.sys_clk_domain = SrcClockDomain(
            clock=f"{sys_freq}Hz",
            voltage_domain=sys_voltage_domain,
        )

        # ── Network ───────────────────────────────────────────────────────────
        # The ChiNoC is sized for CPUs + DMA ports + VIP nodes.
        # VIP nodes are counted alongside DMA controllers in the topology.
        self.ruby_system.network = ChiNoC(
            self.ruby_system,
            num_cores,
            self._cores_per_cluster,
            board.has_dma_ports(),
            self._num_l3_slices,
            clk_domain=self.sys_clk_domain,
        )

        self.ruby_system.number_of_virtual_networks = 4
        self.ruby_system.network.number_of_virtual_networks = 4

        # ── L3 slices ─────────────────────────────────────────────────────────
        board_ranges  = board.mem_ranges
        num_slices    = self._num_l3_slices
        if num_slices < 1 or (num_slices & (num_slices - 1)) != 0:
            raise ValueError(
                f"[VIPHierarchy] num_l3_slices ({num_slices}) must be a power of 2"
            )
        intlv_bits    = int(math.log2(num_slices))

        for i in range(num_slices):
            interleaved = [
                AddrRange(
                    start=r.start, end=r.end,
                    intlvHighBit=6, intlvBits=intlv_bits, intlvMatch=i,
                )
                for r in board_ranges
            ]
            l3 = SharedL3(
                config=self._l3_config,
                network=self.ruby_system.network,
                cache_line_size=board.get_cache_line_size(),
                addr_ranges=interleaved,
            )
            l3.clk_domain   = self.sys_clk_domain
            l3.ruby_system  = self.ruby_system
            self._l3_caches.append(l3)

        # ── Memory controllers ────────────────────────────────────────────────
        self.memory_controllers = self._create_memory_controllers(board)
        for l3 in self._l3_caches:
            l3.downstream_destinations = self.memory_controllers

        # ── L2 caches ─────────────────────────────────────────────────────────
        num_l2 = num_cores // self._cores_per_cluster
        l2_caches = [
            SharedL2(
                config=self._l2_config,
                network=self.ruby_system.network,
                cache_line_size=board.get_cache_line_size(),
            )
            for _ in range(num_l2)
        ]
        for l2 in l2_caches:
            l2.ruby_system             = self.ruby_system
            l2.downstream_destinations = self._l3_caches

        # ── CPU clusters ──────────────────────────────────────────────────────
        self.core_clusters = [
            self._create_core_cluster(
                core, i, board, l2_caches, self._cores_per_cluster
            )
            for i, core in enumerate(board.get_processor().get_cores())
        ]

        # ── VIP requestor nodes ───────────────────────────────────────────────
        # Use VIPRequestorCoSim (full IPC co-sim) when the gem5 build includes
        # the VIPController SimObject; fall back to VIPRequestor (standalone).
        try:
            _use_cosim = VIPRequestorCoSim is not None
        except Exception:
            _use_cosim = False

        for i in range(self._num_vip_nodes):
            shm = self._vip_shm_names[i] if i < len(self._vip_shm_names) \
                  else f"/chi_vip_{i}"
            if _use_cosim:
                try:
                    vip = VIPRequestorCoSim(
                        network=self.ruby_system.network,
                        cache_line_size=board.get_cache_line_size(),
                        ruby_system=self.ruby_system,
                        shm_name=shm,
                        num_tbes=self._vip_num_tbes,
                        num_snoop_tbes=self._vip_num_snoop_tbes,
                    )
                    vip.downstream_destinations = self._l3_caches
                    self.vip_nodes.append(vip)
                    continue
                except RuntimeError:
                    pass  # VIPController not available; fall through

            # Standalone mode
            vip = VIPRequestor(
                network=self.ruby_system.network,
                cache_line_size=board.get_cache_line_size(),
                clk_domain=self.sys_clk_domain,
                num_tbes=self._vip_num_tbes,
                num_snoop_tbes=self._vip_num_snoop_tbes,
            )
            vip.ruby_system             = self.ruby_system
            vip.downstream_destinations = self._l3_caches
            self.vip_nodes.append(vip)

        mode = "co-sim IPC" if _use_cosim else "standalone shim"
        print(f"[VIPHierarchy] {self._num_vip_nodes} VIP node(s) added ({mode}):")
        for i, vip in enumerate(self.vip_nodes):
            shm = getattr(vip, 'shm_name', self._vip_shm_names[i]
                          if i < len(self._vip_shm_names) else f"/chi_vip_{i}")
            print(f"  VIP[{i}]: MachineType_Cache, version={vip.version}"
                  f"  shm='{shm}'")
        print(f"[VIPHierarchy] L3 slices: {self._num_l3_slices}")
        for i, l3 in enumerate(self._l3_caches):
            print(f"  L3[{i}]: MachineType_Cache, version={l3.version}")

        # ── DMA controllers ───────────────────────────────────────────────────
        if board.has_dma_ports():
            dma_controllers = self._create_dma_controllers(board)
            self.ruby_system.num_of_sequencers = (
                len(self.core_clusters) * 2 + len(dma_controllers)
            )
        else:
            dma_controllers = []
            self.ruby_system.num_of_sequencers = len(self.core_clusters) * 2

        # ── Connect all controllers ────────────────────────────────────────────
        cpu_caches = list(chain.from_iterable(
            (c.dcache, c.icache) for c in self.core_clusters
        ))
        all_controllers = (
            cpu_caches
            + l2_caches
            + self._l3_caches
            + self.memory_controllers
            + dma_controllers
            + self.vip_nodes
        )
        self.ruby_system.network.connectControllers(all_controllers)
        self.ruby_system.network.setup_buffers()

        # ── System port proxy ─────────────────────────────────────────────────
        self.ruby_system.sys_port_proxy = RubyPortProxy()
        self.ruby_system.sys_port_proxy.ruby_system = self.ruby_system
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)

    # ── Private helpers (same as L3CacheHierarchy) ───────────────────────────

    def _create_core_cluster(self, core, core_num, board, l2_caches, cores_per_cluster):
        cluster = SubSystem()

        cluster.dcache = PrivateL1MOESICache(
            config=self._l1_config,
            network=self.ruby_system.network,
            core=core,
            cache_line_size=board.get_cache_line_size(),
            target_isa=board.get_processor().get_isa(),
            clk_domain=board.get_clock_domain(),
            prefetcher_=None,
            sc_lock_enabled_=False,
        )
        cluster.icache = PrivateL1MOESICache(
            config=self._l1_config,
            network=self.ruby_system.network,
            core=core,
            cache_line_size=board.get_cache_line_size(),
            target_isa=board.get_processor().get_isa(),
            clk_domain=board.get_clock_domain(),
            prefetcher_=None,
            sc_lock_enabled_=False,
        )

        cluster.dcache.sequencer = RubySequencer(
            version=core_num,
            dcache=cluster.dcache.cache,
            clk_domain=cluster.dcache.clk_domain,
            ruby_system=self.ruby_system,
        )
        cluster.icache.sequencer = RubySequencer(
            version=core_num,
            dcache=NULL,
            clk_domain=cluster.icache.clk_domain,
            ruby_system=self.ruby_system,
        )

        if board.has_io_bus():
            cluster.dcache.sequencer.connectIOPorts(board.get_io_bus())

        cluster.dcache.ruby_system = self.ruby_system
        cluster.icache.ruby_system = self.ruby_system

        core.connect_icache(cluster.icache.sequencer.in_ports)
        core.connect_dcache(cluster.dcache.sequencer.in_ports)
        core.connect_walker_ports(
            cluster.dcache.sequencer.in_ports,
            cluster.icache.sequencer.in_ports,
        )

        if board.get_processor().get_isa() == ISA.X86:
            core.connect_interrupt(
                cluster.dcache.sequencer.interrupt_out_port,
                cluster.dcache.sequencer.in_ports,
            )
        else:
            core.connect_interrupt()

        l2_idx = core_num // cores_per_cluster
        cluster.dcache.downstream_destinations = [l2_caches[l2_idx]]
        cluster.icache.downstream_destinations = [l2_caches[l2_idx]]
        return cluster

    def _create_memory_controllers(self, board):
        mcs = []
        for rng, port in board.get_mem_ports():
            mc = MemoryController(self.ruby_system.network, rng, port)
            mc.ruby_system = self.ruby_system
            mcs.append(mc)
        return mcs

    def _create_dma_controllers(self, board):
        dmas = []
        for i, port in enumerate(board.get_dma_ports()):
            ctrl = DMARequestor(
                self.ruby_system.network,
                board.get_cache_line_size(),
                board.get_clock_domain(),
            )
            version = len(board.get_processor().get_cores()) + i
            ctrl.sequencer = RubySequencer(version=version, in_ports=port)
            ctrl.sequencer.dcache       = NULL
            ctrl.ruby_system            = self.ruby_system
            ctrl.sequencer.ruby_system  = self.ruby_system
            ctrl.downstream_destinations = self._l3_caches
            dmas.append(ctrl)
        return dmas
