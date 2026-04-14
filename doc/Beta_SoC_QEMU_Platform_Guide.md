# Beta SoC QEMU 平台技术手册

版本: 3.0
日期: 2026-04-09
基于: QEMU 10.0.2 + Beta SoC Machine Model

---

## 目录

1. [平台概述](#1-平台概述)
2. [CPU 架构](#2-cpu-架构)
3. [完整内存映射](#3-完整内存映射)
4. [中断架构](#4-中断架构)
5. [PCIe 子系统](#5-pcie-子系统)
6. [MCPU 子系统](#6-mcpu-子系统)
7. [启动流程](#7-启动流程)
8. [JSON 配置格式](#8-json-配置格式)
9. [FDT 生成](#9-fdt-生成)
10. [DWC DesignWare PCIe 修改](#10-dwc-designware-pcie-修改)
11. [rv-gen4 PCIe 驱动修改](#11-rv-gen4-pcie-驱动修改)
12. [构建指南](#12-构建指南)
13. [运行指南](#13-运行指南)
14. [已知问题和限制](#14-已知问题和限制)
15. [文件参考](#15-文件参考)

---

## 1. 平台概述

### 1.1 Beta SoC 简介

Beta SoC 是一款高性能 RISC-V 处理器, 采用异构多核架构, 包含 32 个 RV64 应用处理器核心 (ACPU) 和 1 个 RV32 管理处理器核心 (MCPU). ACPU 采用 RVA23S64 Profile, 支持 VLEN=512 的向量扩展和 Ztso 全序内存模型. MCPU 负责系统初始化、固件加载和核心释放.

### 1.2 QEMU 平台仿真范围

本 QEMU 平台模型 (machine type: `beta`) 仿真了 Beta SoC 的以下硬件:

- 32 个 ACPU (RV64, RVA23S64 profile, VLEN=512, Ztso) + 1 个 MCPU (RV32IMAC)
- 8 通道 DDR 内存 (8 x 4GB = 32GB, 64 字节交织)
- 3 级 APLIC 中断控制器层次 (MLROOT -> MLAPP -> SLAPP)
- 每核 IMSIC (M-level + S-level, 255 中断 ID, 4 个 VS guest 文件)
- ACLINT (MSWI + MTIMER)
- 4 个 Synopsys DesignWare PCIe 控制器子系统 (x16, 支持 4x4 分叉)
- 4 个 RISC-V IOMMU (每个 PCIe 子系统一个)
- UART (ns16550a)
- SOC_TOP_CSR (带核心释放机制)
- MCPU 子系统: PLIC、Platform Timer、AHB Master Window
- NOR Flash (pflash_cfi01)
- 通用 MMIO 存根设备 (I2C, SPI, GPIO, Mailbox, Watchdog, DDR 控制器等)

### 1.3 架构示意图

```
+================================================================+
|                     Beta SoC QEMU Platform                      |
+================================================================+
|                                                                  |
|  +------------------+     +-----------------------------------+ |
|  |     MCPU (RV32)  |     |        ACPU Cluster Array         | |
|  |    hartid = 32   |     |  Cluster 0: core 0-3  (hartid 0-3)| |
|  |    RV32IMAC      |     |  Cluster 1: core 4-7  (hartid 4-7)| |
|  |                  |     |  Cluster 2: core 8-11             | |
|  |  +-----------+   |     |  Cluster 3: core 12-15            | |
|  |  | MCPU PLIC |   |     |  Cluster 4: core 16-19            | |
|  |  +-----------+   |     |  Cluster 5: core 20-23            | |
|  |  | Platform  |   |     |  Cluster 6: core 24-27            | |
|  |  | Timer     |   |     |  Cluster 7: core 28-31            | |
|  |  +-----------+   |     |  (hartid 28-31)                   | |
|  |  | AHB Master|   |     |  RV64, RVA23S64, VLEN=512, Ztso  | |
|  |  | Window    |   |     +-----------------------------------+ |
|  |  +-----------+   |                    |                       |
|  +--------|---------+                    |                       |
|           |                              |                       |
|     +-----|------------------------------|---------+             |
|     |              System Bus (40-bit PA)          |             |
|     +----------------------------------------------+             |
|        |         |         |        |        |                   |
|   +---------+ +------+ +------+ +------+ +------+               |
|   |SOC_TOP  | |ACLINT| |APLIC | |IMSIC | | UART |               |
|   |CSR      | |MSWI  | |3-level| |M+S  | |ns16550|              |
|   |0x01_0000| |MTIMER| |MLROOT| |per   | |      |               |
|   |  _0000  | |      | |MLAPP | |hart  | |      |               |
|   |         | |      | |SLAPP | |      | |      |               |
|   +---------+ +------+ +------+ +------+ +------+               |
|        |                                                         |
|   +----+-------+-------+-------+                                 |
|   | PCIe0      | PCIe1 | PCIe2 | PCIe3 |                        |
|   | DWC x16    | DWC   | DWC   | DWC   |                        |
|   | +IOMMU0    | +IOMMU1| +IOMMU2| +IOMMU3|                     |
|   +------------+-------+-------+--------+                        |
|        |         |       |       |                               |
|   +----+----+----+---+---+---+---+---+                           |
|   | DDR Ch0 | Ch1    | Ch2   |...| Ch7  | (8 x 4GB = 32GB)     |
|   | 0x40_   | 0x50_  | 0x60_ |   |0xB0_ |                      |
|   | 0000_   | 0000_  | 0000_ |   |0000_ |                      |
|   | 0000    | 0000   | 0000  |   |0000  |                       |
|   +---------+--------+-------+---+------+                        |
+==================================================================+
```

### 1.4 仿真限制

- DDR 内存交织: 在 QEMU 中实现为单块连续内存, 不模拟真实的 8 通道 64 字节交织行为
- PCIe PHY: 链路训练不被仿真, 通过 APPL 寄存器硬编码 link-up 状态
- Cache 层次: L1/L2/L3 缓存不被仿真, Zicbom/Zicbop/Zicboz 指令模拟为内存屏障+TLB flush
- DDR 控制器: 仅提供寄存器存根（DWC UMCTL2 兼容，含 STAT/SWCTL/SWSTAT 回调）, 不模拟时序和校准
- IOMMU DMA 路径: IOMMU 设备已创建（寄存器可访问）, 但 PCIe DMA 旁路 IOMMU（iommu_ops=NULL）, PCI DMA 使用 1:1 物理地址映射. 详见 §14.1
- PCIe 端点: 无真实 PCIe 端点设备, config space 读取返回 0xFFFFFFFF (无设备)
- L3_MEM_CTRL_REG: 保留, 当前制程不支持
- Sv39 DDR 映射: 稀疏 DDR 跨度（452 GB）超出 Sv39 线性映射窗口, 需加 `mem=2G` 或使用 Sv39 patch. 详见 §14.6
- TCG 性能: macOS M4 上 32 核 SMP bringup 约需 7 分钟（无 KVM 加速）, 调试建议使用 `maxcpus=4`

---

## 2. CPU 架构

### 2.1 ACPU (应用处理器)

| 属性 | 值 |
|------|-----|
| 核心数 | 32 (8 clusters x 4 cores) |
| ISA | RV64IMAFDCV (RVA23S64 Profile) |
| 向量扩展 | VLEN=512, ELEN=64, vlenb=64 |
| 内存模型 | Ztso (Total Store Ordering) |
| MMU | Sv39 |
| Hartid | 0-31 |
| 复位向量 | 0x40_0000_0000 (DDR0 基地址) |
| H 扩展 | 支持 (Hypervisor) |
| PMU | 支持 (Sscofpmf) |

**集群拓扑:**

```
Cluster 0: hartid 0,  1,  2,  3
Cluster 1: hartid 4,  5,  6,  7
Cluster 2: hartid 8,  9,  10, 11
Cluster 3: hartid 12, 13, 14, 15
Cluster 4: hartid 16, 17, 18, 19
Cluster 5: hartid 20, 21, 22, 23
Cluster 6: hartid 24, 25, 26, 27
Cluster 7: hartid 28, 29, 30, 31
```

Hartid 分配公式: `hartid = cluster_id * 4 + core_id_within_cluster`

### 2.2 MCPU (管理处理器)

| 属性 | 值 |
|------|-----|
| 核心数 | 1 |
| ISA | RV32IMAC |
| Hartid | 32 |
| ITCM | 64 KB |
| DTCM | 64 KB |
| SRAM | 512 KB |
| 复位向量 | 0x00_D000_0000 (MCPU ROM) |

MCPU 在 Linux 视角中不可见 (ACPU FDT 中不包含 MCPU 节点). MCPU 有独立的 FDT 树, 运行独立的管理固件.

### 2.3 完整 ISA 扩展列表

以下是 Beta SoC ACPU 支持的全部 ISA 扩展, 来源于 `beta_dtb.c` 中的 `isa_exts[]` 数组 (共 65 个扩展):

**基础 ISA + MISA:**

| 扩展 | 描述 |
|------|------|
| `i` | 整数基础指令集 |
| `m` | 整数乘除法 |
| `a` | 原子操作 |
| `f` | 单精度浮点 |
| `d` | 双精度浮点 |
| `c` | 压缩指令 |
| `v` | 向量扩展 (VLEN=512) |
| `h` | Hypervisor 扩展 |
| `b` | 位操作 (Zba+Zbb+Zbs 合集) |

**特权架构扩展:**

| 扩展 | 描述 |
|------|------|
| `zicsr` | CSR 访问指令 |
| `zifencei` | 指令-数据一致性 fence |
| `zicntr` | 基础计数器 (cycle, time, instret) |
| `zihpm` | 硬件性能监控计数器 |

**AIA + 定时器:**

| 扩展 | 描述 |
|------|------|
| `smaia` | M-mode 高级中断架构 |
| `ssaia` | S-mode 高级中断架构 |
| `sstc` | Supervisor 定时器比较 (stimecmp) |

**位操作:**

| 扩展 | 描述 |
|------|------|
| `zba` | 地址生成位操作 |
| `zbb` | 基础位操作 |
| `zbs` | 单位位操作 |

**标量密码学:**

| 扩展 | 描述 |
|------|------|
| `zkt` | 常数时间执行 |

**浮点扩展:**

| 扩展 | 描述 |
|------|------|
| `zfhmin` | 半精度浮点最小支持 |
| `zfa` | 浮点附加指令 |

**Cache 块操作:**

| 扩展 | 描述 |
|------|------|
| `zicbom` | Cache 块管理操作 (block size=64) |
| `zicbop` | Cache 块预取 (block size=64) |
| `zicboz` | Cache 块清零 (block size=64) |

**提示和原子扩展:**

| 扩展 | 描述 |
|------|------|
| `zihintpause` | PAUSE 提示指令 |
| `zihintntl` | Non-temporal locality 提示 |
| `zawrs` | Wait-on-Reservation-Set |

**向量子扩展:**

| 扩展 | 描述 |
|------|------|
| `zvfhmin` | 向量半精度浮点最小支持 |
| `zvbb` | 向量基础位操作 |
| `zvkt` | 向量常数时间执行 |

**压缩扩展:**

| 扩展 | 描述 |
|------|------|
| `zcb` | 简单压缩位操作 |
| `zcmop` | 压缩 may-be-operations |

**条件和 may-be-ops:**

| 扩展 | 描述 |
|------|------|
| `zicond` | 整数条件操作 |
| `zimop` | May-be-operations |

**Supervisor 扩展:**

| 扩展 | 描述 |
|------|------|
| `svnapot` | NAPOT 页面大小 |
| `svpbmt` | 基于页表的内存类型 |
| `svinval` | 细粒度地址空间无效化 |
| `svade` | 硬件 A/D 位更新异常 |
| `sscofpmf` | 计数器溢出和权限模式过滤 |
| `ssnpm` | Supervisor 自然对齐指针屏蔽 |
| `supm` | User 指针屏蔽 |

**命名特性 (Cache/内存模型):**

| 扩展 | 描述 |
|------|------|
| `zic64b` | Cache 行大小为 64 字节 |
| `ziccif` | Cache 一致性指令获取 |
| `ziccrse` | Cache 一致性相同有效性 |
| `ziccamoa` | Cache 一致性 AMO 对齐 |
| `zicclsm` | Cache 一致性 Load/Store 混合大小 |
| `za64rs` | 64 字节保留集对齐 |

**特权命名特性:**

| 扩展 | 描述 |
|------|------|
| `ss1p13` | Supervisor 规范版本 1.13 |
| `svbare` | 裸地址翻译模式 |
| `sv39` | 39 位虚拟地址 |
| `ssccptr` | 主存具有缓存一致性点 |
| `sstvecd` | stvec 支持 Direct 模式 |
| `sstvala` | stval 提供异常信息 |
| `sscounterenw` | scounteren 可写 |
| `ssu64xl` | UXLEN=64 |

**Hypervisor 命名特性:**

| 扩展 | 描述 |
|------|------|
| `ssstateen` | Supervisor State Enable |
| `shcounterenw` | hcounteren 可写 |
| `shvstvala` | hstval 提供 VS 异常信息 |
| `shtvala` | htval 提供异常信息 |
| `shvstvecd` | vstvec 支持 Direct 模式 |
| `shvsatpa` | vsatp 支持相同模式 |
| `shgatpa` | hgatp 支持 |

**Beta 专有:**

| 扩展 | 描述 |
|------|------|
| `ztso` | Total Store Ordering 内存模型 |

---

## 3. 完整内存映射

### 3.1 ACPU 视角 (40-bit 物理地址空间)

| 地址 | 大小 | 设备 | 描述 |
|------|------|------|------|
| `0x00_D000_0000` | 64 KB | MCPU ROM | MCPU BootROM |
| `0x00_D004_0000` | 64 KB | APLIC MLROOT | APLIC 根域 (直接传递) |
| `0x00_D005_0000` | 64 KB | APLIC MLAPP | APLIC M-level 域 (MSI) |
| `0x00_D006_0000` | 64 KB | APLIC SLAPP | APLIC S-level 域 (MSI) |
| `0x00_D008_7000` | 4 KB | UART0 | ns16550a 串口 |
| `0x00_D800_0000` | 128 MB | BT_SPI_XIP | Boot SPI 就地执行 Flash |
| `0x00_F000_0000` | 512 KB | MCPU SRAM | MCPU 工作内存 |
| `0x01_0000_0000` | 64 KB | SOC_TOP_CSR | 系统顶层控制/状态寄存器 |
| `0x01_1020_0000` | per-hart | IMSIC M-level | 每核 M-mode IMSIC |
| `0x01_1040_0000` | per-hart | IMSIC S-level | 每核 S-mode IMSIC (含 VS guest) |
| `0x01_2000_0000` | 64 MB | DDR0 控制器 | DDR 通道 0 控制寄存器 |
| `0x01_2400_0000` | 64 MB | DDR1 控制器 | DDR 通道 1 控制寄存器 |
| `0x01_2800_0000` | 64 MB | DDR2 控制器 | DDR 通道 2 控制寄存器 |
| `0x01_2C00_0000` | 64 MB | DDR3 控制器 | DDR 通道 3 控制寄存器 |
| `0x01_3000_0000` | 64 MB | DDR4 控制器 | DDR 通道 4 控制寄存器 |
| `0x01_3400_0000` | 64 MB | DDR5 控制器 | DDR 通道 5 控制寄存器 |
| `0x01_3800_0000` | 64 MB | DDR6 控制器 | DDR 通道 6 控制寄存器 |
| `0x01_3C00_0000` | 64 MB | DDR7 控制器 | DDR 通道 7 控制寄存器 |
| `0x01_4000_0000` | 3 MB | PCIe0 DBI | PCIe 子系统 0 DWC 寄存器 |
| `0x01_4500_0000` | 1 MB | PCIe0 MGMT | PCIe 子系统 0 管理层寄存器 |
| `0x01_4600_0000` | 8 MB | PCIe0 APPL | PCIe 子系统 0 APPL 寄存器 |
| `0x01_4800_0000` | 3 MB | PCIe1 DBI | PCIe 子系统 1 DWC 寄存器 |
| `0x01_4D00_0000` | 1 MB | PCIe1 MGMT | PCIe 子系统 1 管理层寄存器 |
| `0x01_4E00_0000` | 8 MB | PCIe1 APPL | PCIe 子系统 1 APPL 寄存器 |
| `0x01_5000_0000` | 3 MB | PCIe2 DBI | PCIe 子系统 2 DWC 寄存器 |
| `0x01_5500_0000` | 1 MB | PCIe2 MGMT | PCIe 子系统 2 管理层寄存器 |
| `0x01_5600_0000` | 8 MB | PCIe2 APPL | PCIe 子系统 2 APPL 寄存器 |
| `0x01_5800_0000` | 3 MB | PCIe3 DBI | PCIe 子系统 3 DWC 寄存器 |
| `0x01_5D00_0000` | 1 MB | PCIe3 MGMT | PCIe 子系统 3 管理层寄存器 |
| `0x01_5E00_0000` | 8 MB | PCIe3 APPL | PCIe 子系统 3 APPL 寄存器 |
| `0x06_0000_0000` | 1 MB | ACLINT | MSWI + MTIMER (ACPU 32 核) |
| `0x07_0000_0000` | 8 KB | AXI Bridge 0 | AXI 桥 0 CSR |
| `0x07_0000_1800` | 4 KB | IOMMU 0 | PCIe0 RISC-V IOMMU |
| `0x07_0000_2000` | 8 KB | AXI Bridge 1 | AXI 桥 1 CSR |
| `0x07_0000_3800` | 4 KB | IOMMU 1 | PCIe1 RISC-V IOMMU |
| `0x07_0000_4000` | 8 KB | AXI Bridge 2 | AXI 桥 2 CSR |
| `0x07_0000_5800` | 4 KB | IOMMU 2 | PCIe2 RISC-V IOMMU |
| `0x07_0000_6000` | 8 KB | AXI Bridge 3 | AXI 桥 3 CSR |
| `0x07_0000_7800` | 4 KB | IOMMU 3 | PCIe3 RISC-V IOMMU |
| `0x07_3000_0000` | 1 MB x 8 | L3 OCM Slice 0-7 | L3 Cache OCM/Scratchpad |
| `0x40_0000_0000` | 4 GB | DDR 通道 0 | 主存 (OpenSBI/内核加载地址) |
| `0x50_0000_0000` | 4 GB | DDR 通道 1 | 主存 |
| `0x60_0000_0000` | 4 GB | DDR 通道 2 | 主存 |
| `0x70_0000_0000` | 4 GB | DDR 通道 3 | 主存 |
| `0x80_0000_0000` | 4 GB | DDR 通道 4 | 主存 |
| `0x90_0000_0000` | 4 GB | DDR 通道 5 | 主存 |
| `0xA0_0000_0000` | 4 GB | DDR 通道 6 | 主存 |
| `0xB0_0000_0000` | 4 GB | DDR 通道 7 | 主存 |
| `0xC0_0000_0000` | 256 MB | PCIe0 ECAM | PCIe 配置空间 |
| `0xC0_0000_0000` | 64 GB | PCIe0 MMIO | PCIe MMIO 窗口 |
| `0xD0_0000_0000` | 256 MB | PCIe1 ECAM | PCIe 配置空间 |
| `0xD0_0000_0000` | 64 GB | PCIe1 MMIO | PCIe MMIO 窗口 |
| `0xE0_0000_0000` | 256 MB | PCIe2 ECAM | PCIe 配置空间 |
| `0xE0_0000_0000` | 64 GB | PCIe2 MMIO | PCIe MMIO 窗口 |
| `0xF0_0000_0000` | 256 MB | PCIe3 ECAM | PCIe 配置空间 |
| `0xF0_0000_0000` | 64 GB | PCIe3 MMIO | PCIe MMIO 窗口 |

### 3.2 DDR 配置

| 参数 | 值 |
|------|-----|
| 通道数 | 8 |
| 通道宽度 | 64-bit |
| 速率 | 4800 Mbps |
| 每通道最大容量 | 64 GB |
| 交织粒度 | 64 字节 |
| 默认每通道大小 | 4 GB |
| 总容量 | 32 GB |

---

## 4. 中断架构

### 4.1 三级 APLIC 层次结构

Beta SoC 采用 RISC-V AIA (Advanced Interrupt Architecture) 标准, 实现了三级 APLIC (Advanced Platform-Level Interrupt Controller) 层次:

```
+------------------------------------------------------+
|              APLIC 三级层次结构                        |
+------------------------------------------------------+
|                                                        |
|  MLROOT (M-Level Root)                                 |
|  @ 0x00_D004_0000, 64KB                               |
|  模式: 直接传递 (Direct Delivery)                       |
|  目标: MCPU hartid=32, M-mode 外部中断                  |
|  功能: 可接收所有 1023 个中断源                          |
|  默认: 所有源委托给 MLAPP                               |
|       |                                                |
|       v (delegation)                                   |
|  MLAPP (M-Level Application)                           |
|  @ 0x00_D005_0000, 64KB                               |
|  模式: MSI (写入 IMSIC M-level)                        |
|  目标: ACPU M-mode, 通过 IMSIC M                       |
|       |                                                |
|       v (delegation)                                   |
|  SLAPP (S-Level Application)                           |
|  @ 0x00_D006_0000, 64KB                               |
|  模式: MSI (写入 IMSIC S-level)                        |
|  目标: ACPU S-mode, 通过 IMSIC S                       |
|  Linux 内核通过此域接收外部中断                          |
+------------------------------------------------------+
```

中断传递路径:
1. 外部设备触发 -> MLROOT GPIO 输入
2. MLROOT 根据 sourcecfg 委托到 MLAPP
3. MLAPP 根据 sourcecfg 委托到 SLAPP
4. SLAPP 生成 MSI 写入 -> IMSIC S-level
5. IMSIC 触发对应 hart 的 S-mode 外部中断 (SIP.SEIP)

### 4.2 IMSIC 配置

| 参数 | 值 |
|------|-----|
| M-level 基地址 | `0x01_1020_0000` |
| S-level 基地址 | `0x01_1040_0000` |
| 中断 ID 数量 | 255 |
| Guest VS 文件数 | 4 (GEILEN=4) |
| Guest Index Bits | 3 |
| 每核 M-level 页面 | 1 (无 VS) |
| 每核 S-level 页面 | 5 (1 supervisor + 4 VS guest) |

每核 IMSIC 布局:
- M-level: `m_base + hartid * IMSIC_HART_SIZE(0)`, 每核 4KB
- S-level: `s_base + hartid * IMSIC_HART_SIZE(guest_bits)`, 每核 32KB (含 guest 文件)

### 4.3 MCPU PLIC

MCPU 拥有独立的紧凑型 PLIC (非 SiFive 标准布局), 位于 `0xE004_0000`, 大小 4KB, 用于 MCPU 子系统内部中断 (调试模块、错误状态等, 共 64 个源).

**寄存器布局:**

| 偏移 | 名称 | 宽度 | 访问 | 描述 |
|------|------|------|------|------|
| `0x00` | EL_LO | 32-bit | R/W | 边沿/电平选择 (源 0-31), 0=电平, 1=边沿 |
| `0x04` | EL_HI | 32-bit | R/W | 边沿/电平选择 (源 32-63) |
| `0x08` | SM_LO | 32-bit | R/W | 源屏蔽 (源 0-31), 1=屏蔽 |
| `0x0C` | SM_HI | 32-bit | R/W | 源屏蔽 (源 32-63) |
| `0x10` | DBG_EN_LO | 32-bit | R/W | 调试使能 (源 0-31) |
| `0x14` | DBG_EN_HI | 32-bit | R/W | 调试使能 (源 32-63) |
| `0x18`-`0x34` | PRIORITY[0-7] | 32-bit x 8 | R/W | 优先级 (每源 8-bit, 4 源/寄存器) |
| `0x38` | IE_LO | 32-bit | R/W | 中断使能 (源 0-31) |
| `0x3C` | IE_HI | 32-bit | R/W | 中断使能 (源 32-63) |
| `0x40` | THRESHOLD | 32-bit | R/W | 优先级阈值 |
| `0x44` | ID | 32-bit | R/W | 读=最高优先级等待中断 ID (0=无), 写=完成 |

**中断传递逻辑:**
1. Pending (IP): 设备触发时置位, 电平触发在源撤销时清除, 边沿触发保持到软件 claim
2. Active 条件: `active = IP & IE & ~SM` (pending 且 enabled 且未屏蔽)
3. 优先级筛选: 所有 active 源中, 选择优先级 > threshold 且最高的源
4. Claim: 读 ID 寄存器获取最高优先级 ID, 写 ID 寄存器清除对应 pending 位
5. 输出: 驱动 MCPU 的 M-mode 外部中断 (MIP.MEIP)

### 4.4 中断源映射表

以下是生产配置中定义的全部 99 个命名中断源:

| IRQ 编号 | 名称 | 描述 |
|---------|------|------|
| 1 | `mcpu_wdt` | MCPU 看门狗定时器 |
| 2 | `gp_timer` | 通用定时器 |
| 3 | `xip_ssi` | XIP SPI 接口 |
| 4 | `gp_ssi` | 通用 SPI 接口 |
| 5 | `uart0` | UART 串口 (已接线, 活跃) |
| 6-11 | `i2c0`-`i2c5` | I2C 总线 0-5 |
| 12 | `mailbox_mcpu` | MCPU Mailbox |
| 13 | `mailbox_acpu` | ACPU Mailbox |
| 14-15 | `usb0`-`usb1` | USB 控制器 0-1 |
| 16 | `tsensor` | 温度传感器 |
| 17-18 | `vsensor0`-`vsensor1` | 电压传感器 0-1 |
| 19 | `psensor` | 功率传感器 |
| 20-27 | `ddr0`-`ddr7` | DDR 控制器 0-7 |
| 28-43 | `pcie0_0`-`pcie0_15` | PCIe 子系统 0 中断 (16 个) |
| 44-59 | `pcie1_0`-`pcie1_15` | PCIe 子系统 1 中断 (16 个) |
| 60-75 | `pcie2_0`-`pcie2_15` | PCIe 子系统 2 中断 (16 个) |
| 76-91 | `pcie3_0`-`pcie3_15` | PCIe 子系统 3 中断 (16 个) |
| 92-99 | `acpu_irq0`-`acpu_irq7` | ACPU 通用中断 0-7 |
| 100-103 | (IOMMU 0) | PCIe0 IOMMU: CQ, FQ, PM, PQ (已接线) |
| 104-107 | (IOMMU 1) | PCIe1 IOMMU: CQ, FQ, PM, PQ (已接线) |
| 108-111 | (IOMMU 2) | PCIe2 IOMMU: CQ, FQ, PM, PQ (已接线) |
| 112-115 | (IOMMU 3) | PCIe3 IOMMU: CQ, FQ, PM, PQ (已接线) |

**GPIO 接线状态:**
- 活跃接线: UART (IRQ 5) -> APLIC root GPIO, IOMMU (IRQ 100-115) -> APLIC root GPIO
- 未接线 (设计如此): PCIe INTx (IRQ 28-91), 通用 MMIO 存根 (IRQ 1-27, 92-99)
- 独立路径 (不经过 APLIC): ACLINT timer/software 中断, MCPU PLIC

---

## 5. PCIe 子系统

### 5.1 概述

Beta SoC 包含 4 个独立的 PCIe 子系统, 每个子系统基于 Synopsys DesignWare (DWC) PCIe IP:

| 子系统 | DBI 基地址 | ECAM 基地址 | MMIO 基地址 | IOMMU 基地址 | IOMMU 基础 IRQ |
|--------|-----------|------------|------------|-------------|---------------|
| PCIe0 | `0x01_4000_0000` | `0xC0_0000_0000` | `0xC0_0000_0000` | `0x07_0000_1800` | 100 |
| PCIe1 | `0x01_4800_0000` | `0xD0_0000_0000` | `0xD0_0000_0000` | `0x07_0000_3800` | 104 |
| PCIe2 | `0x01_5000_0000` | `0xE0_0000_0000` | `0xE0_0000_0000` | `0x07_0000_5800` | 108 |
| PCIe3 | `0x01_5800_0000` | `0xF0_0000_0000` | `0xF0_0000_0000` | `0x07_0000_7800` | 112 |

### 5.2 每控制器内存映射

以 PCIe0 为例 (x16 模式):

| 地址 | 大小 | 区域 | 描述 |
|------|------|------|------|
| `0x01_4000_0000` | 3 MB | DBI | DWC 控制寄存器 (含 iATU) |
| `0x01_4030_0000` | 512 KB | ATU | iATU 寄存器 (在 DBI 范围内) |
| `0x01_4500_0000` | 1 MB | MGMT | 子系统管理层配置 |
| `0x01_4600_0000` | 8 MB | APPL | rv16 控制器 APPL 寄存器 |
| `0xC0_0000_0000` | 256 MB | ECAM | PCIe 配置空间 (bus/dev/fn/reg) |
| `0xC0_1000_0000` | 512 MB | Prefetchable MMIO | PCIe 64-bit 可预取 MMIO |
| `0xC0_3000_0000` | 256 MB | Non-pref MMIO | PCIe 32-bit 非预取 MMIO |

### 5.3 分叉支持

每个子系统支持两种模式:
- **x16 模式** (`bifurcation=1`): 单个 DWC 控制器, 独占全部资源
- **4x4 模式** (`bifurcation=4`): 4 个独立 DWC 控制器, 资源四等分

4x4 分叉时的资源分配:

| 资源 | 每控制器大小 | 偏移公式 |
|------|------------|---------|
| DBI | 256 KB | `dbi_base + j * 0x40000` |
| APPL | 2 MB | `dbi_base + 0x6000000 + j * 0x200000` |
| ECAM | `ecam_size / 4` | `ecam_base + j * (ecam_size / 4)` |
| Prefetchable MMIO | 128 MB | `mmio_base + 0x10000000 + j * 0x8000000` |
| Non-pref MMIO | 64 MB | `mmio_base + 0x30000000 + j * 0x4000000` |
| Bus Range | 64 buses | `j * 0x40` to `j * 0x40 + 0x3F` |

### 5.4 IOMMU

每个 PCIe 子系统配有一个 RISC-V IOMMU, 位于 AXI 桥 CSR 空间内:

| 子系统 | IOMMU 地址 | AXI 桥基地址 | 偏移 | 4 个 IRQ |
|--------|----------|------------|------|---------|
| PCIe0 | `0x07_0000_1800` | `0x07_0000_0000` | +0x1800 | 100-103 |
| PCIe1 | `0x07_0000_3800` | `0x07_0000_2000` | +0x1800 | 104-107 |
| PCIe2 | `0x07_0000_5800` | `0x07_0000_4000` | +0x1800 | 108-111 |
| PCIe3 | `0x07_0000_7800` | `0x07_0000_6000` | +0x1800 | 112-115 |

每个 IOMMU 有 4 个中断: Command Queue (CQ), Fault Queue (FQ), Page-request (PM), Performance Monitoring (PQ).

### 5.5 DWC 模型修改 (相对上游)

详见第 10 节.

### 5.6 FDT 属性

每个 PCIe 控制器在 FDT 中的关键属性:

| 属性 | 值 | 描述 |
|------|-----|------|
| `compatible` | `"rivai,rv-gen4-pcie"` | 驱动匹配标识 |
| `device_type` | `"pci"` | PCI 设备类型 |
| `reg` | dbi + appl + config | 三段寄存器空间 |
| `reg-names` | `"dbi"`, `"appl"`, `"config"` | 寄存器空间名称 |
| `msi-parent` | IMSIC S-level phandle | MSI 控制器 (禁用 DWC 内部 MSI) |
| `msi-map` | `0x0, imsic_s, 0x0, 0x10000` | 所有 PCI RID 映射到 IMSIC S |
| `dma-coherent` | (空属性) | QEMU 内存始终一致 |
| `ranges` | prefetchable + non-pref | PCI 地址到 CPU 地址翻译 |

**非预取 MMIO 地址翻译:**
由于 PCI 桥的 Memory Base/Limit 仅 32 位, 非预取区域的 PCI 地址使用 CPU 地址的低 32 位:
```
PCI 地址 = CPU 地址的低 32 位 (< 4GB)
CPU 地址 = 实际 > 4GB 位置
DWC iATU 负责地址翻译
```

### 5.7 iATU 出站 Viewport 机制

DWC PCIe 使用 iATU (internal Address Translation Unit) 进行 CPU 到 PCI 地址翻译:

- **MEM 类型**: CPU 内存访问 -> PCI Memory 事务
  - `viewport->mem` 为系统内存的别名, 映射到 PCI 地址空间
  - `alias_offset = viewport->target` (PCI 目标地址)
- **CFG 类型**: CPU 内存访问 -> PCI Configuration 事务
  - `viewport->cfg` 为 I/O 区域, 通过 `designware_pcie_root_data_access()` 处理
  - `target` 编码 bus/devfn: `bus = target[31:24]`, `devfn = target[23:16]`

Viewport 选择通过 `ATU_VIEWPORT` 寄存器 (偏移 0x900):
- 低 4 位: viewport 索引 (0-15)
- bit 31: 方向 (0=出站, 1=入站)

每个 viewport 有 6 个寄存器: CR1 (类型), CR2 (使能), LOWER_BASE, UPPER_BASE, LIMIT, LOWER_TARGET/UPPER_TARGET.

---

## 6. MCPU 子系统

### 6.1 MCPU 内存映射 (32-bit 视角)

MCPU 是 RV32 处理器, 地址空间为 32 位. 通过地址映射和 AHB Master Window 访问 SoC 的 40 位物理地址空间:

| 地址 | 大小 | 设备 | 描述 |
|------|------|------|------|
| `0xA000_0000` | 256 MB | AHB Master Window | 40-bit 地址翻译窗口 |
| `0xD000_0000` | 64 KB | MCPU ROM | BootROM (复位向量) |
| `0xD008_7000` | 4 KB | UART0 | 共享 UART (与 ACPU 相同物理地址) |
| `0xD800_0000` | 128 MB | BT_SPI_XIP | Boot SPI Flash |
| `0xE000_0000` | 64 KB | Subsystem IO | SOC_TOP_CSR 别名 (映射到 0x01_0000_0000) |
| `0xE004_0000` | 4 KB | MCPU PLIC | 子系统内部中断控制器 |
| `0xE004_1000` | 16 KB | Platform Timer | MSWI + MTIMER (ACLINT 布局) |
| `0xE004_D000` | 4 KB | ext_addr 寄存器 | AHB Master Window 高地址控制 |
| `0xF000_0000` | 512 KB | MCPU SRAM | 主工作内存 |

### 6.2 Subsystem IO (SOC_TOP_CSR 别名)

MCPU 地址空间中 `0xE000_0000` 处有一个 MMIO 别名区域, 映射到全局地址 `0x01_0000_0000` 的 SOC_TOP_CSR. 这允许 MCPU 直接访问系统顶层控制寄存器 (如 SW_RST_CTRL1, ACPU_BOOT_ADDR_L/H) 而无需通过 AHB Master Window.

实现方式: `memory_region_init_alias()` 将 SOC_TOP_CSR 的 MMIO 区域创建为别名, 挂载在 `0xE000_0000`.

### 6.3 MCPU PLIC 详细说明

地址: `0xE004_0000`, 大小: 4 KB
中断源: 64 个 (source 0 保留/未使用, 1-63 可用)
输出: 驱动 MCPU M-mode 外部中断 (MIP.MEIP)

**寄存器详细描述:**

| 偏移 | 名称 | 宽度 | 访问 | 描述 |
|------|------|------|------|------|
| `0x00` | EL_LO | 32b | R/W | Edge/Level 选择 bit 0-31. 0=电平触发, 1=边沿触发 |
| `0x04` | EL_HI | 32b | R/W | Edge/Level 选择 bit 32-63 |
| `0x08` | SM_LO | 32b | R/W | Source Mask bit 0-31. 1=屏蔽该源(不参与仲裁) |
| `0x0C` | SM_HI | 32b | R/W | Source Mask bit 32-63. 写入触发 PLIC 更新 |
| `0x10` | DBG_EN_LO | 32b | R/W | Debug Enable bit 0-31 |
| `0x14` | DBG_EN_HI | 32b | R/W | Debug Enable bit 32-63 |
| `0x18` | PRIO[0] | 32b | R/W | 优先级: source 0-3, 每源 8-bit (bit[7:0]=src0, bit[15:8]=src1, ...) |
| `0x1C` | PRIO[1] | 32b | R/W | 优先级: source 4-7 |
| `0x20` | PRIO[2] | 32b | R/W | 优先级: source 8-11 |
| `0x24` | PRIO[3] | 32b | R/W | 优先级: source 12-15 |
| `0x28` | PRIO[4] | 32b | R/W | 优先级: source 16-19 |
| `0x2C` | PRIO[5] | 32b | R/W | 优先级: source 20-23 |
| `0x30` | PRIO[6] | 32b | R/W | 优先级: source 24-27 |
| `0x34` | PRIO[7] | 32b | R/W | 优先级: source 28-31 |
| `0x38` | IE_LO | 32b | R/W | Interrupt Enable bit 0-31. 1=使能. 写入触发更新 |
| `0x3C` | IE_HI | 32b | R/W | Interrupt Enable bit 32-63. 写入触发更新 |
| `0x40` | THRESHOLD | 32b | R/W | 优先级阈值. 只有优先级 > threshold 的源可中断 CPU. 写入触发更新 |
| `0x44` | ID | 32b | R/W | Claim/Complete. 读=返回最高优先级等待中断 ID (0=无); 写=完成处理, 清除对应 pending bit |

### 6.4 Platform Timer

地址: `0xE004_1000`

采用标准 ACLINT 布局, 为 MCPU (hartid=32) 提供软件中断和定时器:

| 偏移 (相对 0xE0041000) | 功能 | 描述 |
|-----------------------|------|------|
| `0x0000` - `0x3FFF` | MSWI | Machine Software Interrupt (软件中断) |
| `0x4000` + | MTIMER | mtimecmp + mtime (定时器) |

- MSWI: 写入触发 MCPU 的 MIP.MSIP
- MTIMER: 64-bit mtime/mtimecmp, RV32 上通过高低 32 位对访问
- timebase_freq: 1,125,000,000 Hz (1.125 GHz)

### 6.5 AHB Master Window

MCPU 是 RV32 (32-bit 地址), 但 SoC 有 40-bit 物理地址空间. AHB Master Window 提供地址翻译:

- 窗口地址: `0xA000_0000` - `0xAFFF_FFFF` (256 MB)
- ext_addr 寄存器: `0xE004_D00C` (8-bit, R/W)
- 地址翻译公式: `PA[39:0] = { ext_addr[7:0], offset[27:0] }`

其中 `offset` 是 MCPU 在 `0xA000_0000` 窗口内的偏移量 (28 位).

**使用示例:**
要访问物理地址 `0x40_1234_5678`:
1. 设置 `ext_addr = 0x40` (写 `0xE004_D00C`)
2. 读/写 `0xA123_4567` (窗口内偏移 `0x1234_5678` -> 被截断为 `0x0123_4567`)

注意: offset 只有 28 位, 所以窗口内最大偏移为 256MB. 要访问同一个 ext_addr 下超过 256MB 的范围, 需要更新 ext_addr.

### 6.6 MCPU FDT

MCPU 有独立的设备树, 放置在 MCPU SRAM 末尾 (sram_base + sram_size - 64KB = `0xF007_0000`).

包含以下节点:
- `/`: model = "Beta MCPU Management Processor", #address-cells=1, #size-cells=1
- `/cpus/cpu@32`: RV32IMAC, hartid=32
- `/memory`: MCPU SRAM @ `0xF000_0000`, 512KB
- `/soc/interrupt-controller`: MLROOT APLIC (直接传递到 MCPU)
- `/soc/serial`: UART (共享)
- `/soc/mswi`: Platform Timer MSWI @ `0xE004_1000`
- `/soc/mtimer`: Platform Timer MTIMER @ `0xE004_5000`

---

## 7. 启动流程

### 7.1 Production 模式 (mcpu_bootrom)

这是默认的生产启动模式, 模拟真实硬件的启动流程:

```
MCPU 复位 (hartid=32)
    |
    v
BootROM @ 0x00_D000_0000  [MCPU ROM, RV32 代码]
    |
    +-- 1. 设置栈指针: sp = 0x10000
    |
    +-- 2. 初始化 UART @ 0xD008_7000
    |       - 设置 DLAB=1, LCR=0x83
    |       - 设置分频器 DLL=2, DLM=0 (115200 baud @ 3.6864MHz)
    |       - 设置 LCR=0x03 (8N1, DLAB=0)
    |       - 设置 FCR=0xC7 (使能并复位 FIFO)
    |       - 禁用所有中断 (IER=0)
    |
    +-- 3. 打印启动信息 ("=== Beta SoC Production MCPU BootROM ===")
    |
    +-- 4. 设置 32 个 ACPU 核心的启动地址:
    |       对每个 core N (0-31):
    |         ACPU_BOOT_ADDR_L_N = 0x00000000  [DDR0 低 32 位]
    |         ACPU_BOOT_ADDR_H_N = 0x00000040  [DDR0 高 32 位 = 0x40]
    |       => boot_addr = 0x40_0000_0000 (DDR0 基地址, OpenSBI 加载位置)
    |
    +-- 5. 释放所有 ACPU 核心:
    |       写 SW_RST_CTRL1 @ (0xE000_0000 + 0x810) = 0xFFFFFFFE
    |       bits[31:1] = 全 1 (释放 core 0-30)
    |       bit[0] = 0 (BACPU, 保留不释放)
    |
    +-- 6. 进入空闲循环 (wfi)
    |
    v
SOC_TOP_CSR 检测 SW_RST_CTRL1 的 0->1 跳变:
    对每个 bit N (N=1..31), 如果检测到 0->1:
      core_idx = N - 1
      读取 ACPU_BOOT_ADDR_L_{core_idx} 和 ACPU_BOOT_ADDR_H_{core_idx}
      boot_addr = (ADDR_H << 32) | ADDR_L
      boot_arg = BOOT_ARG 寄存器值 或 QEMU FDT 地址
      |
      +-- cpu_reset(core)
      +-- cpu->halted = 0
      +-- cpu_set_pc(core, boot_addr)     [设置 PC = 0x4000000000]
      +-- a0 = acpu_hartid_base + core_idx  [设置 hartid]
      +-- a1 = boot_arg                      [设置 FDT 地址]
      +-- cpu_resume(core)
    |
    v
ACPU core 0 从 0x40_0000_0000 开始执行
    => OpenSBI fw_jump
    => Linux 内核
```

**SW_RST_CTRL1 寄存器 (偏移 0x810):**

| 位域 | 描述 |
|------|------|
| bit 0 | sw_bacpu_core_rst_n (BACPU 复位控制, 当前忽略) |
| bit 1 | sw_acpu_core_rst_n[0] (core 0 复位释放) |
| bit 2 | sw_acpu_core_rst_n[1] (core 1 复位释放) |
| ... | ... |
| bit N+1 | sw_acpu_core_rst_n[N] (core N 复位释放) |
| bit 31 | sw_acpu_core_rst_n[30] (core 30 复位释放) |

检测机制: `newly_released = value & ~prev_value` (检测 0->1 跳变)
写入 0xFFFFFFFE 时, bits[31:1] 全部从 0 变为 1, 触发所有 31 个核心的释放.

**Per-core 启动地址寄存器命名:**
- `ACPU_BOOT_ADDR_L_N`: 偏移 `0x000 + N*4`, 低 32 位
- `ACPU_BOOT_ADDR_H_N`: 偏移 `0x100 + N*4`, 高 32 位
- 复位值: `ADDR_L = 0xD800_0000` (BT_SPI_XIP), `ADDR_H = 0x0000_0000`
- BootROM 将其改写为 DDR0 基地址

### 7.2 ACPU 直接模式 (acpu_direct)

用于开发和测试, 跳过 MCPU BootROM, ACPU 直接从 `acpu_entry` 开始执行:

- ACPU 核心不设置 `start-powered-off`, 直接运行
- MCPU 核心被 halt
- 复位处理程序设置: `a0 = hartid`, `a1 = FDT 地址`
- 使用场景: 不需要 MCPU BootROM 的纯 Linux 测试

启动参数:
- JSON 配置中设置 `boot.mode = "acpu_direct"`
- `boot.acpu_entry` 指定 ACPU 入口地址

### 7.3 BootROM 关键汇编代码解读

BootROM 源文件: `mcpu_bootrom_production.S`

**UART 初始化 (uart_init):**
- 使用 UART 基地址 `0xD008_7000`, regshift=2 (寄存器间距 4 字节)
- 配置 8N1, 115200 波特率 (分频器=2, 时钟 3.6864MHz)
- 使能 FIFO

**启动地址设置循环:**
```
对每个 core N = 0..31:
  ACPU_BOOT_ADDR_L[N] = SOC_CSR_BASE + N*4       = 0xE0000000 + N*4
  ACPU_BOOT_ADDR_H[N] = SOC_CSR_BASE + N*4 + 0x100 = 0xE0000100 + N*4
  写入: L = 0x00000000, H = 0x00000040
```

**核心释放:**
```
地址: SOC_CSR_BASE + 0x810 = 0xE0000810
写入: 0xFFFFFFFE (bit 31:1 = 1, bit 0 = 0)
```

---

## 8. JSON 配置格式

### 8.1 顶层结构

```json
{
  "version": "2.0",
  "acpu": { ... },
  "mcpu": { ... },
  "memory_regions": [ ... ],
  "interrupts": {
    "aplic": { ... },
    "imsic": { ... },
    "clint": { ... },
    "plic": { ... },
    "irq_map": [ ... ]
  },
  "devices": [ ... ],
  "ddr": { ... },
  "pcie": [ ... ],
  "boot": { ... }
}
```

### 8.2 各节描述

**acpu 节:**

| 字段 | 类型 | 默认值 | 描述 |
|------|------|--------|------|
| `num_clusters` | uint32 | 必填 | 集群数量 (生产=8) |
| `cores_per_cluster` | uint32 | 必填 | 每集群核心数 (生产=4) |
| `isa` | string | 必填 | ISA 字符串 (如 "rv64imafdcv") |
| `mmu_mode` | string | "sv39" | MMU 模式 ("sv39", "sv48" 等) |
| `pma_type` | string | "none" | PMA 类型 |
| `reset_vector` | hex string | 必填 | 复位向量地址 |

**mcpu 节:**

| 字段 | 类型 | 默认值 | 描述 |
|------|------|--------|------|
| `num_cores` | uint32 | 必填 | 核心数 (生产=1) |
| `isa` | string | 必填 | ISA 字符串 (如 "rv32imac") |
| `itcm_kb` | uint32 | 0 | ITCM 大小 (KB) |
| `dtcm_kb` | uint32 | 0 | DTCM 大小 (KB) |
| `sram_kb` | uint32 | 0 | SRAM 大小 (KB) |
| `reset_vector` | hex string | 必填 | 复位向量地址 |

**memory_regions 节 (数组):**

每个元素:

| 字段 | 类型 | 描述 |
|------|------|------|
| `name` | string | 区域名称 (用于内部查找) |
| `base` | hex string | 基地址 |
| `size` | hex string | 大小 |
| `type` | string | "ram", "rom", "mmio" |
| `cpu_visible` | string | "acpu", "mcpu", "both" |

**interrupts.aplic 节:**

| 字段 | 类型 | 描述 |
|------|------|------|
| `num_sources` | uint32 | 中断源数量 (生产=1023) |
| `domains` | array | APLIC 域配置数组 |

每个域:

| 字段 | 类型 | 描述 |
|------|------|------|
| `name` | string | 域名 |
| `level` | string | "root", "m", "s" |
| `base` | hex string | 基地址 |
| `size` | hex string | 大小 |
| `parent` | string/null | 父域名 |

**interrupts.imsic 节:**

| 字段 | 类型 | 描述 |
|------|------|------|
| `m_base` | hex string | M-level IMSIC 基地址 |
| `s_base` | hex string | S-level IMSIC 基地址 |
| `num_ids` | uint32 | 中断 ID 数量 (默认 255) |
| `num_vs_files` | uint32 | VS guest 文件数 (默认 0) |
| `guest_index_bits` | uint32 | Guest 索引位数 |

**interrupts.clint 节:**

| 字段 | 类型 | 描述 |
|------|------|------|
| `base` | hex string | CLINT 基地址 |
| `size` | hex string | CLINT 大小 |
| `timebase_freq` | uint32 | 时基频率 (Hz) |

**interrupts.irq_map (数组):**

每个元素:

| 字段 | 类型 | 描述 |
|------|------|------|
| `name` | string | 中断源名称 |
| `irq_num` | uint32 | 中断编号 |
| `target` | string | "aplic" 或 "plic" |
| `domain` | string | "m_root", "m_app", "s_app" |

**devices 节 (数组):**

每个元素:

| 字段 | 类型 | 描述 |
|------|------|------|
| `name` | string | 设备名称 |
| `type` | string | "uart", "soc_top_csr", "generic_mmio" 等 |
| `base` | hex string | 基地址 |
| `size` | hex string | 区域大小 |
| `cpu_target` | string | "acpu", "mcpu", "both" |
| `compatible` | string | DT compatible 字符串 |
| `irq_name` | string | irq_map 中的键名 |
| `clock_hz` | uint32 | 时钟频率 |
| `instance_id` | int | 实例 ID |
| `registers` | array | 寄存器定义数组 |

**寄存器定义格式 (registers 数组中的元素):**

| 字段 | 类型 | 描述 |
|------|------|------|
| `name` | string | 寄存器名称 (如 "SW_RST_CTRL1") |
| `offset` | hex string | 偏移地址 |
| `width` | uint32 | 宽度 (4=32-bit) |
| `access` | string | "rw", "ro", "wo", "w1c", "rc" |
| `reset_value` | hex string | 复位值 |
| `fields` | array | 位域定义 (可选) |

**回调机制:**

某些寄存器写入会触发副作用回调:
- `SW_RST_CTRL1`: 写入触发 `beta_soc_csr_sw_rst_ctrl1()`, 检测 0->1 跳变释放 ACPU 核心
- `CORE_RELEASE` (legacy): 写入触发 `beta_soc_csr_core_release()`, 位掩码释放核心
- `RESET_CTRL`: 写入触发 `beta_soc_csr_reset_ctrl()` (当前未实现)

回调基于寄存器名称自动检测: 如果 JSON 中定义了 "SW_RST_CTRL1" 则使用生产模式, 否则使用旧版 "CORE_RELEASE" 模式.

**ddr 节:**

| 字段 | 类型 | 描述 |
|------|------|------|
| `num_channels` | uint32 | 通道数 (生产=8) |
| `channel_width_bits` | uint32 | 通道宽度 (64) |
| `speed_mbps` | uint32 | DDR 速率 (4800) |
| `max_per_channel_gb` | uint32 | 每通道最大容量 (64) |
| `interleave_step_bytes` | uint32 | 交织步长 (64) |
| `channels` | array | 每通道配置 |

每通道:

| 字段 | 类型 | 描述 |
|------|------|------|
| `id` | uint32 | 通道 ID |
| `ctrl_base` | hex string | 控制器寄存器基地址 |
| `mem_base` | hex string | 内存基地址 |
| `size` | hex string | 大小 |

**pcie 节 (数组):**

每个元素:

| 字段 | 类型 | 描述 |
|------|------|------|
| `dbi_base` | hex string | DWC DBI 寄存器基地址 |
| `dbi_size` | hex string | DBI 大小 |
| `ecam_base` | hex string | ECAM 配置空间基地址 |
| `ecam_size` | hex string | ECAM 大小 |
| `mmio_base` | hex string | 32-bit MMIO 窗口基地址 |
| `mmio_size` | hex string | MMIO 窗口大小 |
| `num_lanes` | uint32 | 通道数 (1/2/4/8/16) |
| `bifurcation` | int | 1=x16, 4=4x4 分叉 |
| `irq_name` | string | MSI 中断名称 |
| `irq_intx_name` | string | INTx 中断名称 |
| `iommu_base` | hex string | IOMMU 寄存器基地址 |
| `iommu_base_irq` | int | IOMMU 基础 IRQ 编号 |

**boot 节:**

| 字段 | 类型 | 描述 |
|------|------|------|
| `mode` | string | "mcpu_bootrom", "mcpu_xip", "acpu_direct" |
| `bootrom_base` | hex string | BootROM 基地址 |
| `bootrom_size` | hex string | BootROM 大小 |
| `flash_base` | hex string | NOR Flash 基地址 |
| `flash_size` | hex string | NOR Flash 大小 |
| `mcpu_entry` | hex string | MCPU 入口地址 |
| `acpu_entry` | hex string | ACPU 入口地址 |

### 8.3 配置修改示例

**修改核心数量:**
```json
"acpu": {
  "num_clusters": 4,
  "cores_per_cluster": 2
}
```
结果: 8 个 ACPU 核心 (hartid 0-7), MCPU hartid=8.

**修改 DDR 大小:**
```json
"channels": [
  { "id": 0, "ctrl_base": "0x120000000", "mem_base": "0x4000000000", "size": "0x200000000" }
]
```
结果: 通道 0 为 8GB.

---

## 9. FDT 生成

### 9.1 ACPU FDT 节点结构

```
/ (model="Beta RISC-V SoC", compatible="riscv-beta,qemu")
  /chosen
    bootargs = (来自 -append)
    stdout-path = /soc/serial@...
    linux,initrd-start = ...
    linux,initrd-end = ...
  /aliases
    serial0 = /soc/serial@...
  /cpus (#address-cells=1, #size-cells=0)
    timebase-frequency = 1125000000
    /cpu@0 .. /cpu@31
      device_type = "cpu"
      reg = <hartid>
      compatible = "riscv"
      riscv,isa = "rv64imafdcv"
      riscv,isa-base = "rv64i"
      mmu-type = "riscv,sv39"
      riscv,isa-extensions = (65 个扩展的字符串数组)
      riscv,cbom-block-size = 64
      riscv,cboz-block-size = 64
      /interrupt-controller
        compatible = "riscv,cpu-intc"
        #interrupt-cells = 1
  /memory@4000000000
    device_type = "memory"
    reg = <0x40 0x00000000 0x08 0x00000000>  (32GB)
  /soc (compatible="simple-bus", ranges)
    /mswi@600000000
      compatible = "riscv,aclint-mswi"
    /mtimer@600004000
      compatible = "riscv,aclint-mtimer"
    /interrupt-controller@110200000  (IMSIC M)
      compatible = "riscv,imsics"
      msi-controller
    /interrupt-controller@110400000  (IMSIC S)
      compatible = "riscv,imsics"
      msi-controller
      riscv,guest-index-bits = 3
    /interrupt-controller@d0060000  (SLAPP)
      compatible = "riscv,aplic"
      msi-parent = <imsic_s_phandle>
    /interrupt-controller@d0050000  (MLAPP)
      compatible = "riscv,aplic"
      msi-parent = <imsic_m_phandle>
      riscv,children = <slapp_phandle>
      riscv,delegation = <slapp_phandle 1 1023>
    /interrupt-controller@d0040000  (MLROOT)
      compatible = "riscv,aplic"
      riscv,children = <mlapp_phandle>
      riscv,delegation = <mlapp_phandle 1 1023>
    /serial@d0087000
      compatible = "ns16550a"
      clock-frequency = 3686400
      reg-shift = 2
      reg-io-width = 1
    /iommu@700001800  (IOMMU 0)
      compatible = "riscv,iommu"
      #iommu-cells = 1
    /pcie-mgmt@145000000  (PCIe 管理层)
      compatible = "rivai,rv-mgmt-pcie"
      /pcie@140000000  (rv-gen4-pcie 控制器)
        compatible = "rivai,rv-gen4-pcie"
        device_type = "pci"
        msi-parent = <imsic_s_phandle>
        msi-map = <0x0 imsic_s 0x0 0x10000>
        dma-coherent
```

### 9.2 MCPU FDT 节点结构

```
/ (model="Beta MCPU Management Processor", #address-cells=1, #size-cells=1)
  /chosen
    stdout-path = /soc/serial@...
  /aliases
    serial0 = /soc/serial@...
  /cpus
    timebase-frequency = 1125000000
    /cpu@32
      riscv,isa = "rv32imac"
      riscv,isa-base = "rv32i"
  /memory@f0000000
    device_type = "memory"
    reg = <0xf0000000 0x80000>  (512KB SRAM)
  /soc
    /interrupt-controller@d0040000  (MLROOT, 直接传递)
    /serial@d0087000
    /mswi@e0041000
    /mtimer@e0045000
```

### 9.3 ISA 扩展 FDT 属性格式

ISA 扩展通过 `riscv,isa-extensions` 属性以字符串数组形式传递:

```
riscv,isa-extensions = "i", "m", "a", "f", "d", "c", "v", "h", "b",
    "zicsr", "zifencei", "zicntr", "zihpm", "smaia", "ssaia", "sstc",
    "zba", "zbb", "zbs", "zkt", "zfhmin", "zfa", "zicbom", "zicbop",
    "zicboz", "zihintpause", "zihintntl", "zawrs", "zvfhmin", "zvbb",
    "zvkt", "zcb", "zcmop", "zicond", "zimop", "svnapot", "svpbmt",
    "svinval", "svade", "sscofpmf", "ssnpm", "supm", "zic64b", "ziccif",
    "ziccrse", "ziccamoa", "zicclsm", "za64rs", "ss1p13", "svbare",
    "sv39", "ssccptr", "sstvecd", "sstvala", "sscounterenw", "ssu64xl",
    "ssstateen", "shcounterenw", "shvstvala", "shtvala", "shvstvecd",
    "shvsatpa", "shgatpa", "ztso";
```

### 9.4 验证 FDT

启动后提取 FDT 并反编译为可读文本:

```bash
# 从 QEMU 导出 FDT (使用 QEMU monitor)
(qemu) dumpdtb beta.dtb

# 反编译为 DTS 文本
dtc -I dtb -O dts -o beta.dts beta.dtb

# 查看特定节点
grep -A 10 "riscv,isa-extensions" beta.dts
```

---

## 10. DWC DesignWare PCIe 修改

以下是对 QEMU 上游 `hw/pci-host/designware.c` 的修改列表. 这些修改使 DWC 模型能够正确支持 Beta SoC 的 PCIe 子系统.

### 10.1 Type 1 配置转发 (pci_find_bus_nr)

- **文件**: `hw/pci-host/designware.c`, `designware_pcie_root_data_access()` 函数
- **修改**: 当 busnum != 0 时, 使用 `pci_find_bus_nr()` 查找目标总线, 而不是只在根总线上操作
- **原因**: 上游实现只能访问 bus 0 设备. Beta 的 rv-gen4 驱动通过 iATU CFG viewport 访问下游总线设备 (bus 1+), 需要 Type 1 配置周期转发
- **影响**: 允许通过 ATU viewport 对多级总线上的 PCIe 设备进行配置空间访问

### 10.2 ATU LIMIT 64-bit 修复

- **文件**: `hw/pci-host/designware.c`, `DESIGNWARE_PCIE_ATU_LIMIT` case in config_write
- **修改**: 写入 LIMIT 低 32 位时, 继承 BASE 的高 32 位: `viewport->limit = (viewport->base & 0xFFFFFFFF00000000ULL) | val`
- **原因**: DWC v4.60A 之前的内核驱动只写 LIMIT 的低 32 位 (UPPER_LIMIT 寄存器不存在). 当 BASE > 4GB 时, LIMIT 的高 32 位为 0, 导致 `limit - base` 下溢, 计算出的 viewport 大小错误
- **影响**: 修复了 Beta PCIe 地址 > 4GB 时 iATU viewport 大小计算错误的问题

### 10.3 ATU 地址重映射 (0x300000 -> 0x904)

- **文件**: `hw/pci-host/designware.c` / `hw/pci-host/designware.h`
- **修改**: ATU 寄存器通过 DBI config space 偏移访问. Beta 的 rv-gen4 驱动使用 DBI 偏移 0x300000 作为 ATU 基地址, 而 QEMU DWC 模型使用标准偏移 0x904. 驱动端或 QEMU 端进行了地址映射
- **原因**: Beta 硬件将 ATU 寄存器放在 DBI 空间的 0x300000 偏移处 (独立的 ATU 窗口), 而非内联在 DBI config 空间中
- **影响**: 确保 rv-gen4 驱动的 ATU 编程能正确映射到 QEMU 内部的 viewport 结构

### 10.4 Viewport 大小限制

- **文件**: `hw/pci-host/designware.c`, `designware_pcie_update_viewport()` 函数
- **修改**: 添加大小限制: 当 `limit < base` (寄存器尚未设置) 导致 size > 4GB 时, 限制 MEM viewport 为 1GB, CFG viewport 为 4KB:
  ```c
  if (size > 0x100000000ULL) {
      size = viewport->cr[0] == DESIGNWARE_PCIE_ATU_TYPE_MEM
           ? 0x40000000ULL : 0x1000ULL;
  }
  ```
- **原因**: 防止 viewport 寄存器部分编程时的 QEMU 内存区域溢出
- **影响**: 避免了 iATU 编程过程中的暂态错误状态导致的内存映射异常

### 10.5 扩展 MMIO 区域 (0x300100)

- **文件**: `hw/pci-host/designware.c` / `hw/pci-host/designware.h`
- **修改**: DWC host MMIO 区域大小扩展, 以覆盖 ATU 窗口在 0x300000 的偏移
- **原因**: 标准 DWC 模型的 DBI MMIO 区域不够大, 不包含 ATU 寄存器在 0x300000+ 处的映射
- **影响**: rv-gen4 驱动可以通过 MMIO 访问 ATU 寄存器

### 10.6 上游兼容性影响

这些修改是向后兼容的:
- Type 1 转发: busnum=0 时行为不变
- LIMIT 64-bit: 当 base < 4GB 时, 高 32 位继承值为 0, 行为不变
- Viewport 大小限制: 仅在异常情况下生效
- 不影响其他使用 DWC 模型的平台 (如 imx7)

---

## 11. rv-gen4 PCIe 驱动修改

### 11.1 native_ecam = true

Beta SoC 的 PCIe 配置空间访问有两种路径:
1. **iATU 路径**: 驱动编程 ATU CFG viewport, 通过 viewport 窗口访问目标设备的 config space
2. **Native ECAM 路径**: 直接通过 ECAM 区域 (bus/dev/fn 编码在地址中) 访问

设置 `native_ecam = true` 使驱动绕过 iATU, 直接使用 ECAM 方式访问配置空间.

**原因**: 在 QEMU 中, DWC 模型的 iATU CFG 访问路径需要精确的 viewport 编程序列. ECAM 方式更简单直接, 避免了 iATU 编程的复杂性.

### 11.2 iATU vs ECAM 配置访问路径

**iATU 路径:**
```
CPU -> 写 ATU_VIEWPORT (选择 viewport)
    -> 写 ATU_CR1 (type=CFG)
    -> 写 ATU_LOWER_TARGET (bus/devfn)
    -> 写 ATU_CR2 (enable)
    -> 读/写 ATU CFG 窗口地址
    -> DWC 模型将请求路由到 PCI 设备
```

**ECAM 路径:**
```
CPU -> 直接读/写 ECAM 基地址 + (bus<<20 | devfn<<12 | reg)
    -> QEMU beta_pcie_config_read/write 处理
    -> pci_find_bus_nr + pci_find_device
    -> pci_host_config_read/write_common
```

### 11.3 栈溢出根因和修复

rv-gen4 驱动在初始化时可能出现内核栈溢出. 根因是 PCIe 枚举过程中的深层递归调用链. 修复方案: 保持 `THREAD_SIZE_ORDER = 2` (16KB 栈), 并优化驱动中的递归深度. 不要将 THREAD_SIZE_ORDER 改为其他值, 而是修复根本原因.

---

## 12. 构建指南

### 12.1 系统要求

**x86_64 Linux（推荐）:**
- 宿主机: Ubuntu 22.04 / 24.04
- Docker (用于交叉编译内核和 rootfs)
- 磁盘空间: 至少 50 GB
- 内存: 至少 16 GB

**macOS Apple Silicon（M4 等）:**
- macOS 14 Sonoma 或更新
- Homebrew
- 磁盘空间: 至少 20 GB
- 内存: 16 GB（-j4 编译），32 GB（可用 -j8 或更高）

> **重要**: macOS 上源码路径不能包含空格，meson/ninja 不处理路径中的空格。
> 若工作目录名含空格（如 `"beta qemu"`），请先将源码拷贝到无空格路径，例如 `~/qemu-beta-src/`。

### 12.2 依赖包安装 (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential git python3 python3-pip python3-venv \
    ninja-build pkg-config \
    libglib2.0-dev libpixman-1-dev libfdt-dev \
    libsdl2-dev libgtk-3-dev libvte-2.91-dev \
    libssl-dev libcap-ng-dev libattr1-dev \
    flex bison \
    device-tree-compiler \
    qemu-user-static binfmt-support debootstrap \
    docker.io
```

### 12.2b 依赖包安装 (macOS / Homebrew)

```bash
brew install glib pixman ninja meson pkg-config dtc libslirp python3
```

> Apple Clang 15+ 已满足编译要求，无需额外安装 GCC。

### 12.3 QEMU 构建（Linux）

```bash
cd /path/to/qemu

# 配置 (编译 RV64 + RV32 系统模式，MCPU 需要 riscv32-softmmu)
./configure \
    --target-list=riscv64-softmmu,riscv32-softmmu \
    --enable-slirp \
    --enable-virtfs \
    --prefix=/opt/qemu-beta

# 编译
ninja -C build -j$(nproc)

# 安装 (可选)
ninja -C build install
```

验证:
```bash
./build/qemu-system-riscv64 --version
./build/qemu-system-riscv64 -machine help | grep beta
```

### 12.3b QEMU 构建（macOS M4）

```bash
# 确保源码在无空格路径
cp -r beta_soc_qemu_release/qemu-10.0.2-beta ~/qemu-beta-src
cd ~/qemu-beta-src

mkdir -p build && cd build
../configure \
    --target-list=riscv64-softmmu,riscv32-softmmu \
    --enable-slirp \
    --disable-docs \
    --disable-gtk \
    --disable-sdl \
    --disable-vnc \
    --extra-cflags="-O2" \
    2>&1 | tee configure.log

# 16 GB 内存推荐不超过 -j4
make -j4
```

编译产物: `build/qemu-system-riscv64-unsigned`（macOS 下默认不签名，直接可用）

验证:
```bash
./build/qemu-system-riscv64-unsigned --version
./build/qemu-system-riscv64-unsigned -machine help | grep beta
```

### 12.4 Linux 内核构建 (Docker 交叉编译)

```bash
# 使用 Docker 环境进行交叉编译
docker run --rm -v /path/to/linux:/work -w /work \
    riscv64-toolchain:latest \
    bash -c "
        make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- defconfig
        # 应用 Beta SoC 配置补丁
        scripts/config --enable CONFIG_SOC_BETA
        scripts/config --set-val CONFIG_THREAD_SIZE_ORDER 2
        make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j\$(nproc) Image
    "
```

目标: Linux 6.19.5, GCC 15.2

### 12.5 OpenSBI 构建 (Docker 交叉编译)

```bash
docker run --rm -v /path/to/opensbi:/work -w /work \
    riscv64-toolchain:latest \
    bash -c "
        make CROSS_COMPILE=riscv64-linux-gnu- \
             PLATFORM=generic \
             FW_JUMP_ADDR=0x4000200000 \
             FW_JUMP_FDT_ADDR=0x4000000000 \
             -j\$(nproc)
    "
```

输出: `build/platform/generic/firmware/fw_jump.bin`

注意: 使用 OpenSBI v1.6+, v1.4 存在定时器中断兼容性问题.

### 12.6 BootROM 构建

```bash
riscv64-elf-gcc -march=rv32imac -mabi=ilp32 \
    -nostdlib -nostartfiles \
    -Ttext=0xD0000000 \
    -o mcpu_bootrom.elf \
    mcpu_bootrom_production.S

riscv64-elf-objcopy -O binary mcpu_bootrom.elf mcpu_bootrom.bin
```

### 12.7 Rootfs 构建 (Ubuntu 25.10)

```bash
# 创建空磁盘镜像
dd if=/dev/zero of=rootfs.img bs=1M count=4096
mkfs.ext4 rootfs.img

# 挂载并使用 debootstrap
sudo mount rootfs.img /mnt
sudo debootstrap --arch=riscv64 plucky /mnt http://ports.ubuntu.com/

# 配置基本系统
sudo chroot /mnt /bin/bash -c "
    echo 'beta-qemu' > /etc/hostname
    echo 'root:root' | chpasswd
    systemctl enable serial-getty@ttyS0.service
"

sudo umount /mnt
```

---

## 13. 运行指南

### 13.1 Production 模式命令行

**所需文件:**
- `firmware/bootrom/mcpu_bootrom_production.bin`
- `firmware/opensbi/fw_jump_0x4000000000.bin`
- `kernel/Image-6.19.5-beta`
- `rootfs/initramfs.cpio.gz`

```bash
RELEASE=~/Desktop/beta_release/beta_soc_qemu_release
QEMU=~/qemu-beta-src/build/qemu-system-riscv64-unsigned   # macOS
# QEMU=./qemu-10.0.2-beta/build/qemu-system-riscv64       # Linux

$QEMU \
    -machine beta,config-file=$RELEASE/config/beta_production_config.json \
    -bios $RELEASE/firmware/bootrom/mcpu_bootrom_production.bin \
    -device loader,file=$RELEASE/firmware/opensbi/fw_jump_0x4000000000.bin,addr=0x4000000000 \
    -kernel $RELEASE/kernel/Image-6.19.5-beta \
    -initrd $RELEASE/rootfs/initramfs.cpio.gz \
    -append "root=/dev/ram rdinit=/init earlycon=ns16550a,mmio32,0xD0087000 console=ttyS0,115200n8 mem=2G" \
    -nographic
```

参数说明:

| 参数 | 描述 |
|------|------|
| `-machine beta` | 选择 Beta SoC machine type |
| `config-file=...` | 指定 JSON 硬件配置文件 |
| `-bios mcpu_bootrom_production.bin` | MCPU BootROM 固件, 加载到 `bootrom_base`（0xD0000000）|
| `-device loader,file=fw_jump_0x4000000000.bin,addr=0x4000000000` | OpenSBI 加载到 DDR0 基地址 |
| `-kernel Image-6.19.5-beta` | Linux 内核镜像 |
| `-initrd initramfs.cpio.gz` | initramfs 根文件系统 |
| `earlycon=ns16550a,mmio32,0xD0087000` | 早期串口（MCPU UART，ns16550a MMIO 模式）|
| `mem=2G` | 只向内核申报 DDR0 前 2 GB，规避 Sv39 稀疏 DDR 映射崩溃（详见 §14.6）|
| `-nographic` | 无图形输出, 串口重定向到终端 |

**调试加速（减少核心数）:**
```bash
# maxcpus=4: 只激活 4 核，macOS M4 上启动时间从 ~7 分钟缩短到 ~1 分钟
-append "... maxcpus=4"
```

### 13.2 ACPU 直接模式命令行

```bash
./build/qemu-system-riscv64 \
    -M beta,config-file=beta_acpu_direct_config.json \
    -device loader,file=fw_jump.bin,addr=0x4000000000 \
    -device loader,file=Image,addr=0x4000200000 \
    -nographic
```

需要修改 JSON 配置: `"boot": { "mode": "acpu_direct", ... }`

### 13.3 使用 Ubuntu Rootfs 启动

```bash
ROOTFS=/path/to/rootfs.img ./scripts/run_production.sh
```

或手动:
```bash
./build/qemu-system-riscv64 \
    -M beta,config-file=beta_production_config.json \
    -bios mcpu_bootrom.bin \
    -device loader,file=fw_jump.bin,addr=0x4000000000 \
    -kernel Image \
    -initrd initramfs.cpio.gz \
    -append "root=/dev/vda rw console=ttyS0 earlycon" \
    -drive file=rootfs.img,format=raw,id=hd0 \
    -device virtio-blk-pci,drive=hd0 \
    -nographic
```

### 13.4 添加 Virtio 设备

**virtio-net (网络):**
```bash
-device virtio-net-pci,netdev=net0 \
-netdev user,id=net0,hostfwd=tcp::2222-:22
```

**virtio-blk (块设备):**
```bash
-drive file=disk.img,format=qcow2,id=drive0 \
-device virtio-blk-pci,drive=drive0
```

**virtio-9p (文件共享):**
```bash
-fsdev local,id=fsdev0,path=/shared,security_model=mapped \
-device virtio-9p-pci,fsdev=fsdev0,mount_tag=shared
```

### 13.5 注意事项

- `-smp` 和 `-m` 参数在 Beta machine 中被 JSON 配置覆盖. 核心数由 `acpu.num_clusters * acpu.cores_per_cluster` 决定, 内存由 DDR 通道配置决定
- 最大 CPU 数 = 33 (32 ACPU + 1 MCPU), 在 `beta_machine_class_init()` 中定义
- 不支持 VM migration (vmsd 被清除)

---

## 14. 已知问题和限制

### 14.1 virtio-net MSI-X 与 IOMMU

**现象**: 当 FDT 中存在 `iommu-map` 属性时, 内核为 virtio-net 分配 IOVA 进行 DMA. 但 QEMU 中 IOMMU 未接入 DWC PCIe DMA 路径, 设备使用 IOVA 作为物理地址进行 DMA, 导致数据损坏.

**规避**: FDT 中故意省略 `iommu-map` 属性. 内核使用 1:1 DMA 映射, 一切正常.

**待解决**: 将 IOMMU 接入 DWC DMA 数据路径后, 可重新启用 `iommu-map`.

### 14.2 OpenSBI v1.4 定时器中断不兼容

**现象**: 使用 OpenSBI v1.4 时, 定时器中断处理异常.

**规避**: 使用 OpenSBI v1.6 或更高版本.

### 14.3 PCIe PHY Link-up 仿真

**现象**: rv-gen4 驱动输出 "Phy link never came up" 警告.

**原因**: QEMU 不仿真 PCIe PHY 链路训练过程. link-up 状态通过两种方式硬编码:
1. DBI `PHY_DEBUG_R1` 寄存器 (偏移 0x72C): 始终返回 `XMLH_LINK_UP` (bit 4)
2. APPL `APPL_RV_AHB_OVRD_REG_01` (偏移 0x4): 始终设置 bit 12 和 bit 1

**影响**: 警告可忽略, 不影响功能.

### 14.4 swiotlb 分配失败

**现象**: 内核启动时输出 swiotlb 相关警告.

**原因**: Beta SoC 的所有 DDR 通道都位于 4GB 以上 (最低地址 0x40_0000_0000). 32-bit DMA 设备需要 bounce buffer, 但 swiotlb 可能未能分配足够的低地址内存.

**影响**: 对 QEMU 仿真不构成实际问题, 因为 QEMU 中 PCI DMA 不受物理地址限制.

### 14.5 IOMMU IRQ 向量

**现象**: IOMMU 中断向量 100-115 不在硬件中断表 (irq_map) 中显式定义.

**原因**: IOMMU IRQ 通过 `iommu_base_irq` 字段在 PCIe 配置中指定, 由 IOMMU 设备在运行时直接使用, 不需要在 irq_map 中声明.

**影响**: IOMMU 中断功能正常, 但中断向量表查询时找不到这些编号.

### 14.6 Sv39 稀疏 DDR 线性映射崩溃

**现象**: 不加 `mem=2G` 内核参数时, 内核在 `setup_vm_final()` 阶段崩溃或挂起（无输出）.

**原因**: Beta SoC 的 8 通道 DDR 起始于 `0x40_0000_0000`（256 GB 偏移），末尾通道（Ch7）结束于 `0xC0_0000_0000`（768 GB），总跨度约 452 GB。Linux Sv39 的直接映射区（linear map）从 `PAGE_OFFSET` 开始理论覆盖 512 GB，但起始偏移 + 通道间空洞导致线性映射无法全部覆盖，内核在映射 Ch1+ 通道时访问空洞地址，触发 early page fault。

**规避**:
1. 内核命令行加 `mem=2G`（仅使用 DDR0 前 2 GB，适合快速验证）
2. 应用 `0001-riscv-handle-sparse-DDR-outside-sv39-linear-mapping.patch`，跳过 Sv39 窗口外的稀疏区域
3. 内核默认使用 Sv48（不受此限制），`mem=2G` 是强制 Sv39 场景的必要参数

**影响**: 正常生产启动使用 `mem=2G` 规避，对裸机功能验证无影响。完整内存访问测试需要 Sv48 或上述 patch。

### 14.7 32 核 TCG 性能（macOS M4）

**现象**: macOS M4 上运行 32 核生产配置，从 QEMU 启动到 Linux shell 约需 **7 分钟**。

**原因**: macOS 不支持 KVM 硬件虚拟化，QEMU 使用 TCG 软件解释执行。32 个 ACPU 核心的 SMP bringup 需要依次处理每个核的 IMSIC IPI，TCG 多线程调度开销随核数线性增长。

**规避**: 调试时使用 `maxcpus=4`，启动时间缩短至约 1 分钟。32 核测试仅在验证 SMP 相关功能时使用。

---

## 15. 文件参考

### 15.1 Beta 专有源文件

| 文件路径 | 描述 |
|---------|------|
| `hw/riscv/beta.c` | 主 machine init 文件: 创建所有设备、CPU、内存、中断控制器 |
| `hw/riscv/beta_dtb.c` | ACPU FDT + MCPU FDT 生成, ISA 扩展列表 |
| `hw/riscv/beta_config.h` | 所有配置结构体定义 (BetaSoCConfig 等) |
| `hw/riscv/beta_config.c` | JSON 配置文件解析器 |
| `hw/misc/beta_soc_csr.c` | SOC_TOP_CSR 设备, 核心释放机制 (SW_RST_CTRL1 / CORE_RELEASE) |
| `hw/misc/beta_soc_csr.h` | SOC_TOP_CSR 头文件, BetaSoCCSRState 定义 |
| `hw/misc/beta_generic_mmio.c` | 通用 MMIO 存根设备 (JSON 驱动的寄存器映射) |
| `hw/misc/beta_generic_mmio.h` | 通用 MMIO 头文件 |
| `hw/riscv/beta.h` | Beta machine state 头文件 (BetaMachineState) |

### 15.2 修改的上游文件

| 文件路径 | 修改摘要 |
|---------|---------|
| `hw/pci-host/designware.c` | 5 项修改: Type 1 config 转发 (pci_find_bus_nr), ATU LIMIT 64-bit 修复, viewport 大小限制, MMIO 区域扩展 |
| `hw/pci-host/designware.h` | 对应头文件修改, 新增常量和结构体字段 |
| `hw/pci/pci-internal.h` | 可能的内部 API 调整以支持 pci_find_bus_nr 调用 |

### 15.3 配置文件

| 文件路径 | 描述 |
|---------|------|
| `beta_production_config.json` | 生产配置: 完整硬件参数, 8x4=32 ACPU, 8 DDR 通道, 4 PCIe 子系统 |
| `beta_test_config.json` (如存在) | 测试配置: 简化参数, 低地址, 旧版 CORE_RELEASE 机制 |

### 15.4 固件和脚本

| 文件路径 | 描述 |
|---------|------|
| `firmware/bootrom/mcpu_bootrom_production.S` | MCPU BootROM 汇编源码 |
| `scripts/run_production.sh` | 生产模式启动脚本 |

---

*本文档基于 QEMU 10.0.2 Beta SoC Machine Model 源代码自动生成. 所有地址、偏移量和配置值均从源代码和 JSON 配置文件中提取.*
