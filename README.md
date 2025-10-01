# gem5_co_simulation

This repository provides a wrapper and supporting scripts to run **co-simulations** on top of another gem5 repository. It is designed to make it easier to set up experiments, manage disk images, and reproduce results when integrating gem5 with external software stacks.

---

## 📌 Overview

* Uses an **external gem5 build** as the simulator backend.
* Provides helper scripts for configuring runs, launching workloads, and analyzing output.
* Designed for **system-level studies** where gem5 interacts with OS images, workloads, or other simulators.

---

## 🔧 Prerequisits
'''bash
sudo apt update
sudo apt install -y build-essential python3 python3-dev python3-six \
    scons m4 libprotobuf-dev protobuf-compiler libgoogle-perftools-dev \
    libboost-all-dev libhdf5-dev zlib1g-dev
'''

## 🔧 Requirements

* Python 3.8+

