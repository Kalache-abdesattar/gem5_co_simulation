# gem5_co_simulation

This repository provides a wrapper and supporting scripts to run **co-simulations** on top of another gem5 repository. It is designed to make it easier to set up experiments, manage disk images, and reproduce results when integrating gem5 with external software stacks.

---

## 📌 Overview

* Uses an **external gem5 build** as the simulator backend.
* Provides helper scripts for configuring runs, launching workloads, and analyzing output.
* Designed for **system-level studies** where gem5 interacts with OS images, workloads, or other simulators.

---

## 🔧 Prerequisits
```bash
sudo apt update
sudo apt install -y build-essential python3 python3-dev python3-six \
    scons m4 libprotobuf-dev protobuf-compiler libgoogle-perftools-dev \
    libboost-all-dev libhdf5-dev zlib1g-dev
```

## Using the Docker Image
All the sources, images, and artifects are contained within the docker image. Therefore, there is no need to reproduce the artifects yourself. 
```bash
docker pull abdesattar/tau_gem5:latest
```

Run the docker image interactively with privileged permissions (required for KVM and accessing Loop devices). Also, mount your local workspace: 
```bash
docker run -it --privileged -v $HOME_DIR:/mnt DOCKER_IMAGE
```

## Docker Image Directory Structure
```
/home/gem5_co_simulation/ (main)
├── gem5/ (submodule)   # GEM5 submodule
|   ├── build/
|   |   ├── RISCV_CHI/  # RISCV CHI GEM5 Model Executable
|   |   ├── X86_CHI/    # X86 CHI GEM5 Model Executable
├── config/             # GEM5 Model config files
|   ├── bench/          # Parsec benchmark config file for X86  
│   ├── chi/            # CHI protocol configs, nodes, and interconnect definitions
│   └── run/            # Generic RISC-V and X86 runner scripts
├── checkpoints/        # Architectural checkpoints for speeding-up RISCV bring-up 
├── images/           
│   ├── kernel/       
│   ├── disk/
|   |   ├── riscv-ubuntu-npb        # ubuntu 24.04 image with Nasa Parallel Benchmark built for RISCV
|   |   ├── riscv-ubuntu-24.04.img  # A generic ubuntu 24.04 image with Gromacs+libm5.a built for RISCV (V extension disabled)
|   |   ├── x86-ubuntu-22.04.img    # A generic ubuntu 22.04 image with Gromacs+libm5.a built for X86 
│   └── bootloaders/
├── out/                # GEM5 simulation default output directory
└── helper/             # Helper scripts for parsing GEM5 stats

/home/venv/             # Python virtual environment
/root/.cache/gem5/      # contains x86-parsec disk image downloaded directly from gem5.resources
```

## Re-running the Experiments Yourself 
### X86 Parsec Benchmark 
From the repo root **/home/gem5_co_simulation**.

```bash
gem5/build/X86_CHI/gem5.opt config/bench/x86-parsec.py --benchmark BENCHMARK --size {simsmall, simmedium, simlarge}

# You can check other options (including the list of benchmarks) by executing:
gem5/build/X86_CHI/gem5.opt config/bench/x86-parsec.py --help

# Enabling json stats:
gem5/build/X86_CHI/gem5.opt --stats-file=json://stats.json config/bench/x86-parsec.py --benchmark BENCHMARK --size {simsmall, simmedium, simlarge}

# Quick Example
gem5/build/X86_CHI/gem5.opt --stats-file=json://stats.json config/bench/x86-parsec.py --benchmark blackscholes --size simsmall

# The benchmark blackscholes takes around 10min to complete on the detailed CHI Hierarchy with SimpleTiming (default). The exact runtime depends on your machine's
# single threaded performance. If the application takes an abnormally long time to complete, restart it.  
```

When the gem5 loads the kernel and disk image, it exposes a port terminal port (typically on 3456). You can access it as follow from another terminal:
```bash
# Tip: use Tmux, it is installed in the docker image
gem5/util/term/m5term 3456
```

