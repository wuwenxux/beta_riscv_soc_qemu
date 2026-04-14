# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

This is the **Beta SoC QEMU** full-chip simulation platform — a fork of QEMU 10.0.2 with a custom RISC-V machine model (`-machine beta`) that emulates a dual-CPU SoC:

- **ACPU**: 32 cores, RV64 RVA23S64, VLEN=512, Ztso (8 clusters × 4 cores, hartid 0–31)
- **MCPU**: 1 core, RV32IMAC, hartid=32 (management processor that releases ACPU cores via SW_RST_CTRL1)
- **DDR**: 32 GB (8 channels × 4 GB) at physical address `0x4000000000`
- **Interrupt hierarchy**: 3-level APLIC (MLROOT→MLAPP→SLAPP) + IMSIC M/S per core + MCPU PLIC
- **PCIe**: 4× Synopsys DesignWare Gen4 controllers with RISC-V IOMMU (currently bypassed in QEMU)

## Repository Layout

```
beta_soc_qemu_release_v2/
├── qemu-10.0.2-beta/          # QEMU source with Beta SoC platform code
│   ├── hw/riscv/beta.c        # Main machine model — device instantiation, memory map
│   ├── hw/riscv/beta_dtb.c    # FDT/DTB generation for all hardware nodes
│   ├── hw/riscv/beta_config.c # JSON config file loader (parses beta_*.json at runtime)
│   ├── hw/misc/beta_soc_csr.c # SW_RST_CTRL1 register — ACPU core release logic
│   └── hw/misc/beta_generic_mmio.c  # Generic read/write MMIO stub device
├── config/                    # Runtime JSON hardware configs
│   ├── beta_production_config.json  # 32-core + 8-channel DDR (production)
│   ├── beta_acpu_direct_config.json # Skip MCPU BootROM (direct ACPU start)
│   └── beta_bm_run.json       # Bare-metal single-core config
├── firmware/
│   ├── bootrom/mcpu_bootrom_production.bin  # MCPU BootROM — releases ACPUs
│   └── opensbi/fw_jump_0x4000000000.bin     # OpenSBI FW_JUMP for DDR base
├── kernel/Image-6.19.5-beta   # Linux 6.19.5 kernel image
├── rootfs/                    # initramfs images (BusyBox / net-test)
├── scripts/                   # Build and launch helpers
└── tests/                     # Regression test suite
    ├── regression.sh           # Main test runner
    ├── cases/                  # Individual test case definitions (01–05)
    └── lib/common.sh           # Shared test utilities
```

## Build

The QEMU source is in `qemu-10.0.2-beta/`. **The build path must not contain spaces** (meson/ninja limitation).

**One-click build (Linux, recommended):**
```bash
./scripts/build_all.sh
# Produces: qemu-10.0.2-beta/build/qemu-system-riscv64
```

**Manual build:**
```bash
cd qemu-10.0.2-beta
mkdir -p build && cd build
../configure \
    --target-list=riscv64-softmmu,riscv32-softmmu \
    --enable-slirp \
    --disable-docs
ninja -j$(nproc)
```

**Ubuntu/Debian dependencies:**
```bash
sudo apt-get install -y build-essential ninja-build meson python3 python3-venv \
    pkg-config libglib2.0-dev libpixman-1-dev libslirp-dev libfdt-dev
```

**macOS (Homebrew) dependencies:**
```bash
brew install ninja meson pkg-config glib pixman libslirp dtc python3
# macOS produces: qemu-system-riscv64-unsigned (unsigned binary)
```

> The regression scripts expect the binary at `~/qemu-beta-src/build/qemu-system-riscv64-unsigned`. For development, place a symlink or use the in-tree build path.

## Running

```bash
# Production boot (32 cores, MCPU BootROM → ACPU)
./scripts/run_production.sh

# ACPU direct (skip MCPU, faster for dev)
./scripts/run_acpu_direct.sh

# Bare-metal ELF on single ACPU core
./scripts/run_bm.sh <path/to/test.elf>
```

**Development tip**: Add `maxcpus=4` to `-append` to skip 28 secondary HART timeout waits — boots in seconds instead of minutes under TCG.

**Memory note**: The kernel `mem=2G` parameter limits DDR to the first 2 GB of DDR0 to avoid a sparse-DDR linear map crash with Sv39. The `-append` strings in `run_production.sh` already include this.

**Export DTB for inspection:**
```bash
<qemu-binary> -machine beta,config-file=config/beta_production_config.json,dumpdtb=/tmp/beta.dtb -nographic
dtc -I dtb -O dts /tmp/beta.dtb | less
```

## Tests

```bash
cd tests

# Run all 5 regression rounds (includes incremental QEMU build)
./regression.sh --all

# Run a single round
./regression.sh --test smoke      # Round 1: DTB dump, no kernel (30s)
./regression.sh --test smp4       # Round 2: 4-core SMP Linux boot (120s)
./regression.sh --test virtio     # Round 3: PCIe + VirtIO network (180s)
./regression.sh --test smp32      # Round 4: Full 32-core production (600s)
./regression.sh --test irq_check  # Round 5: IRQ table consistency (requires dtc)

# Skip rebuild
./regression.sh --no-build --test smp4

# List available rounds
./regression.sh --list
```

Logs are saved to `tests/logs/<timestamp>/`. Round 5 requires `dtc` installed.

## Key Architecture Notes

### JSON-driven hardware configuration
All hardware parameters (memory regions, MMIO addresses, interrupt routing, peripheral register maps) are loaded at QEMU startup from the `-machine beta,config-file=<path>.json` argument. `beta_config.c` parses the JSON; `beta.c` instantiates devices from it. Changes to platform topology go in the JSON config, not hardcoded in C.

### DTB generation
`beta_dtb.c` generates the complete FDT at runtime from the loaded config. CPU nodes are added in reverse order (`for (i = num_acpu; i-- > 0; )`) so that `cpu@0` appears first in the DTB — this is a deliberate fix; reversing the loop breaks IPI interrupt routing.

### ACPU core release (MCPU boot flow)
`beta_soc_csr.c` implements SW_RST_CTRL1: a 32-bit register where bits[31:1] map to ACPU cores 0–30. Core 31 has no register bit; it is released automatically when all other bits are set. Secondary HARTs have `eistate[1]` pre-written to `0x3` (ENABLED|PENDING) to avoid a TCG race window where an IPI arriving before `eidelivery=1` is set would be silently dropped.

### IOMMU bypass
RISC-V IOMMU devices are instantiated (registers accessible) but explicitly unbound from PCIe DMA (`bus->iommu_ops = NULL`). The kernel has no `iommu-map` in the DTB and uses physical addresses for DMA; enabling IOMMU translation with an empty translation table would fault every DMA access.

### MMIO shadowing guard
`beta_mmio_overlaps_hw()` in `beta.c` prevents generic MMIO stubs from shadowing real APLIC/IMSIC address ranges. Any generic device whose address range overlaps an AIA component is silently skipped during device creation.

### TCG memory ordering
`target/riscv/cpu-param.h` sets `TCG_GUEST_DEFAULT_MO = TCG_MO_ALL` for correct TSO semantics. CBO cache instructions (`cbo.clean/flush/inval`) insert `smp_mb()` barriers and flush the TLB.
