# prepare gem5 repository and build RISCV and X86 models 
git submodule update --init --recursive
cd gem5/

## X86: build .fast and .opt version

#// scons build/X86_CHI/gem5.debug -j$(nproc)
scons build/X86_CHI/gem5.fast -j$(nproc)
scons build/X86_CHI/gem5.opt -j$(nproc)

## same for RISCV

#// scons build/RISCV_CHI/gem5.debug -j$(nproc)
scons build/RISCV_CHI/gem5.fast -j$(nproc)
scons build/RISCV_CHI/gem5.opt -j$(nproc)

cd -

# Download relevant images, bootloaders, and kernels
mkdir -p images
mkdir -p images/kernel
mkdir -p images/bootloader
mkdir -p images/disk


## riscv-ubuntu-24.04 and x86-ubuntu-22.04 disk images

#wget 
#wget 

## bootloaders/kernels
wget -P images/kernel https://gem5dist.blob.core.windows.net/dist/develop/kernels/riscv/static/linux-6.6.33
wget -P images/kernel https://gem5dist.blob.core.windows.net/dist/develop/kernels/x86/static/vmlinux-x86-ubuntu-6.8.0-52-generic

wget -P images/bootloader https://gem5dist.blob.core.windows.net/dist/develop/kernels/riscv/static/riscv-bootloader-opensbi-1.3.1-20231129
