#!/bin/bash
# Beta SoC QEMU - Bare-metal test runner
#
# Boots a single ACPU core in acpu_direct mode.
# MCPU is halted automatically. No BootROM or OS needed.
#
# Usage:
#   ./scripts/run_bm.sh <elf_file>
#   ./scripts/run_bm.sh scripts/bm.elf
#
# Notes:
#   - cpu-num=1 because MCPU is cpu 0, ACPU core 0 is cpu 1
#   - UART (ns16550a) at 0x10000000 for console output
#   - DDR at 0x80000000, 128MB

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RELEASE_DIR="$(dirname "$SCRIPT_DIR")"
QEMU="$RELEASE_DIR/qemu-10.0.2-beta/build/qemu-system-riscv64"

if [ ! -f "$QEMU" ]; then
    echo "ERROR: QEMU not built. Run ./scripts/build_all.sh first."
    exit 1
fi

ELF_FILE="${1:?Usage: $0 <elf_file>}"
if [ ! -f "$ELF_FILE" ]; then
    echo "ERROR: ELF file not found: $ELF_FILE"
    exit 1
fi

exec "$QEMU" \
    -machine beta,config-file="$RELEASE_DIR/config/beta_bm_run.json" \
    -device loader,file="$ELF_FILE",cpu-num=1 \
    -nographic \
    "$@"
