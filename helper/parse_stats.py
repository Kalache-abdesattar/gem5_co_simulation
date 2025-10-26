#!/usr/bin/env python3
import json
import sys
import matplotlib.pyplot as plt
import numpy as np


import os 


# Allow passing stats path as argument
stats_path = sys.argv[1] if len(sys.argv) > 1 else "/home/gem5_co_simulation/stats.json"

# Load JSON stats
with open(stats_path, "r") as f:
    stats = json.load(f)

# Print top-level keys for debugging
print(stats.keys())

# Extract top-level metrics
sim_seconds = stats["simTicks"]["value"] / stats["simFreq"]["value"]
sim_insts = stats["simInsts"]["value"]
print(f"Simulated seconds: {sim_seconds}")
print(f"Simulated instructions: {sim_insts}")

# Navigate into nested structure
board = stats["board"]
cache_hierarchy = board.get("cache_hierarchy", {})


l3_cache = cache_hierarchy["l3cache"]
l3_cache_hit_accesses = l3_cache["cache"]["m_demand_hits"]["value"]
l3_cache_miss_accesses = l3_cache["cache"]["m_demand_misses"]["value"]
l3_hit_ratio = l3_cache_hit_accesses / (l3_cache_hit_accesses + l3_cache_miss_accesses)

print(f"Hit percentage {l3_hit_ratio * 100}%")

clusters = cache_hierarchy["core_clusters"]
ruby_system = cache_hierarchy["ruby_system"]
cluster_values = clusters["value"] 


# print((cluster_values[0])["dcache"]["cache"]["m_demand_hits"]["value"])


# Iterate through all core clusters
for cluster_idx, cluster in enumerate(cluster_values):
    dcache = cluster.get("dcache", None)
    if not dcache:
        continue

    hist_name = "outTransLatHist.SendReadShared"
    hist = dcache.get(hist_name, None)
    if not hist or hist.get("type") != "Distribution":
        continue

    # Extract bins and counts
    num_bins = int(hist["num_bins"])
    bin_size = float(hist["bin_size"])
    counts = [hist["value"][str(i)]["value"] for i in range(num_bins)]
    edges = np.arange(0, num_bins * bin_size, bin_size)

    # Plot
    plt.figure(figsize=(8, 5))
    plt.bar(edges, counts, width=bin_size * 0.9, align='edge', edgecolor='black')
    plt.title(f"Cluster {cluster_idx} DCache {hist_name}")
    plt.xlabel(f"Latency (cycles, bin size = {bin_size})")
    plt.ylabel("Count")
    plt.grid(axis="y", linestyle="--", alpha=0.7)

    # Save
    safe_name = hist_name.replace("::", "_").replace("/", "_")
    plot_path = os.path.join("/mnt/", f"cluster{cluster_idx}_{safe_name}.png")
    plt.savefig(plot_path)
    plt.close()
    print(f"✅ Saved histogram: {plot_path}")

# plt.show()
