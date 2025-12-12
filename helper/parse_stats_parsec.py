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

        
    #print(f"\n\n ====== {component_name} Cache Component Stats ======")
    #print(f"{component_name}_{id_} Total Accesses: {total_accesses}")
    #print(f"{component_name}_{id_} Cache Hit percentage: {hit_ratio * 100}%")

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
    "ideal_parsec_blackscholes",
    "ideal_parsec_bodytrack",
    "ideal_parsec_canneal",
    # "ideal_parsec_facesim",
    # "ideal_parsec_ferret",
    "ideal_parsec_fluidanimate",
    "ideal_parsec_freqmine",
    "ideal_parsec_raytrace",
    "ideal_parsec_streamcluster",
    "ideal_parsec_swaptions",
    "ideal_parsec_vips",
    "ideal_parsec_x264", 
    "nonideal_parsec_blackscholes",
    "nonideal_parsec_bodytrack",
    "nonideal_parsec_canneal",
    # "nonideal_parsec_facesim",
    # "nonideal_parsec_ferret",
    "nonideal_parsec_fluidanimate",
    "nonideal_parsec_freqmine",
    # "nonideal_parsec_raytrace",
    # "nonideal_parsec_streamcluster",
    # "nonideal_parsec_swaptions",
    # "nonideal_parsec_vips",
    # "nonideal_parsec_x264"
]



def get_stats(stats_path, sim_time, sim_insts, total_accesses, hits):

    # Load JSON stats
    with open(stats_path, "r") as f:
        stats = json.load(f)
    
    # print(f)
    # Print top-level keys for debugging
    # print(stats.keys())

    # Extract top-level metrics
    sim_seconds = stats["simTicks"]["value"] / stats["simFreq"]["value"]
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

    [total_access, hit] = scalar_stats(l3_cache, "")
    total_accesses["l3_cache"].append(total_access)
    hits["l3_cache"].append(hit)


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


def plot_l3_raw(benchmarks, l3_accesses, l3_hits, output_dir="plots"):
    """
    Takes raw arrays of data, splits them into Ideal/Non-Ideal, and plots them.
    Assumes the first half of the array is Ideal, and the second half is Non-Ideal.
    """
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)

    # 1. Determine Split Point (Ideal vs Non-Ideal)
    # We assume the lists contain: [Ideal_A, Ideal_B, ..., NonIdeal_A, NonIdeal_B, ...]
    n = len(benchmarks)
    mid_point = len(l3_accesses) // 2
    
    # Slice the data
    ideal_acc = l3_accesses[:mid_point]
    nonideal_acc = l3_accesses[mid_point:]
    
    ideal_hits = l3_hits[:mid_point]
    nonideal_hits = l3_hits[mid_point:]

    # X-Axis setup
    # If we have more data points than names, generate dummy names
    if len(ideal_acc) > n:
        labels = benchmarks + [f"Test_{i}" for i in range(len(ideal_acc) - n)]
    else:
        labels = benchmarks[:len(ideal_acc)] # Match length
        
    x = np.arange(len(labels))
    width = 0.35

    # --- PLOT 1: TOTAL ACCESSES ---
    fig, ax = plt.subplots(figsize=(12, 6))
    rects1 = ax.bar(x - width/2, ideal_acc, width, label='Ideal', color='skyblue', edgecolor='black')
    rects2 = ax.bar(x + width/2, nonideal_acc, width, label='Non-Ideal', color='salmon', edgecolor='black')

    ax.set_ylabel('Total Accesses')
    ax.set_title('L3 Cache Accesses: Ideal vs Non-Ideal')
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.legend()
    ax.grid(axis='y', linestyle='--', alpha=0.5)

    # Optional: Log scale if numbers vary significantly (e.g. 10k vs 10M)
    # ax.set_yscale('log')

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, "l3_accesses_raw.png"))
    print(f"Saved {output_dir}/l3_accesses_raw.png")
    plt.close()

    # --- PLOT 2: HITS ---
    fig, ax = plt.subplots(figsize=(12, 6))
    rects1 = ax.bar(x - width/2, ideal_hits, width, label='Ideal', color='steelblue', edgecolor='black')
    rects2 = ax.bar(x + width/2, nonideal_hits, width, label='Non-Ideal', color='darkred', edgecolor='black')

    ax.set_ylabel('Total Hits')
    ax.set_title('L3 Cache Hits: Ideal vs Non-Ideal')
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.legend()
    ax.grid(axis='y', linestyle='--', alpha=0.5)

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, "l3_hits_raw.png"))
    print(f"Saved {output_dir}/l3_hits_raw.png")
    plt.close()



