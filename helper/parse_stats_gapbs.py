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

    return [total_accesses, cache_hit_accesses]

    
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



benchmarks = [
    "ideal_gapbs_bc",
    "ideal_gapbs_bfs",
    "ideal_gapbs_cc",
    "ideal_gapbs_pr",
    "ideal_gapbs_tc",
    "nonideal_gapbs_bc",
    "nonideal_gapbs_bfs",
    "nonideal_gapbs_cc",
    "nonideal_gapbs_pr",
    "nonideal_gapbs_tc",
]

sim_insts = []
sim_time = []

total_accesses = {
    # L1 caches
    "dcache_0": [],
    "icache_0": [],
    "dcache_1": [],
    "icache_1": [],
    "dcache_2": [],
    "icache_2": [],
    "dcache_3": [],
    "icache_3": [],
    "dcache_4": [],
    "icache_4": [],
    "dcache_5": [],
    "icache_5": [],
    "dcache_6": [],
    "icache_6": [],
    "dcache_7": [],
    "icache_7": [],

    # Private L2 caches
    "l2_cache_0": [],
    "l2_cache_1": [],
    "l2_cache_2": [],
    "l2_cache_3": [],
    "l2_cache_4": [],
    "l2_cache_5": [],
    "l2_cache_6": [],
    "l2_cache_7": [],

    "l3_cache"  : [],
}


hits = {
    # L1 caches
    "dcache_0": [],
    "icache_0": [],
    "dcache_1": [],
    "icache_1": [],
    "dcache_2": [],
    "icache_2": [],
    "dcache_3": [],
    "icache_3": [],
    "dcache_4": [],
    "icache_4": [],
    "dcache_5": [],
    "icache_5": [],
    "dcache_6": [],
    "icache_6": [],
    "dcache_7": [],
    "icache_7": [],

    # Private L2 caches
    "l2_cache_0": [],
    "l2_cache_1": [],
    "l2_cache_2": [],
    "l2_cache_3": [],
    "l2_cache_4": [],
    "l2_cache_5": [],
    "l2_cache_6": [],
    "l2_cache_7": [],

    "l3_cache"  : [],
}



def get_stats(stats_path):

    # Load JSON stats
    with open(stats_path, "r") as f:
        stats = json.load(f)

    # Print top-level keys for debugging
    print(stats.keys())

    # Extract top-level metrics
    sim_seconds = stats["simTicks"]["value"]
    sim_inst = stats["simInsts"]["value"]

    sim_time.append(sim_seconds)
    sim_insts.append(sim_inst)

    # print("\n ====== Simulation Runtime Stats ======")
    # print(f"Simulated seconds: {sim_seconds}")
    # print(f"Simulated instructions: {sim_insts}")

    # Navigate into nested structure
    board = stats["board"]
    cache_hierarchy = board.get("cache_hierarchy", {})


    l3_cache = cache_hierarchy["l3cache"]

    scalar_stats(l3_cache, None)

    # get_transaction_hist("outTransLatHist.SendReadNoSnp", l3_cache, None, plot_dir)


    clusters = cache_hierarchy["core_clusters"]
    ruby_system = cache_hierarchy["ruby_system"]
    cluster_values = clusters["value"] 


    # print((cluster_values[0])["dcache"]["cache"]["m_demand_hits"]["value"])
    # Iterate through all core clusters
    for cluster_idx, cluster in enumerate(cluster_values):
        cache_component = cluster.get("icache", None)
        [total_access, hit] = scalar_stats(cache_component, cluster_idx)

        total_accesses["icache_"+str(cluster_idx)].append(total_access)
        hits["icache_"+str(cluster_idx)].append(hit)

        cache_component = cluster.get("dcache", None)
        [total_access, hit] = scalar_stats(cache_component, cluster_idx)
        
        total_accesses["dcache_"+str(cluster_idx)].append(total_access)
        hits["dcache_"+str(cluster_idx)].append(hit)

        # L2 is named L1.downstream in CHI Ruby     
        # We use either an L1i or L1d 's downstream pointer to access the corresponding L2
        l2_cache = cache_component["downstream_destinations"]["value"][0]
        [total_access, hit] = scalar_stats(l2_cache, cluster_idx)

        total_accesses["l2_cache_"+str(cluster_idx)].append(total_access)
        hits["l2_cache_"+str(cluster_idx)].append(hit)


        # get_transaction_hist("outTransLatHist.SendReadNoSnp", l2_cache, cluster_idx, plot_dir)
        
        # //  does not work on a docker env
        # plt.show()


def plot_sim_time(output_dir="plots"):
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)

    # Split benchmarks into Ideal and Non-Ideal
    mid_point = len(benchmarks) // 2
    
    # Extract short labels (e.g., "ideal_gapbs_bc" -> "BC")
    labels = [b.split('_')[-1].upper() for b in benchmarks[:mid_point]]
    
    x = np.arange(len(labels))  # label locations
    width = 0.35  # bar width

    # Split the collected sim_time list
    ideal_times = sim_time[:mid_point]
    nonideal_times = sim_time[mid_point:]

    fig, ax = plt.subplots(figsize=(10, 6))
    rects1 = ax.bar(x - width/2, ideal_times, width, label='Ideal', color='tab:blue')
    rects2 = ax.bar(x + width/2, nonideal_times, width, label='Non-Ideal', color='tab:orange')

    # Formatting
    ax.set_ylabel('Simulated Ticks (Lower is Better)')
    ax.set_title('Simulation Time Comparison: Ideal vs Non-Ideal')
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.legend()
    ax.grid(axis='y', linestyle='--', alpha=0.7)

    # Add text labels on top of bars (optional, useful if values are close)
    # ax.bar_label(rects1, padding=3, fmt='%.1e', fontsize=8)
    # ax.bar_label(rects2, padding=3, fmt='%.1e', fontsize=8)

    save_path = os.path.join(output_dir, "comparison_sim_time.png")
    plt.savefig(save_path)
    print(f"Saved {save_path}")
    plt.close()


def main():
    base_path = "/opt/gem5_gitlab/out/"
    
    print("Parsing stats...")
    for test in benchmarks: 
        json_path = os.path.join(base_path, test, "stats.json")
        if os.path.exists(json_path):
            get_stats(json_path)
        else:
            print(f"Warning: File not found {json_path}")
            sim_time.append(0)
            sim_insts.append(0)
            # Fill dummy cache data to prevent index errors
            for k in total_accesses.keys():
                total_accesses[k].append(0)
                hits[k].append(0)

    print("Generating sim_time plot...")
    plot_sim_time("/mnt/fedora/")  # <--- Call the new function here


if __name__ == "__main__":
    main()