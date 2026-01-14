# prepare gem5 repository and build RISCV and X86 models 
export GEM5_EXTRAS=$(pwd)/config/

git submodule update --init --recursive
cd gem5/

pip3 install -r requirements.txt


## X86: build .fast and .opt version

echo "[env.sh]: Building GEM5 Binaries"
# scons build/X86_CHI/gem5.debug -j$(nproc) EXTRAS="$GEM5_EXTRAS"
# scons build/X86_CHI/gem5.fast -j$(nproc) EXTRAS="$GEM5_EXTRAS"
scons build/X86_CHI/gem5.opt -j$(nproc) EXTRAS="$GEM5_EXTRAS"

# ## same for RISCV

# #// scons build/RISCV_CHI/gem5.debug -j$(nproc) EXTRAS="$GEM5_EXTRAS"
# # scons build/RISCV_CHI/gem5.fast -j$(nproc) EXTRAS="$GEM5_EXTRAS"
# scons build/RISCV_CHI/gem5.opt -j$(nproc) EXTRAS="$GEM5_EXTRAS"


echo "[env.sh] Building m5term..."
cd util/term && make 

cd - 
cd .. 


echo "[env.sh]: Downloading Kernels, Disk images, and Bootloaders"

# # Download relevant images, bootloaders, and kernels
mkdir -p images
mkdir -p images/kernel
mkdir -p images/bootloader
mkdir -p images/disk



# # ## bootloaders/kernels
wget -O images/kernel/riscv-linux-5.15.180 https://gem5dist.blob.core.windows.net/dist/develop/kernels/riscv/static/riscv-linux-5.15.180-kernel
wget -O images/kernel/x86-linux-6.8.0-52 https://gem5dist.blob.core.windows.net/dist/develop/kernels/x86/static/vmlinux-x86-ubuntu-6.8.0-52-generic

wget -O images/bootloader/riscv-bootloader-opensbi-1.3.1 https://gem5dist.blob.core.windows.net/dist/develop/kernels/riscv/static/riscv-bootloader-opensbi-1.3.1-20231129

mkdir -p checkpoints
mkdir -p out
