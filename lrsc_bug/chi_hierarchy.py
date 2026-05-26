import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from config.chi.l3_cache_hierarchy import L3CacheHierarchy


class LRSCHierarchy(L3CacheHierarchy):
    """
    L3CacheHierarchy with sc_lock_enabled=True on every dcache.

    AbstractNode sets sc_lock_enabled=False by default.  Flipping it enables
    the CHI exclusive-monitor timeout in the SLICC state machine: when a
    Store-Conditional fails, the L1 dcache will eventually relinquish the UC
    (Unique-Clean) exclusive line so that other requestors can make progress.

    Without this flag the L1 can silently hold exclusive ownership after a
    failed SC, keeping the HN-F directory's exclusive-monitor entry live
    indefinitely -- the suspected root cause of the bug we are chasing.
    """

    def _create_core_cluster(
        self, core, core_num, board, l2_caches, l1_prefetchers, cores_per_cluster
    ):
        cluster = super()._create_core_cluster(
            core, core_num, board, l2_caches, l1_prefetchers, cores_per_cluster
        )
        # Enable SC-lock timeout on the data cache only (not the icache,
        # which never issues Store-Conditionals).
        cluster.dcache.sc_lock_enabled = True
        return cluster
