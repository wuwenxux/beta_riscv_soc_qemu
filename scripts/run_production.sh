#!/bin/bash
# Beta SoC QEMU - Production boot (MCPU BootROM -> ACPU 32 cores)
#
# This boots the full production flow:
#   1. MCPU runs BootROM at 0xD0000000
#   2. BootROM writes per-core boot addresses and releases 32 ACPU cores via SW_RST_CTRL1
#   3. OpenSBI starts on all 32 ACPU cores
#   4. Linux 6.19.5 boots with 32 cores, 32GB DDR
#
# Expected output:
#   "MCPU: All ACPU cores released. MCPU idle."
#   "OpenSBI v1.3" banner
#   "Linux version 6.19.5"
#   "Run /init as init process"

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RELEASE_DIR="$(dirname "$SCRIPT_DIR")"
# Primary: built from source (no spaces in path)
QEMU="$HOME/qemu-beta-src/build/qemu-system-riscv64-unsigned"
if [ ! -f "$QEMU" ]; then
    # Fallback: in-tree build
    QEMU="$RELEASE_DIR/qemu-10.0.2-beta/build/qemu-system-riscv64"
fi

if [ ! -f "$QEMU" ]; then
    echo "ERROR: QEMU not built. Run 'make -j4' in ~/qemu-beta-src/build/ first."
    exit 1
fi

# Default to busybox initramfs; use ROOTFS env to override
ROOTFS="${ROOTFS:-$RELEASE_DIR/rootfs/initramfs.cpio.gz}"

exec "$QEMU" \
    -machine beta,config-file="$RELEASE_DIR/config/beta_production_config.json" \
    -bios "$RELEASE_DIR/firmware/bootrom/mcpu_bootrom_production.bin" \
    -device loader,file="$RELEASE_DIR/firmware/opensbi/fw_jump_0x4000000000.bin",addr=0x4000000000 \
    -kernel "$RELEASE_DIR/kernel/Image-6.19.5-beta" \
    -initrd "$ROOTFS" \
    -append "root=/dev/ram rdinit=/init earlycon=uart8250,mmio32,0xd0087000,115200n8 console=ttyS0,115200n8" \
    -nographic \
    "$@"
