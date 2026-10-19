# McPAT / CACTI — Cache Timing and Power Modelling

Moved out of the root README to keep that focused on the co-simulation.
These are the analytical tools used to derive the cache latencies fed into
the gem5 CHI model.

---

## Building McPAT/Cacti Tools
McPAT and CACTI are analytical tools used to estimate the performance, area, and power characteristics of memory structures, such as SRAM caches, DRAM, and register files. 


```bash
git submodule update --init --recursive
```

Open *ext/mcpat/cacti/cacti.mk* and remove **-m32** option. 
Then, to enable AVX replace:
```bash
OPT = -O3 -msse2 -mfpmath=sse -DNTHREADS=$(NTHREADS) 
```
with 
```bash
OPT = -O3 -mavx2 -mfma -mfpmath=sse -DNTHREADS=$(NTHREADS)
```

build:
```bash
cd ext/mcpat/cacti
make -j($nproc)
```

In order to build McPAT, the same steps are applied to *ext/mcpat/mcpat.mk*. However, only memory latencies are required at this stage so Cacti tool will suffice.

## Using McPAT/Cacti Tools
CACTI requires a configuration file (.cfg) that defines the cache architecture you want to model. A template can be found *config/mcpat_configs/*. Change the values of size, associativity, #banks,...ect and run:

```bash
./ext/mcpat/cacti/cacti -infile config/mcpat_configs/cache.cfg > cacti_cache.out
```

Please keep the *-access mode* as *fast* as it is the mode used in the GEM5 CHI model.

## Understanding the Output of McPAT/Cacti
After running CACTI, the tool generates a detailed hardware report. For architectural modeling in gem5, prioritize the following metrics:

* **Access Time (ns):** The total time required for a cache hit. In a sequential access model, this is the sum of tag and data array delays.
* **Cycle Time (ns):** The minimum time required between consecutive requests. If this is much lower than the Access Time, it indicates the cache is internally pipelined.
* **Total Leakage Power (mW):** The static power consumption. For a 64MB cache at 22nm, ensuring this is under 50mW usually requires using `LSTP` (Low Standby Power) transistors.
* **Dynamic Read/Write Energy (nJ):** The energy consumed per individual access. 
* **Area Efficiency:** The ratio of the memory cell area to the total peripheral logic area. Values above 70% are typical for large Last Level Caches (LLCs).


## Integrating with gem5
To translate CACTI's physical results into a **gem5 CHI (Coherent Hub Interface)** model, you must map the nanosecond results to the simulator's cycle-based parameters.

### Step 1: Time-to-Cycle Conversion
Convert the nanosecond results into clock cycles based on your L3 clock frequency ($f$) (the frequency of the L3 half of cores/board frequency):
$$\text{Cycles} = \lceil \text{CACTI Time (ns)} \times f (\text{GHz}) \rceil$$

### Step 2: Parameter Mapping
Apply the converted values to your gem5 Python configuration:

| gem5 Parameter | CACTI Component |
| :--- | :--- |
| `tag_latency` | **Tag side** time component |
| `data_latency` | **Data side** time component |

The other latencies in the python config (eg. read_hit_latency, allocation_latency) are pipeline latency and are *fixed* regardless of the size of the data/tag memory sizes. 

