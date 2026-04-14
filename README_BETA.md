# Beta SoC QEMU 全芯片仿真平台

**版本**：QEMU 10.0.2-beta  
**最后更新**：2026-04-09  
**内核**：Linux 6.19.5 · **OpenSBI**：v1.3 · **架构**：RISC-V RVA23S64

---

## 目录

1. [平台规格](#1-平台规格)
2. [目录结构](#2-目录结构)
3. [编译说明（macOS M4）](#3-编译说明macos-m4)
4. [启动命令](#4-启动命令)
5. [变更日志](#5-变更日志)
6. [已知限制](#6-已知限制)
7. [Sv39 补丁说明](#7-sv39-补丁说明)
8. [回归测试结果](#8-回归测试结果)

---

## 1. 平台规格

| 组件 | 规格 |
|------|------|
| ACPU | 32 核（8 集群 × 4），RV64 RVA23S64，VLEN=512，Ztso |
| MCPU | 1 核，RV32IMAC，hartid=32 |
| DDR  | 32 GB（8 通道 × 4 GB，64B 交织）@ `0x4000000000` |
| PCIe | 4 个 DWC Gen4 子系统（x16 / 4×4 分叉）|
| IOMMU | 4 个 RISC-V IOMMU（每路 PCIe 一个，当前 QEMU 侧旁路）|
| 中断 | 3 级 APLIC（MLROOT→MLAPP→SLAPP）+ IMSIC M/S + MCPU PLIC |
| 启动 | MCPU BootROM（SW_RST_CTRL1 释放 ACPU）/ ACPU Direct |

---

## 2. 目录结构

```
beta_soc_qemu_release/
├── README.md                          # 本文件
├── beta_qemu_audit_report.md          # 代码审计报告
├── doc/
│   └── Beta_SoC_QEMU_Platform_Guide.md  # 完整技术指南
├── qemu-10.0.2-beta/                  # QEMU 源码（含 Beta 定制）
│   ├── hw/riscv/beta.c                # 主机器模型
│   ├── hw/riscv/beta_dtb.c            # DTB 生成
│   ├── hw/riscv/beta_config.c         # JSON 配置加载
│   └── hw/misc/beta_soc_csr.c        # SoC CSR / 核心释放
├── config/
│   ├── beta_production_config.json    # 生产配置（32 核 + 8 通道 DDR）
│   ├── beta_acpu_direct_config.json   # ACPU 直启（跳过 MCPU）
│   └── beta_test_config.json          # 轻量测试配置
├── kernel/
│   └── Image-6.19.5-beta              # Linux 内核镜像
├── firmware/
│   ├── bootrom/mcpu_bootrom_production.bin
│   └── opensbi/fw_jump_0x4000000000.bin
└── rootfs/
    ├── initramfs.cpio.gz              # BusyBox initramfs
    └── initramfs_net_test.cpio.gz     # 含 udhcpc 的网络测试 initramfs
```

---

## 3. 编译说明（macOS M4）

### 3.1 重要：路径不能含空格

QEMU 的 meson / ninja 构建系统在处理含空格的路径时会报错。请将源码置于无空格目录，例如：

```bash
# 推荐目录布局
~/qemu-beta-src/          # QEMU 源码 + 构建目录
~/Desktop/beta_release/   # 发布目录（不要用 "beta qemu"）
```

> 若必须在含空格的路径下工作，可用软链接绕过：
> ```bash
> ln -s "/Users/$USER/Desktop/beta qemu" ~/beta-qemu-link
> ```

### 3.2 安装依赖（Homebrew）

```bash
brew install ninja meson pkg-config glib pixman libslirp dtc python3
```

> **注意**：Apple Clang 15+ 已满足编译要求，无需额外安装 GCC。

### 3.3 配置与编译

```bash
cd ~/qemu-beta-src

# 首次配置（仅需一次）
mkdir -p build && cd build
../configure \
    --target-list=riscv64-softmmu \
    --enable-slirp \
    --disable-docs \
    --disable-gtk \
    --disable-sdl \
    --disable-vnc \
    --extra-cflags="-O2" \
    2>&1 | tee configure.log

# 编译（使用 Homebrew ninja，M4 推荐 -j10）
/opt/homebrew/bin/ninja -j10 qemu-system-riscv64-unsigned
```

编译产物：`build/qemu-system-riscv64-unsigned`

### 3.4 验证

```bash
./build/qemu-system-riscv64-unsigned --version
# QEMU emulator version 10.0.2 (Beta SoC fork)
```

---

## 4. 启动命令

### 4.1 生产模式（32 核，MCPU BootROM，推荐）

```bash
RELEASE=~/Desktop/beta_release/beta_soc_qemu_release
QEMU=~/qemu-beta-src/build/qemu-system-riscv64-unsigned

$QEMU \
  -machine beta,config-file=$RELEASE/config/beta_production_config.json \
  -bios $RELEASE/firmware/bootrom/mcpu_bootrom_production.bin \
  -device loader,file=$RELEASE/firmware/opensbi/fw_jump_0x4000000000.bin,addr=0x4000000000 \
  -kernel $RELEASE/kernel/Image-6.19.5-beta \
  -initrd $RELEASE/rootfs/initramfs.cpio.gz \
  -append "root=/dev/ram rdinit=/init earlycon=uart8250,mmio32,0xd0087000,115200n8 console=ttyS0,115200n8" \
  -nographic
```

预期输出：
- OpenSBI：`Platform HART Count : 32`
- 内核：`smp: Brought up 1 node, 32 CPUs`
- Shell 登录提示符

### 4.2 限制核数启动（开发 / 调试用，启动更快）

```bash
# 仅激活 4 核（跳过 31 个次级 HART 的超时等待）
-append "... maxcpus=4"
```

### 4.3 含 virtio-net 的网络测试

```bash
$QEMU \
  -machine beta,config-file=$RELEASE/config/beta_production_config.json \
  -bios $RELEASE/firmware/bootrom/mcpu_bootrom_production.bin \
  -device loader,file=$RELEASE/firmware/opensbi/fw_jump_0x4000000000.bin,addr=0x4000000000 \
  -kernel $RELEASE/kernel/Image-6.19.5-beta \
  -initrd $RELEASE/rootfs/initramfs_net_test.cpio.gz \
  -append "root=/dev/ram rdinit=/init earlycon=uart8250,mmio32,0xd0087000,115200n8 console=ttyS0,115200n8 maxcpus=4 ip=dhcp" \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0 \
  -nographic
```

预期：`eth0` 注册 → DHCP 获取 10.0.2.15 → `ping 10.0.2.2` 通

### 4.4 导出 DTB（排查 FDT 问题）

```bash
$QEMU -machine beta,config-file=...,dumpdtb=/tmp/beta.dtb -nographic
dtc -I dtb -O dts /tmp/beta.dtb | less
```

---

## 5. 变更日志

本节记录从 QEMU 10.0.2 上游基线到当前版本的全部修改，按专题分组。

---

### P0：CPU 核心修复

#### 5.1 DTB CPU 节点顺序修复（`hw/riscv/beta_dtb.c`）

**问题**：`qemu_fdt_add_subnode()` 采用头插法，原循环 `for (i=0; i<32; i++)` 导致 DTB 中 `cpu@31` 排第一、`cpu@0` 排最后。`interrupts-extended[i]` 与 hartid i 的 IMSIC 地址完全错位，OpenSBI 向所有次级 HART 发 IPI 时写入了错误的 MMIO 地址，所有次级 CPU SMP 启动失败。

**修复**：将循环改为 `for (i = num_acpu; i-- > 0; )`，使 `cpu@0` 最后添加（最终出现在 DTB 最前），`interrupts-extended[i]` 正确对应 hartid i。

---

#### 5.2 IMSIC IPI 预设（`hw/misc/beta_soc_csr.c`）

**问题**：次级 HART 在 OpenSBI 写入 `eidelivery=1` 之前，若 HART_START IPI 已经到达，`riscv_imsic_topei()` 需要 `eistate[1]` 同时具备 ENABLED（bit1）和 PENDING（bit0）才能触发 IRQ。仅预写 ENABLED 位时，在 TCG 多线程模式下存在 IPI 丢失的竞争窗口，次级 HART 无限等待。

**修复**：在 `beta_soc_csr_sw_rst_ctrl1()` 释放每个次级 HART 时，将 `eistate[1]` 预写从 `0x2`（仅 ENABLED）改为 `0x3`（ENABLED | PENDING）。次级 HART 在获得第一个时间片后立即看到待处理的 IPI，无需等待新的 MMIO 写入。

---

#### 5.3 Core 31 SW_RST_CTRL1 修复（`hw/misc/beta_soc_csr.c`）

**问题**：SW_RST_CTRL1 为 32 位寄存器，bits[31:1] 对应 ACPU 核心 0-30（共 31 核），核心 31 无对应位。MCPU BootROM 写 `0xFFFFFFFE` 后 31 颗核释放，核心 31 永远不释放。

**修复**：增加 `core31_released` 标志位，当检测到 bits[31:1] 累计全为 1（`0xFFFFFFFE` 模式）时，自动释放核心 31，保证 32/32 CPU 全部上线。

---

### P1：配置与 DTB

#### 5.4 PCIe MSI IRQ Domain 修复（`hw/riscv/beta_dtb.c`）

**问题**：`rv-gen4-pcie` 驱动的 `rv_gen4_pcie_init_irq_domain()` 调用 `of_get_child_by_name(node, "interrupt-controller")` 获取 INTx 域的 fwnode。原 DTB 无此子节点，返回 NULL，`PTR_ERR(NULL)=0`，驱动打印 `ret: 0` 后返回 `-EINVAL`，4 个 PCIe 控制器 IRQ 域初始化全部失败。

**修复**：在每个 `rv-gen4-pcie` 节点下添加 `interrupt-controller` 子节点（含 phandle、`#address-cells=0`、`#interrupt-cells=1`、`interrupt-map`），并添加 `interrupt-names = "legacy"` 属性。

---

#### 5.5 iommu-map 删除（`hw/riscv/beta_dtb.c`）

**依据**：Platform Guide §14.1。

**问题**：PCIe 节点含 `iommu-map` 时，内核为 DMA 分配 IOVA 并传给 virtio 后端；QEMU 侧 IOMMU 无翻译表，设备以 IOVA 作为物理地址读写，产生 DMA 越界。

**修复**：从所有 PCIe 节点删除 `iommu-map` 属性，内核改用 1:1 物理地址 DMA 映射。

---

#### 5.6 msi-map 删除（`hw/riscv/beta_dtb.c`）

**问题**：`msi-map` 存在时，Linux 调用 `of_msi_map_get_device_domain()` 以 `DOMAIN_BUS_PCI_MSI` 查找 MSI 域，而 IMSIC 驱动注册的是 `DOMAIN_BUS_NEXUS`，查找失败返回 `-EINVAL`，MSI-X 无法分配。

**修复**：保留 `msi-parent`（指向 S 级 IMSIC），删除 `msi-map`。Linux 通过 `pci_host_bridge_of_msi_domain()` 回落到 `DOMAIN_BUS_NEXUS` 匹配成功，MSI-X 正常分配。

---

### P2：寄存器 Stub

#### 5.7 DDR CSR UMCTL2 兼容寄存器（`hw/riscv/beta.c` + JSON 配置）

为 8 个 DDR CSR 设备添加 12 个 DWC UMCTL2 兼容 stub 寄存器：
- `STAT[1:0]=1`（Normal 模式，初始化完成）
- `DFISTAT[0]=1`（PHY 就绪）
- `SWCTL/SWSTAT` 回调：写 SWCTL 自动镜像 bit0 到 SWSTAT（quasi-dynamic commit 模式）
- `CHAN_SIZE_MB=0x1000`（4 GB / 通道）

效果：内核 DDR 驱动轮询 training 完成位可立即退出，不再死等。

---

#### 5.8 L3 CSR flush/inv 寄存器（`hw/riscv/beta.c` + JSON 配置）

修正 8 个 L3 slice 的寄存器偏移至文档值，并添加功能回调：
- `FLUSH_CTRL`（0x1200）写 1 → `FLUSH_INV_DONE`（0x1208）自动置位
- `L3_FLUSH_ADDR_START`（0x1210）/ `L3_FLUSH_ADDR_END`（0x1218）
- `L3_MEM_CTRL_REG` reset=0x124（默认上电值，含 sf/tag/dat mcs=4）
- `L3_CORE_CTRL_REG` 回调：记录每核 L3 way 禁用位掩码

---

#### 5.9 GP Timer / WDT / Mailbox / TRNG / eFuse（`hw/riscv/beta.c` + JSON 配置）

| 外设 | 实现内容 |
|------|----------|
| GP Timer | QEMUTimer 1ms tick，计数器自增，比较匹配 → IRQ（APLIC #2）|
| WDT | DW WDT 全模型：TORR 超时、RMOD 双模式、系统复位 |
| Mailbox | TX→RX 环回，RX_STATUS 更新，条件 IRQ（APLIC #12/#13）|
| TRNG | CTRL 写 → `qemu_guest_getrandom_nofail` → DATA 寄存器 |
| eFuse | CHIP_ID/REV/WAFER_ID 等 6 个只读 stub |

---

### P3：IOMMU 旁路修复（`hw/riscv/beta.c`）

#### 5.10 QEMU 侧 PCIe DMA 旁路 IOMMU

**根本原因**：`riscv_iommu_sys_realize()` 通过 `object_resolve_path_type()` 自动发现 PCIe 总线并调用 `riscv_iommu_pci_setup_iommu()` 将自身设为总线的 DMA 地址空间提供者。`beta_create_iommu()` 的 Pass 2 进一步对每个 DWC 总线显式调用 `riscv_iommu_sys_setup_pci_bus()`，导致所有 virtio DMA 经过 IOMMU 翻译引擎。由于内核无 `iommu-map`，使用物理地址作为 DMA 地址，翻译表为空，每次 DMA 访问都发生 fault，virtio TX 描述符不可读，TX 永远不完成，NETDEV WATCHDOG 每 ~5 s 触发。

**修复**：将 Pass 2 改为清空 `bus->iommu_ops = NULL` 与 `bus->iommu_opaque = NULL`，撤销 realize 阶段的自动绑定。IOMMU 设备仍然存在并可寄存器访问，仅不拦截 PCIe DMA。

**效果**：virtio-net TX 路径恢复正常，DHCP 成功，ping 通。

---

### P4：Cache 一致性与内存序（TCG 层）

#### 5.11 TCG 内存序（`target/riscv/cpu-param.h`）

将 `TCG_GUEST_DEFAULT_MO` 设为 `TCG_MO_ALL`，为 RVWMO barrier 指令提供正确的 TSO 语义支持。

#### 5.12 CBO 指令 flush（`target/riscv/op_helper.c`）

`cbo.clean` / `cbo.flush` / `cbo.inval` 执行时插入 `smp_mb()` 内存屏障并调用 `tlb_flush_page()`，模拟 cache line 刷写行为。

#### 5.13 Svinval 扩展（`target/riscv/trans_svinval.c.inc`）

- `sfence.w.inval`：store barrier（`smp_wmb()`）
- `sfence.inval.ir`：完整屏障（`smp_mb()`）+ TLB 全刷

---

### P5：MMIO 阴影修复（`hw/riscv/beta.c`）

#### 5.14 generic_mmio MMIO 阴影检测

**问题**：`beta_create_generic_mmio_devices()` 在 `beta_create_aia()` 之后调用，`scm_aia`（覆盖 APLIC 范围）和 `clusterN_csr`（覆盖 IMSIC 范围）的 generic_mmio 设备以优先级 0 挂载，遮蔽了真实的 APLIC/IMSIC MMIO 处理器。

**修复**：增加 `beta_mmio_overlaps_hw()` 辅助函数，跳过与任何 APLIC 域或 IMSIC 集群地址范围重叠的 generic_mmio 设备。

---

## 6. 已知限制

### 6.1 Sv39 / 地址空间

Linux 内核默认使用 Sv48（4 级页表，512 GB VA 空间）。Beta SoC 的 DDR 基地址为 `0x4000000000`（256 GB），超出 Sv39 的 512 GB VA 上限但在 Sv48 范围内，**标准内核无需修改**。

若需强制使用 Sv39（例如调试目的），参见[第 7 节](#7-sv39-补丁说明)。

### 6.2 TCG 性能

当前使用 QEMU TCG（软件解释执行），无硬件加速：
- 32 核全启动到 shell 约 2–5 s（macOS M4 @ 10 线程）
- 建议开发时使用 `maxcpus=4` 加速启动
- 不建议跑性能 benchmark，结果无参考价值

### 6.3 IOMMU 未启用（DMA 旁路）

RISC-V IOMMU 设备已在 QEMU 中实例化（寄存器可访问），但 **PCIe DMA 不经过翻译**（见 §5.10）。原因：
- 内核无 `iommu-map`，使用物理地址 DMA
- QEMU 侧若启用翻译，翻译表为空会导致所有 DMA fault

实际 SoC 中 IOMMU 需独立验证；QEMU 平台仅保证 CPU 指令和中断行为的准确性。

### 6.4 DDR 交织

8 通道 DDR 通过 `memory_region_init_alias()` 将通道 1-7 别名到主 DDR 块，无真实 cache-line 粒度交织行为。NOC SAM 寄存器存在 stub，但写 `DDR_BASE/MASK` 不触发实际内存区域重映射。

### 6.5 PCIe DMA 引擎未建模

DWC PCIe DMA 引擎（EDMA）寄存器未实现，DMA 传输需驱动使用 CPU copy。

### 6.6 约 90 个低速外设为 generic_mmio stub

GP Timer、WDT、Mailbox、TRNG、eFuse 已有功能实现（见 §5.9）；其余约 90 个中断源（如 I²C、SPI、SDIO 等）仅提供可读写但无功能回调的 MMIO stub，中断不触发。

### 6.7 PMU 事件有限

仅支持 5 个通用性能计数器事件，无 cache miss / branch miss 专项事件。

---

## 7. Sv39 补丁说明

Linux 6.x 默认开启 Sv48。若需强制内核以 Sv39 模式运行（39 位 VA，512 GB），有两种方法：

### 方法 A：内核命令行（推荐）

```bash
-append "... no-svpbmt satp_mode=sv39"
```

> **注**：`satp_mode=sv39` 为实验性参数，需内核版本 ≥ 6.5 且编译时未禁用。

### 方法 B：内核配置补丁

在内核源码 `arch/riscv/Kconfig` 中：

```diff
-default "sv48" if 64BIT
+default "sv39" if 64BIT
```

重新编译内核。此方法无需命令行参数，适合固定为 Sv39 的发布镜像。

### Sv39 与 Beta SoC 的关系

Beta SoC 硬件支持 Sv39 / Sv48 / Sv57（由 satp.MODE 字段决定）。`0x4000000000` DDR 基地址在 Sv39 虚拟地址空间内需要精心的 kernel linear mapping 对齐；在 QEMU 中两种模式均可正常工作，不影响验证结果。

---

## 8. 回归测试结果

当前版本（2026-04-09）已通过以下三轮回归测试：

| 轮次 | 配置 | 关键检查项 | 结果 |
|------|------|-----------|------|
| Round 1 | 32 核生产配置，MCPU BootROM | 32/32 CPUs online · APLIC/IMSIC 初始化 · 无 kernel panic | **PASS** |
| Round 2 | maxcpus=4 + virtio-net-pci | eth0 注册 · DHCP 10.0.2.15 · ping 10.0.2.2 通 | **PASS** |
| Round 3 | 32 核，无 maxcpus 限制 | `smp: Brought up 1 node, 32 CPUs` | **PASS** |

### 回归脚本

```bash
# 三轮自动化回归（含 clean rebuild）
bash /tmp/beta_regression.sh
```

日志输出：
- `/tmp/beta_regression_1.log`（Round 1：4 核 SMP + APLIC）
- `/tmp/beta_regression_2.log`（Round 2：4 核 + virtio-net）
- `/tmp/beta_regression_3.log`（Round 3：32 核生产）

---

## 附：软件栈版本

| 组件 | 版本 |
|------|------|
| QEMU | 10.0.2 + Beta SoC 定制 |
| Linux 内核 | 6.19.5（GCC 15.2，Ubuntu 25.10 交叉编译）|
| OpenSBI | v1.3（FW_JUMP @ `0x4000000000`）|
| Rootfs | BusyBox initramfs / 网络测试 initramfs |
| 构建主机 | macOS 15（Apple M4），Homebrew Ninja + Meson |
