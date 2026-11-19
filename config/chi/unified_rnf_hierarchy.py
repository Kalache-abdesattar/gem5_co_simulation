"""
L3 hierarchy where each core's L1I, L1D and L2 are one UnifiedRNF.

Same L3 slices (SharedL3 home nodes), memory controllers and network shape
as L3CacheHierarchy, minus the per-core Ruby L1/L2 controllers and their
sequencers: the CPU calls its UnifiedRNF directly (IntervalCPU.fast_mem),
and the UnifiedRNF speaks CHI to the L3 over the Ruby network. Each RN-F
router links to every L3 router, as the L2 routers do in ChiNoC.

Only IntervalCPU supports the direct interface.
"""

import math

from m5.objects import (
    AddrRange,
    MessageBuffer,
    RRIPRP,
    RubyPortProxy,
    RubySystem,
    SimpleExtLink,
    SimpleIntLink,
    SimpleNetwork,
    SrcClockDomain,
    Switch,
    TreePLRURP,
    UnifiedRNF,
    VoltageDomain,
)

from gem5.components.cachehierarchies.chi.nodes.abstract_node import (
    AbstractNode,
)
from gem5.components.cachehierarchies.ruby.abstract_ruby_cache_hierarchy import (
    AbstractRubyCacheHierarchy,
)

from config.chi.nodes.memory_controller import MemoryController
from config.chi.nodes.shared_l3 import SharedL3


class UnifiedNoC(SimpleNetwork):
    """One router per controller; RN-F <-> every L3, L3 <-> memory."""

    def __init__(self, ruby_system, num_rnfs, num_l3_slices, clk_domain,
                 rnf_clk_domain=None):
        super().__init__()
        self._rnf_clk_domain = rnf_clk_domain
        self.ruby_system = ruby_system
        self.clk_domain = clk_domain
        self._num_rnfs = num_rnfs
        self._num_l3 = num_l3_slices
        self.netifs = []

    def connectControllers(self, controllers):
        """controllers = rnfs + l3 slices + memory controllers, in order."""
        self.routers = [
            Switch(router_id=i, ext_routing_latency=1, int_routing_latency=1)
            for i in range(len(controllers))
        ]
        # as ChiNoC with core-clocked L1/L2 routers: the RN-F's router (and so
        # its input link) runs on the core clock, its uplinks on the NoC's
        if self._rnf_clk_domain is not None:
            for r in range(self._num_rnfs):
                self.routers[r].clk_domain = self._rnf_clk_domain
        self.ext_links = [
            SimpleExtLink(link_id=i, ext_node=c, int_node=self.routers[i])
            for i, c in enumerate(controllers)
        ]
        links = []

        def link(a, b, dst_clock=False):
            links.append(SimpleIntLink(link_id=len(links) + 1,
                                       src_node=self.routers[a],
                                       dst_node=self.routers[b],
                                       use_dst_clock=dst_clock))

        l3_0 = self._num_rnfs
        mem_0 = l3_0 + self._num_l3
        for r in range(self._num_rnfs):
            for j in range(self._num_l3):
                link(r, l3_0 + j, self._rnf_clk_domain is not None)
                link(l3_0 + j, r)
        for j in range(self._num_l3):
            for m in range(mem_0, len(controllers)):
                link(l3_0 + j, m)
                link(m, l3_0 + j)
        self.int_links = links


def _rnf_queues(rnf, network):
    for name in ("reqOut", "rspOut", "snpOut", "datOut"):
        buf = MessageBuffer()
        buf.out_port = network.in_port
        setattr(rnf, name, buf)
    for name in ("reqIn", "rspIn", "snpIn", "datIn"):
        buf = MessageBuffer()
        buf.in_port = network.out_port
        setattr(rnf, name, buf)


