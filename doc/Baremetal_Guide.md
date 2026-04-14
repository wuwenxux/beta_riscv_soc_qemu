# Beta SoC QEMU 裸机（Bare-Metal）测试指南

基于 `beta_bm_run.json` 配置，在单 ACPU 核心上运行无操作系统的 ELF 程序。

---

## 目录

1. [快速开始](#1-快速开始)
2. [内存映射](#2-内存映射)
3. [编写与编译裸机程序](#3-编写与编译裸机程序)
4. [L0 级验证测试套件](#4-l0-级验证测试套件)
5. [调试技巧](#5-调试技巧)

---

## 1. 快速开始

### 前置条件

QEMU 已编译完成（参见根目录 `README.md` §3）：

```bash
# 验证 QEMU 可用
./qemu-10.0.2-beta/build/qemu-system-riscv64 --version
# QEMU emulator version 10.0.2 (Beta SoC fork)
```

### 运行预编译的测试二进制

`tests/` 目录下已有一个可直接运行的裸机 ELF：

```bash
./scripts/run_bm.sh tests/bm_0407_qemu.elf
```

预期输出（UART `0xD0087000`，115200 baud）：

```
=== Beta SoC Bare-Metal Test ===
UART: OK
...
[PASS]
```

按 `Ctrl-A X` 退出 QEMU。

### 脚本说明

`scripts/run_bm.sh` 等价于：

```bash
QEMU=./qemu-10.0.2-beta/build/qemu-system-riscv64

$QEMU \
  -machine beta,config-file=config/beta_bm_run.json \
  -device loader,file=<elf_file>,cpu-num=1 \
  -nographic
```

> **cpu-num=1**：QEMU 内部 CPU 编号从 0 起，MCPU 占 cpu 0，第一个 ACPU（hartid=0）是 cpu 1。  
> `-device loader,cpu-num=N` 同时加载 ELF 并将该 CPU 的 reset vector 设为 ELF 入口。

---

## 2. 内存映射

`config/beta_bm_run.json` 定义的裸机地址空间：

| 区域 | 基址 | 大小 | 说明 |
|------|------|------|------|
| DDR（代码+数据） | `0x80000000` | 128 MB | ACPU 可见，ELF 入口应链接至此 |
| UART（ns16550a） | `0xD0087000` | 256 B | 32-bit word 访问，10 MHz 时钟 |
| CLINT | `0x02000000` | 64 KB | MSWI + MTIMER |
| PLIC | `0x0C000000` | 4 MB | MCPU PLIC（裸机一般不用）|
| APLIC M-root | `0x0D000000` | 16 KB | 机器级中断控制器 |
| APLIC S-app | `0x0D004000` | 16 KB | 监管级中断控制器 |
| IMSIC M-level | `0x24000000` | — | 每核 M 级 MSI 文件 |
| IMSIC S-level | `0x28000000` | — | 每核 S 级 MSI 文件 |
| SOC_TOP_CSR | `0x100000000` | 8 KB | 核心释放、PLL 状态等寄存器 |
| L2 CSR stub | `0x110100000` | 1 MB | 缓存 CSR（只读返回 0）|

**UART 初始化参数**（115200 baud @ 10 MHz 时钟）：

```
LCR = 0x03  (8N1, DLAB=0)
DLL = 5     (低字节分频)
DLF = 27    (小数分频，DW UART 专用，偏移 0xC0)
```

---

## 3. 编写与编译裸机程序

### 3.1 链接脚本

裸机程序必须链接到 `0x80000000`（DDR 基址）。最小链接脚本：

```ld
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    DDR (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS {
    . = ORIGIN(DDR);

    .text : { *(.text.start) *(.text*) } > DDR
    .rodata : { *(.rodata*) } > DDR
    .data : { *(.data*) } > DDR

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > DDR

    . = ALIGN(4096);
    .stack (NOLOAD) : {
        _stack_bottom = .;
        . += 0x2000 * 2;   /* 8KB × 2 harts */
        _stack_top = .;
    } > DDR
}
```

### 3.2 启动汇编（boot.S）

```asm
.section .text.start, "ax"
.globl _start
_start:
    csrr    a0, mhartid

    /* 每个 hart 独立栈（8KB/hart） */
    la      sp, _stack_top
    li      t0, 0x2000
    mul     t1, a0, t0
    sub     sp, sp, t1

    /* trap handler */
    la      t0, trap_entry
    csrw    mtvec, t0

    /* 开放 PMP */
    li      t0, -1
    srli    t0, t0, 10
    csrw    pmpaddr0, t0
    li      t0, 0xf
    csrw    pmpcfg0, t0

    /* hart 0 清零 BSS，其他 hart 等待 */
    bnez    a0, .wait

    la      t0, __bss_start
    la      t1, __bss_end
.clear:
    bge     t0, t1, .done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       .clear
.done:
    call    main
1:  j       1b

.wait:
    wfi
    j       .wait
```

### 3.3 编译命令

```bash
CROSS=riscv64-linux-gnu-

${CROSS}gcc \
    -march=rv64imafdc -mabi=lp64d \
    -O2 -nostdlib -nostartfiles -ffreestanding \
    -T link.ld \
    boot.S main.c \
    -o mytest.elf

# 可选：生成反汇编用于调试
${CROSS}objdump -d mytest.elf > mytest.dis
```

### 3.4 运行

```bash
./scripts/run_bm.sh mytest.elf
```

---

## 4. L0 级验证测试套件

l0_tests 位于 `../beta_soc_qemu_release/l0_tests/`（v1 目录），包含 5 个验证用例：

| 用例 | 文件 | 测试内容 | 预计耗时（QEMU）|
|------|------|----------|----------------|
| L0-01 | `l0_01_tso/` | TSO Litmus（SB/MP/LB × 5 种拓扑 × 1000 万次）| ~5 min |
| L0-02 | `l0_02_snoop/` | AMO 压力（8 核对角线 + 32 核全压 + 伪共享）| ~3 min |
| L0-03 | `l0_03_dma/` | DMA 一致性（4 种缓存状态）| ~2 min |
| L0-04 | `l0_04_pa/` | 40-bit PA 越界（SV48 + 地址枚举）| ~1 min |
| L0-05 | `l0_05_rvv/` | RVV CRC32c + 纠删码 + 边界条件 | ~2 min |

### 4.1 构建

l0_tests 使用 SRAM `0xF0000000` 作为代码基址，需要 v1 的 `beta_ext_bm_config.json`（含 SRAM 内存区域）。

```bash
cd ../beta_soc_qemu_release/l0_tests

# 构建全部 5 个 ELF（默认 8 核）
make all

# 只构建某一个
make l0_01

# 改变激活核数（HAPS 32 核模式）
make all ACTIVE_HARTS=32

# 产物
ls build/
# l0_01_tso.elf  l0_02_snoop.elf  l0_03_dma.elf  l0_04_pa.elf  l0_05_rvv.elf
```

### 4.2 运行（QEMU）

l0_tests ELF 入口在 `0xF0000000`（SRAM），而 `beta_bm_run.json` 的 reset vector 是 `0x80000000`（DDR），因此需要使用 v1 的 `run_baremetal.sh`，它会自动在 `0x80000000` 生成跳转跳板：

```bash
cd ../beta_soc_qemu_release

# 运行单个用例
./scripts/run_baremetal.sh l0_tests/build/l0_01_tso.elf

# 指定不同配置
BETA_CONFIG=config/beta_ext_bm_config.json \
    ./scripts/run_baremetal.sh l0_tests/build/l0_04_pa.elf
```

> **注**：`beta_ext_bm_config.json` 同时包含 SRAM `0xF0000000`（512KB）和 DDR `0x80000000`（128MB），与 l0_tests 的链接脚本匹配。

### 4.3 预期输出

```
=== L0-01: TSO Litmus Test ===
Iterations per test: 10000000
  diagonal (C0-C7) x SB: violations=0 [PASS]
  diagonal (C0-C7) x MP: violations=0 [PASS]
  ...
=== SUMMARY: 15 tests, total violations=0 ===
[PASS] L0-01 TSO Litmus
```

每个用例最后输出 `[PASS]` 或 `[FAIL]`，`[FAIL]` 时附带失败子测试名称和计数。

---

## 5. 调试技巧

### GDB 连接

```bash
# 启动 QEMU，暂停等待调试器
./scripts/run_bm.sh tests/bm_0407_qemu.elf -s -S

# 另一个终端
riscv64-linux-gnu-gdb tests/bm_0407_qemu.elf
(gdb) target remote :1234
(gdb) b main
(gdb) c
```

### 导出 DTB 验证地址空间

```bash
./qemu-10.0.2-beta/build/qemu-system-riscv64 \
    -machine beta,config-file=config/beta_bm_run.json,dumpdtb=/tmp/bm.dtb \
    -nographic
dtc -I dtb -O dts /tmp/bm.dtb | less
```

### 多核裸机（2 hart）

`beta_bm_run.json` 默认配置 2 个 ACPU（1 cluster × 2 cores，hartid 0 和 1）。boot.S 中 hartid ≠ 0 的核会在 `.wait` 循环自旋，主核通过共享内存或 CLINT IPI 唤醒从核。

如需单核调试，可临时将配置改为 `"cores_per_cluster": 1`（无需重新编译 QEMU）。
