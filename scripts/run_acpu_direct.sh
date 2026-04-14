#!/bin/bash
# Beta SoC QEMU - ACPU Direct boot (skip MCPU)
#
# This boots ACPU cores directly without MCPU BootROM:
#   1. All 32 ACPU cores start immediately at OpenSBI
#   2. No MCPU involvement (MCPU stays halted)
#   3. Faster boot for development/testing
#
# Note: No -bios flag (no MCPU BootROM needed)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RELEASE_DIR="$(dirname "$SCRIPT_DIR")"
QEMU="$RELEASE_DIR/qemu-10.0.2-beta/build/qemu-system-riscv64"

if [ ! -f "$QEMU" ]; then
    echo "ERROR: QEMU not built. Run ./scripts/build_all.sh first."
    exit 1
fi

ROOTFS="${ROOTFS:-$RELEASE_DIR/rootfs/initramfs.cpio.gz}"

exec "$QEMU" \
    -machine beta,config-file="$RELEASE_DIR/config/beta_acpu_direct_config.json" \
    -device loader,file="$RELEASE_DIR/firmware/opensbi/fw_jump_0x4000000000.bin",addr=0x4000000000 \
    -kernel "$RELEASE_DIR/kernel/Image-6.19.5-beta" \
    -initrd "$ROOTFS" \
    -append "root=/dev/ram rdinit=/init console=ttyS0 earlycon" \
    -nographic \
    "$@"
