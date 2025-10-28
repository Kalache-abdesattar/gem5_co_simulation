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


#!/usr/bin/env python3
import json
import sys
import argparse
import os 

import matplotlib.pyplot as plt
import numpy as np



def scalar_stats(component, id_):
    cache_hit_accesses = component["cache"]["m_demand_hits"]["value"]
    cache_miss_accesses = component["cache"]["m_demand_misses"]["value"]
    total_accesses = cache_hit_accesses + cache_miss_accesses

    hit_ratio = cache_hit_accesses / (total_accesses)

    component_name = component["name"]   

    if component_name == "downstream_destinations":
        component_name = "l2_cache" 

        
    print(f"\n\n ====== {component_name} Cache Component Stats ======")
    print(f"{component_name}_{id_} Total Accesses: {total_accesses}")
    print(f"{component_name}_{id_} Cache Hit percentage: {hit_ratio * 100}%")

    
# parses and plots delay histograms for a CHI transaction
# dumps the images as a png to plot_dir 
def get_transaction_hist(transaction, component, id_, plot_dir):
    # hist_name = "outTransLatHist.SendReadShared"
    component_name = component["name"]
    hist_name = transaction

    hist = component.get(hist_name, None)
    if not hist or hist.get("type") != "Distribution":
        print(f"Histogram Stats for {hist_name} are not available")
        return 

    # Extract bins and counts
    num_bins = int(hist["num_bins"])
    bin_size = float(hist["bin_size"])
    counts = [hist["value"][str(i)]["value"] for i in range(num_bins)]
    edges = np.arange(0, num_bins * bin_size, bin_size)

    # Plot
    plt.figure(figsize=(8, 5))
    plt.bar(edges, counts, width=bin_size * 0.9, align='edge', edgecolor='black')
    plt.title(f"{component_name} {id_} Cache {hist_name}")
    plt.xlabel(f"Latency (cycles, bin size = {bin_size})")
    plt.ylabel("Count")
    plt.grid(axis="y", linestyle="--", alpha=0.7)

    # Save
    safe_name = hist_name.replace("::", "_").replace("/", "_")
    os.makedirs(plot_dir, exist_ok=True)
    plot_path = os.path.join(plot_dir, f"{component_name}_{id_}_{safe_name}.png")
    plt.savefig(plot_path)
    plt.close()
    print(f"Saved histogram: {plot_path}")




def main():
    parser = argparse.ArgumentParser(
        description="Parse and visualize cache statistics from gem5 JSON stats."
    )
    parser.add_argument(
        "--stats-path",
        type=str,
        required=True,
        help="Path to the gem5 JSON stats file (e.g., /path/to/stats.json)",
    )
    parser.add_argument(
        "--plot-dir",
        type=str,
        default="./plots",
        help="Directory to save histogram plots (default: ./plots)",
    )

    args = parser.parse_args()


    # Load JSON stats
    with open(args.stats_path, "r") as f:
        stats = json.load(f)

    # Print top-level keys for debugging
    print(stats.keys())

    # Extract top-level metrics
    sim_seconds = stats["simTicks"]["value"] / stats["simFreq"]["value"]
    sim_insts = stats["simInsts"]["value"]

    print("\n ====== Simulation Runtime Stats ======")
    print(f"Simulated seconds: {sim_seconds}")
    print(f"Simulated instructions: {sim_insts}")

    # Navigate into nested structure
    board = stats["board"]
    cache_hierarchy = board.get("cache_hierarchy", {})


    l3_cache = cache_hierarchy["l3cache"]

    scalar_stats(l3_cache, None)

    get_transaction_hist("outTransLatHist.SendReadNoSnp", l3_cache, None, args.plot_dir)


    clusters = cache_hierarchy["core_clusters"]
    ruby_system = cache_hierarchy["ruby_system"]
    cluster_values = clusters["value"] 


    # print((cluster_values[0])["dcache"]["cache"]["m_demand_hits"]["value"])
    # Iterate through all core clusters
    for cluster_idx, cluster in enumerate(cluster_values):
        cache_component = cluster.get("icache", None)
        scalar_stats(cache_component, cluster_idx)

        cache_component = cluster.get("dcache", None)
        scalar_stats(cache_component, cluster_idx)

        # L2 is named L1.downstream in CHI Ruby 
        # We use either an L1i or L1d 's downstream pointer to access the corresponding L2
        l2_cache = cache_component["downstream_destinations"]["value"][0]
        scalar_stats(l2_cache, cluster_idx)

        get_transaction_hist("outTransLatHist.SendReadNoSnp", l2_cache, cluster_idx, args.plot_dir)
        
        # //  does not work on a docker env
        # plt.show()


if __name__ == "__main__":
    main()