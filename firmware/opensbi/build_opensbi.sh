#!/bin/bash
# Build OpenSBI v1.3.1 for Beta SoC
# Requires: Docker with riscv64 cross-compiler
#
# The pre-built binary is included, this script is for rebuilding only.
# Output: fw_jump_0x4000000000.bin

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Building OpenSBI v1.3.1 for Beta SoC..."
echo "FW_TEXT_START=0x4000000000 (DDR base)"
echo "FW_JUMP_ADDR=0x4000200000 (kernel entry)"

docker run --rm \
    -v "$SCRIPT_DIR:/work" \
    ubuntu:25.10 \
    bash -c '
apt-get update -qq && apt-get install -y -qq gcc-15-riscv64-linux-gnu binutils-riscv64-linux-gnu make git python3
ln -sf /usr/bin/riscv64-linux-gnu-gcc-15 /usr/local/bin/riscv64-linux-gnu-gcc
ln -sf /usr/bin/riscv64-linux-gnu-ld.bfd /usr/local/bin/riscv64-linux-gnu-ld
ln -sf /usr/bin/riscv64-linux-gnu-objcopy /usr/local/bin/riscv64-linux-gnu-objcopy
ln -sf /usr/bin/riscv64-linux-gnu-ar /usr/local/bin/riscv64-linux-gnu-ar
ln -sf /usr/bin/riscv64-linux-gnu-nm /usr/local/bin/riscv64-linux-gnu-nm
export PATH=/usr/local/bin:$PATH
cd /tmp && git clone --depth 1 --branch v1.3.1 https://github.com/riscv-software-src/opensbi.git
cd opensbi
make -j$(nproc) CROSS_COMPILE=riscv64-linux-gnu- PLATFORM=generic \
    FW_TEXT_START=0x4000000000 FW_JUMP_ADDR=0x4000200000
cp build/platform/generic/firmware/fw_jump.bin /work/fw_jump_0x4000000000.bin
echo "=== OpenSBI build done ==="
'

echo "Output: $SCRIPT_DIR/fw_jump_0x4000000000.bin"