def main():
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


    for test in benchmarks: 
        json_path = "/opt/gem5_gitlab/out/" + test + "/stats.json"
        get_stats(json_path, sim_time, sim_insts, total_accesses, hits)


    sim_time = sim_time + [1.046068, 0, 1.617322, 3.434042, 5.111734]
    sim_insts = sim_insts + [1029943462, 0, 1768344302, 3803317336, 141165972]

    # Core 0
    total_accesses["dcache_0"].extend([218290681, 2229339, 4600228, 9740082])
    total_accesses["icache_0"].extend([776658747, 4668193, 9416743, 23489231])
    hits["dcache_0"].extend([217772773, 2214720, 4571595, 9599643])
    hits["icache_0"].extend([776581615, 4664860, 9413009, 23471302])

    # Core 1
    total_accesses["dcache_1"].extend([921710, 1445074, 3153110, 4532648])
    total_accesses["icache_1"].extend([2230029, 3515364, 7787399, 11026419])
    hits["dcache_1"].extend([912823, 1429279, 3119462, 4483656])
    hits["icache_1"].extend([2228871, 3513605, 7782929, 11023037])

    # Core 2
    total_accesses["dcache_2"].extend([917999, 1668466, 50025088, 21052181])
    total_accesses["icache_2"].extend([2240549, 4270790, 174405199, 71506488])
    hits["dcache_2"].extend([909688, 1647661, 48582970, 20956377])
    hits["icache_2"].extend([2240253, 4255522, 174307961, 71426564])

    # Core 3
    total_accesses["dcache_3"].extend([941898, 1422446, 6424583, 9170153])
    total_accesses["icache_3"].extend([2320955, 3474993, 15426557, 25986210])
    hits["dcache_3"].extend([933042, 1409359, 6272376, 9032513])
    hits["icache_3"].extend([2319380, 3474370, 15365135, 25934176])

    # Core 4
    total_accesses["dcache_4"].extend([937662, 319956809, 6014560, 5304111])
    total_accesses["icache_4"].extend([2293897, 1278951378, 14459102, 13359332])
    hits["dcache_4"].extend([928734, 318637438, 5880706, 5224138])
    hits["icache_4"].extend([2292032, 1278896927, 14404498, 13345974])

    # Core 5
    total_accesses["dcache_5"].extend([1040045, 1651821, 479837093, 5158836])
    total_accesses["icache_5"].extend([2650670, 4018828, 2532454781, 12488090])
    hits["dcache_5"].extend([1029501, 1637602, 471199778, 5091894])
    hits["icache_5"].extend([2646124, 4017808, 2531814423, 12477165])

    # Core 6
    total_accesses["dcache_6"].extend([213518447, 1496898, 479185065, 7306015])
    total_accesses["icache_6"].extend([759620819, 3699575, 2529789551, 19730165])
    hits["dcache_6"].extend([213055088, 1482673, 470560289, 7168338])
    hits["icache_6"].extend([759557299, 3696602, 2529170780, 19678844])

    # Core 7
    total_accesses["dcache_7"].extend([920678, 318631905, 3031951, 4621532])
    total_accesses["icache_7"].extend([2246708, 1273397752, 7405976, 11476989])
    hits["dcache_7"].extend([912179, 317112807, 3004729, 4574543])
    hits["icache_7"].extend([2246116, 1273335765, 7404222, 11472405])

    # Core 0 L2
    total_accesses["l2_cache_0"].extend([575952, 9938, 16060, 128134])
    hits["l2_cache_0"].extend([141477, 3780, 4816, 20818])

    # Core 1 L2
    total_accesses["l2_cache_1"].extend([9272, 15945, 34504, 46352])
    hits["l2_cache_1"].extend([1279, 2504, 7301, 9133])

    # Core 2 L2
    total_accesses["l2_cache_2"].extend([7944, 33792, 1520822, 166450])
    hits["l2_cache_2"].extend([1036, 12825, 612772, 100643])

    # Core 3 L2
    total_accesses["l2_cache_3"].extend([9686, 12475, 206889, 169217])
    hits["l2_cache_3"].extend([1540, 1425, 64591, 67339])

    # Core 4 L2
    total_accesses["l2_cache_4"].extend([10001, 1246382, 182830, 80626])
    hits["l2_cache_4"].extend([1720, 1231327, 56125, 21911])

    # Core 5 L2
    total_accesses["l2_cache_5"].extend([13815, 13500, 8609059, 69755])
    hits["l2_cache_5"].extend([3408, 1755, 3899732, 14360])

    # Core 6 L2
    total_accesses["l2_cache_6"].extend([518627, 15801, 8580212, 168825])
    hits["l2_cache_6"].extend([108799, 3355, 3944951, 55645])

    # Core 7 L2
    total_accesses["l2_cache_7"].extend([8292, 1442212, 26955, 46151])
    hits["l2_cache_7"].extend([1006, 1428517, 3166, 8561])

    # L3 Cache
    total_accesses["l3_cache"].extend([575952, 9938, 16060, 128134])
    hits["l3_cache"].extend([423607, 62945, 9682007, 327023])

    plot_l3_raw(benchmarks, total_accesses["l3_cache"], hits["l3_cache"], output_dir="/mnt/fedora")

if __name__ == "__main__":
    main()