class UnifiedRNFHierarchy(AbstractRubyCacheHierarchy):
    """
    l1_config / l2_config: the dicts replay_chi.py builds for the Ruby
    L1/L2 (size, assoc, TBE_params); rnf_timing: UnifiedRNF latency
    parameters (name -> cycles) overriding the defaults.
    """

    def __init__(self, l1_config, l2_config, l3_config, num_l3_slices=1,
                 rnf_timing=None, sample_period=0, l1l2_link_clock="noc"):
        super().__init__()
        self._l1 = l1_config
        self._l2 = l2_config
        self._l3_config = l3_config
        self._num_l3_slices = num_l3_slices
        self._timing = rnf_timing or {}
        self._sample_period = sample_period
        # "core": the RN-F's router runs at the core clock, matching
        # L3CacheHierarchy(l1l2_link_clock="core"); "noc": at the NoC clock
        assert l1l2_link_clock in ("noc", "core")
        self._l1l2_link_clock = l1l2_link_clock

    def incorporate_cache(self, board):
        self.ruby_system = RubySystem()
        cores = board.get_processor().get_cores()
        num_cores = len(cores)
        core_clk = board.get_clock_domain()
        sys_period = core_clk.clock[0].value * 2      # as L3CacheHierarchy
        self.sys_clk_domain = SrcClockDomain(
            clock=f"{1 / sys_period}Hz", voltage_domain=VoltageDomain())

        self.ruby_system.network = UnifiedNoC(
            self.ruby_system, num_cores, self._num_l3_slices,
            clk_domain=self.sys_clk_domain,
            rnf_clk_domain=(core_clk if self._l1l2_link_clock == "core"
                            else None))
        self.ruby_system.number_of_virtual_networks = 4
        self.ruby_system.network.number_of_virtual_networks = 4
        network = self.ruby_system.network

        n = self._num_l3_slices
        if not (n > 0 and (n & (n - 1)) == 0):
            raise ValueError("num_l3_slices must be a power of 2")
        intlv_bits = int(math.log2(n))
        l3s = []
        for i in range(n):
            ranges = [AddrRange(start=r.start, end=r.end, intlvHighBit=6,
                                intlvBits=intlv_bits, intlvMatch=i)
                      for r in board.mem_ranges]
            l3 = SharedL3(config=self._l3_config, network=network,
                          cache_line_size=board.get_cache_line_size(),
                          addr_ranges=ranges)
            l3.clk_domain = self.sys_clk_domain
            l3.ruby_system = self.ruby_system
            l3s.append(l3)
        self.l3_caches = l3s

        l1_tbe = self._l1.get("TBE_params", {})
        l2_tbe = self._l2.get("TBE_params", {})
        rnfs = []
        for i, core in enumerate(cores):
            rnf = UnifiedRNF()
            rnf.version = AbstractNode.versionCount()
            rnf.ruby_system = self.ruby_system
            rnf.clk_domain = core_clk
            rnf.transitions_per_cycle = 1024
            rnf.data_channel_size = l3s[0].data_channel_size
            _rnf_queues(rnf, network)
            rnf.l1d_size = self._l1["size"]
            rnf.l1d_assoc = self._l1["assoc"]
            rnf.l1i_size = self._l1["size"]
            rnf.l1i_assoc = self._l1["assoc"]
            rnf.l2_size = self._l2["size"]
            rnf.l2_assoc = self._l2["assoc"]
            # same policies as the Ruby caches: RubyCache default for the
            # L1s, SharedL2's RRIP for the L2
            rnf.l1d_replacement_policy = TreePLRURP(
                num_leaves=self._l1["assoc"])
            rnf.l1i_replacement_policy = TreePLRURP(
                num_leaves=self._l1["assoc"])
            rnf.l2_replacement_policy = self._l2.get(
                "replacement_policy", RRIPRP())
            rnf.num_l1d_tbes = l1_tbe.get("number_of_TBEs", 8)
            rnf.num_l1i_tbes = l1_tbe.get("number_of_TBEs", 8)
            rnf.num_req_tbes = l2_tbe.get("number_of_TBEs", 8)
            rnf.num_repl_tbes = l2_tbe.get("number_of_repl_TBEs", 8)
            rnf.num_snp_tbes = l2_tbe.get("number_of_snoop_TBEs", 4)
            rnf.allow_SD = self._l2.get("allow_SD", False)
            for k, v in self._timing.items():
                setattr(rnf, k, v)
            rnf.sample_period = self._sample_period
            rnf.downstream_destinations = l3s
            rnfs.append(rnf)
        # parent them here first: assigning an orphan to a parameter would
        # make the CPU its parent
        self.rnfs = rnfs
        for core, rnf in zip(cores, rnfs):
            core.get_simobject().fast_mem = rnf

        mcs = []
        for rng, port in board.get_mem_ports():
            mc = MemoryController(network, rng, port)
            mc.ruby_system = self.ruby_system
            mcs.append(mc)
        self.memory_controllers = mcs
        for l3 in l3s:
            l3.downstream_destinations = mcs

        self.ruby_system.num_of_sequencers = num_cores
        network.connectControllers(rnfs + l3s + mcs)
        network.setup_buffers()

        self.ruby_system.sys_port_proxy = RubyPortProxy()
        self.ruby_system.sys_port_proxy.ruby_system = self.ruby_system
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)
