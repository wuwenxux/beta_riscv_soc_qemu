#!/bin/bash
# Beta SoC QEMU - One-click build script
# Usage: ./scripts/build_all.sh
# Requirements: x86_64 Linux with gcc, ninja-build, meson, python3, pkg-config
#               libglib2.0-dev, libpixman-1-dev, libslirp-dev
#
# Ubuntu/Debian dependencies:
#   sudo apt-get install -y build-essential ninja-build meson python3 python3-venv \
#       pkg-config libglib2.0-dev libpixman-1-dev libslirp-dev libfdt-dev

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RELEASE_DIR="$(dirname "$SCRIPT_DIR")"
QEMU_DIR="$RELEASE_DIR/qemu-10.0.2-beta"

echo "============================================"
echo "  Beta SoC QEMU Build"
echo "  QEMU Version: 10.0.2 + Beta modifications"
echo "============================================"

# Check dependencies
for cmd in gcc ninja meson python3 pkg-config; do
    if ! command -v $cmd &>/dev/null; then
        echo "ERROR: $cmd not found. Please install build dependencies."
        echo "Ubuntu: sudo apt-get install build-essential ninja-build meson python3 python3-venv pkg-config libglib2.0-dev libpixman-1-dev libslirp-dev libfdt-dev"
        exit 1
    fi
done

echo "--- Configuring QEMU ---"
cd "$QEMU_DIR"
mkdir -p build && cd build
../configure \
    --target-list=riscv64-softmmu,riscv32-softmmu \
    --enable-slirp \
    --disable-docs

echo "--- Building QEMU ---"
ninja -j$(nproc)

echo ""
echo "============================================"
echo "  Build complete!"
echo "  QEMU binary: $QEMU_DIR/build/qemu-system-riscv64"
echo "============================================"
echo ""
echo "Quick test:"
echo "  cd $RELEASE_DIR && ./scripts/run_production.sh"
