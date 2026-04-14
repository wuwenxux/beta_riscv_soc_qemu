#!/bin/bash
# Build Linux 6.19.5 for Beta SoC QEMU platform
# Uses Ubuntu 25.10 + GCC 15.2 cross-compiler in Docker (colima)
set -e

DOCKER_HOST=unix:///Users/qingyuanren/.colima/default/docker.sock
export DOCKER_HOST

WORKDIR=/Users/qingyuanren/Desktop/QEMU
KERNEL_VER=6.19.5
KERNEL_TAR=linux-${KERNEL_VER}.tar.xz
KERNEL_URL=https://cdn.kernel.org/pub/linux/kernel/v6.x/${KERNEL_TAR}

# Step 1: Download kernel source if not present
if [ ! -f "${WORKDIR}/${KERNEL_TAR}" ]; then
    echo "=== Downloading Linux ${KERNEL_VER} ==="
    curl -L -o "${WORKDIR}/${KERNEL_TAR}" "${KERNEL_URL}"
fi

# Step 2: Extract if not already extracted
if [ ! -d "${WORKDIR}/linux-${KERNEL_VER}" ]; then
    echo "=== Extracting Linux ${KERNEL_VER} ==="
    cd "${WORKDIR}" && tar xf "${KERNEL_TAR}"
fi

# Step 3: Build kernel in Docker
echo "=== Building kernel in Docker (Ubuntu 25.10 + GCC 15.2) ==="
docker run --rm \
    -v "${WORKDIR}:/work" \
    -w /work/linux-${KERNEL_VER} \
    ubuntu:25.10 \
    bash -c '
set -e
echo "--- Installing build dependencies ---"
apt-get update -qq 2>/dev/null || apt-get update -qq 2>/dev/null || true
apt-get install -y -qq --fix-missing gcc-15-riscv64-linux-gnu binutils-riscv64-linux-gnu make bc flex bison libelf-dev libssl-dev kmod cpio 2>/dev/null | tail -1

# Create symlinks so CROSS_COMPILE works without version suffix
ln -sf /usr/bin/riscv64-linux-gnu-gcc-15 /usr/local/bin/riscv64-linux-gnu-gcc
ln -sf /usr/bin/riscv64-linux-gnu-g++-15 /usr/local/bin/riscv64-linux-gnu-g++ 2>/dev/null || true

export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-
export PATH=/usr/local/bin:$PATH

echo "--- GCC version ---"
$CC --version | head -1

echo "--- Generating defconfig ---"
make defconfig

echo "--- Enabling Beta-required configs ---"
# Increase thread stack size to avoid overflow during PCIe enumeration
./scripts/config --set-val CONFIG_THREAD_SIZE_ORDER 3
# Core
./scripts/config --enable CONFIG_SMP
./scripts/config --set-val CONFIG_NR_CPUS 64
./scripts/config --enable CONFIG_RISCV_SBI
./scripts/config --enable CONFIG_RISCV_SBI_V01

# AIA (APLIC + IMSIC)
./scripts/config --enable CONFIG_RISCV_APLIC
./scripts/config --enable CONFIG_RISCV_IMSIC

# Timer
./scripts/config --enable CONFIG_RISCV_TIMER

# ISA extensions
./scripts/config --enable CONFIG_RISCV_ISA_V
./scripts/config --enable CONFIG_RISCV_ISA_V_DEFAULT_ENABLE
./scripts/config --enable CONFIG_RISCV_ISA_SVNAPOT
./scripts/config --enable CONFIG_RISCV_ISA_SVPBMT
./scripts/config --enable CONFIG_RISCV_ISA_ZICBOM
./scripts/config --enable CONFIG_RISCV_ISA_ZICBOZ
./scripts/config --enable CONFIG_RISCV_ISA_ZAWRS

# PCIe
./scripts/config --enable CONFIG_PCI
./scripts/config --enable CONFIG_PCIE_DW
./scripts/config --enable CONFIG_PCIE_DW_PLAT
./scripts/config --enable CONFIG_PCIE_DW_PLAT_HOST
./scripts/config --enable CONFIG_PCI_MSI

# Serial
./scripts/config --enable CONFIG_SERIAL_8250
./scripts/config --enable CONFIG_SERIAL_8250_CONSOLE
./scripts/config --enable CONFIG_SERIAL_OF_PLATFORM

# Virtio (for testing)
./scripts/config --enable CONFIG_VIRTIO
./scripts/config --enable CONFIG_VIRTIO_PCI
./scripts/config --enable CONFIG_VIRTIO_NET
./scripts/config --enable CONFIG_VIRTIO_BLK

# Initramfs
./scripts/config --enable CONFIG_BLK_DEV_INITRD
./scripts/config --enable CONFIG_BLK_DEV_RAM

# KVM / Hypervisor guest support
./scripts/config --enable CONFIG_KVM

echo "--- Running olddefconfig ---"
make olddefconfig

echo "--- Building kernel (Image) ---"
make -j$(nproc) Image

echo "--- Copying output ---"
cp arch/riscv/boot/Image /work/Image-6.19.5

echo "=== Kernel build complete: Image-6.19.5 ==="
'

echo "=== Done! Kernel at ${WORKDIR}/Image-6.19.5 ==="
ls -lh "${WORKDIR}/Image-6.19.5"