### X86 and RISCV Generic Run
Under **/home/gem5_co_simulation/config/run**, two generic run script for both X86 and RISCV are found.  
```bash
~ gem5/build/RISCV_CHI/gem5.opt config/run/riscv-ubuntu-run.py --help

usage: riscv-ubuntu-run.py [-h] [--num-cores NUM_CORES] [--cores-per-cluster CORES_PER_CLUSTER] [--cache-class {chi,mesi-three-level,no-cache}] [--cpu-type {timing,o3,minor}] [--mem-size MEM_SIZE] [--disk-image DISK_IMAGE] [--kernel KERNEL] [--bootloader BOOTLOADER] [--save-checkpoint] [--load-checkpoint] [--checkpoint-path CHECKPOINT_PATH]

Run RISCV Ubuntu FS simulation with CHI cache hierarchy

options:
  -h, --help            show this help message and exit
  --num-cores NUM_CORES
                        Total number of cores
  --cores-per-cluster CORES_PER_CLUSTER
                        Cores per CHI cluster, a cluster is the group of cores sharing an L2
  --cache-class {chi,mesi-three-level,no-cache}
                        Cache hierarchy class to use
  --cpu-type {timing,o3,minor}
                        Type of CPU model to use: timing (TimingSimpleCPU), o3 (O3CPU), or minor (MinorCPU)
  --mem-size MEM_SIZE   Memory size (e.g., 2GiB, 8GiB)
  --disk-image DISK_IMAGE
                        Path to the root disk image
  --kernel KERNEL       Path to the kernel image
  --bootloader BOOTLOADER
                        Path to the bootloader
  --save-checkpoint     Save a checkpoint at the end of the simulation
  --load-checkpoint     Load from an existing checkpoint instead of booting fresh
  --checkpoint-path CHECKPOINT_PATH
                        Path to the checkpoint directory
```

For RISCV, checkpointing is used to accelerate the RISC-V system bringup as well as fast-forward to Region-of-Interest within the targeted benchmark. On the other hand, X86 uses KVM for the same purpose. You can generate a checkpoint by the specifying the options **--save-checkpoint** and similarly for loading a checkpoint **load-checkpoint**; a checkpoint's path for both cases is dicated by **--checkpoint-path**.

**Important**: A checkpoint will not be saved unless you perform a first "m5 exit" when you reach the saving point. You can perform an "m5 exit" interactively from the m5term terminal with **m5 exit** command, or "programatically" from within a benchmark sources' through M5OPS.   

As an example, you can run NPD benchmark for RISC-V as follows: 
```bash
# a checkpoint already exists, we just need to load it.
gem5/build/RISCV_CHI/gem5.opt config/run/riscv-ubuntu-run.py --disk-image /images/disk/riscv-ubuntu-24.04-npb --checkpoint-path checkpoints/riscv_ubuntu_checkpoint --load-checkpoint

# similarly to before, you can enable json stats generation
gem5/build/RISCV_CHI/gem5.opt --stats-file=json://stats.json config/run/riscv-ubuntu-run.py --disk-image /images/disk/riscv-ubuntu-24.04-npb --checkpoint-path checkpoints/riscv_ubuntu_checkpoint --load-checkpoint
```

On another (virtual) terminal:  
```bash
gem5/util/term/m5term 3456
# wait a while for the session to open...

# execute fourier transform, it takes some time
/root/gem5~ ./bin/ft.S.x
```

### Gromacs
Similar as before, just point gem5 to **images/disk/riscv-ubuntu-24.04-img**:
```bash
~ gem5/build/RISCV_CHI/gem5.opt --disk-image /images/disk/riscv-ubuntu-24.04-img --checkpoint-path checkpoints/riscv_gromacs_checkpoint --save-checkpoint
# There are no checkpoints for RISCV Gromacs...the bringup takes around 7min
```
On another (virtual) terminal:  
```bash
gem5/util/term/m5term 3456

# On the gem5 environment 
cd /home/gromacs/out

# Executes mdrun simulation of lyzomzyme in water
# gromacs is annotated with m5ops. Hence, gem5 will perform an exit to switch to the detailed model right before the real simulation   
sudo /home/gromacs/build/bin/gmx mdrun -v -deffnm md_0_10 -cpt -1 -nt 4
```

### Stats Parser
Stats parser parses the json-formatted stat file generated from gem5. It extracts scalar stat entities such as (simulation runtime, number of instructions, hit/miss ratios for each cache level), and vector stat entities (eg. delay histograms for each CHI transaction) and plots them. 
```bash
python3 helper/parse_stats.py
```

### Konata Viewer
[Konata viewer](https://github.com/shioyadan/Konata) is an instruction pipeline visualizer for O3 cpu-type of GEM5. First, install [konata](https://github.com/shioyadan/Konata/releases/tag/v0.39). Then, you can generate konata-compatible traces from gem5 by using **O3PipeView** debug flags, forward the traces file to your host machine for inspection through the mounted shared directory /mnt/.
```bash
# An O3 cpu type must be used 
gem5/build/RISCV_CHI/gem5.opt --debug-flags=O3PipeView --debug-file /mnt/npd_trace.v config/run/riscv-ubuntu-run.py cpu-type o3 ...
```
