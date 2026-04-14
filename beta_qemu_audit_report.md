# Beta SoC QEMU 仿真平台代码审计报告

**审计日期**：2026-04-09  
**审计对象**：Beta SoC QEMU（基于 QEMU 10.0.2 定制 fork）  
**审计范围**：ACPU ISA、AIA 中断系统、内存映射、启动流程、PCIe/IOMMU、DTB 生成、JSON 配置系统  
**审计目标**：硅前软件验证平台准确性评估

---

## 目录

1. [审计范围与文件清单](#1-审计范围与文件清单)
2. [问题汇总（按严重程度）](#2-问题汇总按严重程度)
3. [Critical 问题详述](#3-critical-问题详述)
4. [High 问题详述](#4-high-问题详述)
5. [Medium 问题详述](#5-medium-问题详述)
6. [Low 问题详述](#6-low-问题详述)
7. [专题审计：各模块现状](#7-专题审计各模块现状)
8. [总体结论与修复优先级](#8-总体结论与修复优先级)

---

## 1. 审计范围与文件清单

### 核心源文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `hw/riscv/beta.c` | 2340 | 主机器模型，设备创建与连线 |
| `hw/riscv/beta_config.c` | 678 | JSON 配置加载与解析 |
| `hw/riscv/beta_dtb.c` | 1458 | Device Tree Blob 生成 |
| `hw/misc/beta_soc_csr.c` | 298 | SoC CSR 设备，核心释放机制 |
| `hw/misc/beta_generic_mmio.c` | - | 配置驱动的通用 MMIO 框架 |
| `hw/riscv/beta_config.h` | - | 配置数据结构定义 |
| `hw/riscv/beta.h` | - | 机器级常量与类型声明 |
| `hw/misc/beta_soc_csr.h` | - | SoC CSR 设备头文件 |

### 配置文件

| 文件 | 说明 |
|------|------|
| `config/beta_production_config.json` | 生产配置（32 ACPU + 1 MCPU，8 通道 DDR） |
| `config/beta_acpu_direct_config.json` | ACPU 直接启动（跳过 MCPU，8 ACPU） |
| `config/beta_test_config.json` | 测试配置（legacy CORE_RELEASE 模式） |
| `config/beta_bifurcation_test_config.json` | PCIe 4x4 分叉测试配置 |

---

## 2. 问题汇总（按严重程度）

| ID | 严重程度 | 模块 | 问题简述 |
|----|----------|------|----------|
| C-01 | **Critical** | IOMMU/PCIe | IOMMU 未接入 DWC DMA 路径，导致地址翻译完全绕过 |
| C-02 | **Critical** | ISA/DTB | `marchid`/`mvendorid` CSR 未设置，软件无法识别 SoC 身份 |
| H-01 | **High** | ISA/DTB | Zkn/Zks 标量密码扩展未声明，与硬件规格不符 |
| H-02 | **High** | 启动流程 | ACPU 核心释放中 `acpu_hartid_base` 硬编码为 0，多集群拓扑可能错误 |
| H-03 | **High** | AIA | APLIC `num_sources` 默认值 1023 与配置文件中的实际分配不验证，IRQ 号可能越界 |
| M-01 | **Medium** | ISA | `misa_ext_mask` 未同步更新 `Hypervisor` 扩展（H-ext 写入路径遗漏） |
| M-02 | **Medium** | 内存映射 | DDR 8 通道交织仅通过别名（alias）模拟，无真实 cache-line 粒度交织行为 |
| M-03 | **Medium** | 启动流程 | MCPU AHB 扩展地址窗口（256 MB）硬编码，无越界检测 |
| M-04 | **Medium** | DTB | MCPU 不出现在 ACPU FDT 的 CPU 拓扑中，但某些 Linux 驱动依赖连续 hartid |
| M-05 | **Medium** | 配置系统 | 配置加载未校验 `irq_map` 中中断号是否超过 APLIC `num_sources` |
| L-01 | **Low** | ISA/DTB | `ss1p13`（Supervisor Spec 1.13 命名特性）不是标准 RISC-V ISA 扩展名 |
| L-02 | **Low** | DTB | L3 内存控制器节点注释为"当前进程不支持"，但 L3 RAM 已实际映射 |
| L-03 | **Low** | PCIe | PCIe Link training 状态硬编码为"link up"，无动态检测 |
| L-04 | **Low** | 代码质量 | `beta.c` 单文件 2340 行，多个子系统耦合，维护风险高 |
| L-05 | **Low** | 代码质量 | 多处魔数未定义为宏（如 MCPU hartid `32`、IMSIC guest bits `3`） |

---

## 3. Critical 问题详述

### C-01：IOMMU 未接入 DWC PCIe DMA 路径

**严重程度**：Critical  
**涉及文件**：`hw/riscv/beta_dtb.c:1145–1157`，`hw/riscv/beta.c:2015–2025`

**问题描述**：

RISC-V IOMMU 设备在 `beta.c:2015` 处创建，但**未连接到 DWC PCIe 主机控制器的 DMA 地址翻译路径**。代码本身通过注释明确承认了这一问题：

```c
/* beta_dtb.c:1145–1157 */
/*
 * iommu-map intentionally omitted.
 *
 * The RISC-V IOMMU device is not wired into the DWC PCIe
 * host bridge's DMA path in QEMU — PCI DMA goes directly
 * through the PCI address space alias to system memory.
 * Exposing iommu-map would make the kernel allocate IOVAs
 * for DMA buffers, but the device would DMA using those
 * IOVAs as raw physical addresses (no HW translation),
 * causing silent data corruption.  Without iommu-map the
 * kernel uses 1:1 DMA and everything works.
 *
 * TODO: wire IOMMU into DWC DMA path, then re-enable.
 */
```

**对硅前验证的影响**：

1. **IOMMU 驱动无法验证**：Linux 的 `riscv-iommu` 驱动不会被激活，任何依赖 IOMMU 的软件（如 VFIO、DPDK、虚拟化 I/O）都无法在此平台上验证。
2. **DMA 安全语义失效**：真实硅片上，PCIe 设备 DMA 地址必须经过 IOMMU 翻译；在仿真中直接使用物理地址，导致相关软件路径未被测试。
3. **DTB 故意遗漏 `iommu-map` 属性**：若后续修复 DTB 时直接添加 `iommu-map` 而未先修复 DMA 路径，将导致 kernel panic 或数据损坏。

**建议修复方向**：

1. 为 DWC PCIe 主机桥实现 `dma_map`/`dma_unmap` 回调，调用 RISC-V IOMMU 翻译 API。
2. 完成连线后，在 `beta_dtb.c` 中重新添加 `iommu-map` 属性。
3. 添加集成测试：验证 PCIe DMA 通过 IOMMU 页表正确翻译后可访问内存。

---

### C-02：`marchid`/`mvendorid` CSR 未设置

**严重程度**：Critical  
**涉及文件**：`hw/riscv/beta.c`（全文无相关代码）

**问题描述**：

全仓库中未找到任何对 `marchid`、`mvendorid` 的赋值代码。RISC-V 特权规范要求这两个 CSR 标识处理器的微架构实现和厂商信息。QEMU 的 `riscv_cpu_realize()` 默认将它们设为 0（或 QEMU 内部值），不反映 Beta SoC 实际值。

**对硅前验证的影响**：

1. **固件身份识别失败**：OpenSBI、U-Boot 等固件通常读取 `mvendorid`/`marchid` 来加载特定于 SoC 的 quirk 或驱动。仿真平台返回错误值，导致固件走不同的代码路径，验证结果无效。
2. **Linux `/proc/cpuinfo` 错误**：`riscv,isa` 和 `mvendorid` 信息用于内核 errata 检测（`HWCAP` 相关逻辑）。
3. **ACPI/SMBIOS 兼容性**：若未来支持 ACPI，`marchid` 是必要的标识字段。

**建议修复方向**：

在 `beta_create_acpu()` 函数中（`hw/riscv/beta.c`），在 CPU realize 后设置：

```c
/* 示意代码 */
RISCVCPU *riscv_cpu = RISCV_CPU(cpu);
riscv_cpu->env.mvendorid = BETA_MVENDORID;   /* 从 config 或宏读取 */
riscv_cpu->env.marchid   = BETA_MARCHID;
riscv_cpu->env.mimpid    = BETA_MIMPID;
```

同时在 `beta_config.h` 中定义 `BetaCPUConfig.mvendorid` 等字段，并从 JSON 配置文件加载。

---

## 4. High 问题详述

### H-01：Zkn/Zks 标量密码扩展未在 DTB 中声明

**严重程度**：High  
**涉及文件**：`hw/riscv/beta_dtb.c:216–253`

**问题描述**：

`isa_exts` 数组（第 216–253 行）声明了 62 个 ISA 扩展，但仅包含 `zkt`（Scalar Cryptography Key Derivation）和 `zvkt`（Vector Crypto），**缺少 `Zkn`（NIST 标准密码套件）和 `Zks`（ShangMi 密码套件）**。

如果 Beta 硅片规格包含 `Zkn`/`Zks`（通常它们是 `Zkt` 的超集），则当前 DTB 向操作系统宣告了错误的硬件能力：

```c
/* beta_dtb.c:226 — 当前仅有 zkt */
"zkt",
/* 缺少: "zkn", "zks", "zknd", "zkne", "zknh" 等子扩展 */
```

**对硅前验证的影响**：

- OpenSSL、内核 `crypto` 子系统等会查询 `riscv,isa-extensions` 来决定是否启用硬件加速密码路由；声明错误将导致软件走软件路径，无法验证硬件密码引擎的软件接口。
- 若硬件支持 `Zkn` 但未声明，AES/SHA 加速 intrinsic 不会被编译器和运行时启用。

**建议修复方向**：

对照 `文档/Beta ISA Status.xlsx` 核实硬件实际支持的密码扩展，然后在 `isa_exts` 数组中补充缺失条目：

```c
/* 示意，需按实际规格确认 */
"zkn",    /* NIST Scalar Crypto: includes zknd, zkne, zknh */
"zks",    /* ShangMi Scalar Crypto: includes zksed, zksh */
```

---

### H-02：ACPU `hartid_base` 硬编码为 0，多集群场景存在隐患

**严重程度**：High  
**涉及文件**：`hw/riscv/beta.c:1096–1118`，`hw/misc/beta_soc_csr.c:51`

**问题描述**：

在 `beta_soc_csr_setup()` 调用中（`beta.c:1096` 附近），`acpu_hartid_base` 参数硬编码为 `0`：

```c
/* beta.c:约1110 */
beta_soc_csr_setup(csr,
               acpu_cores, num_acpu,
               0,  /* acpu_hartid_base — 硬编码为 0 */
               ...);
```

而在 `beta_soc_csr.c:51`，core release 时的 hartid 计算为：

```c
s->acpu_hartid_base + core_idx   /* 生产配置中 base=0, idx=0..31 */
```

当前生产配置中 ACPU hartid 从 0 开始，MCPU hartid 为 32，因此 `base=0` 结果正确。但：

1. 若未来修改配置使 ACPU hartid 不从 0 开始（例如，MCPU 占用 hartid 0），此硬编码将导致所有 ACPU 核心的 hartid 计算错误，对 OpenSBI `hart_mask` 的处理产生影响。
2. `a0` 寄存器（hartid）传递错误会导致 OpenSBI 初始化失败或多核 SMP 启动异常。

**建议修复方向**：

将 `acpu_hartid_base` 从 JSON 配置中读取（`acpu.hartid_base` 字段），并在 `BetaACPUConfig` 结构中存储，避免硬编码。

---

### H-03：APLIC `num_sources` 未与 `irq_map` 交叉校验

**严重程度**：High  
**涉及文件**：`hw/riscv/beta.c:803`，`hw/riscv/beta_config.c`

**问题描述**：

APLIC 创建时，`num_sources` 来自配置或默认值 1023（`beta.c:803`）：

```c
uint32_t num_sources = acfg->num_sources ? acfg->num_sources : 1023;
```

配置加载阶段（`beta_config.c`）**未校验** `irq_map` 中任何中断号是否超过 `num_sources`。若某个设备配置了 `irq_num > num_sources`，该中断将被静默忽略或写入越界的 GPIO 输入，导致中断永远无法触发，而没有任何报错。

**对硅前验证的影响**：

- 设备中断失效是最难诊断的仿真 bug 之一（设备运行正常但中断不来）。
- 若 `irq_map` 中的条目是从硬件中断向量表（`文档/中断向量表.pdf`）自动生成的，任何表格更新都可能引入越界中断号。

**建议修复方向**：

在 `beta_config.c` 的配置验证阶段添加：

```c
for (i = 0; i < cfg->interrupts.num_irqs; i++) {
    if (cfg->interrupts.irq_map[i].irq_num >= cfg->interrupts.aplic.num_sources) {
        error_setg(errp, "irq_map[%s]: irq_num %u >= aplic.num_sources %u",
                   cfg->interrupts.irq_map[i].name,
                   cfg->interrupts.irq_map[i].irq_num,
                   cfg->interrupts.aplic.num_sources);
    }
}
```

---

## 5. Medium 问题详述

### M-01：Hypervisor 扩展仅写入 `misa_ext`，`misa_ext_mask` 更新存疑

**严重程度**：Medium  
**涉及文件**：`hw/riscv/beta.c:232–234`

**问题描述**：

H 扩展通过直接写 `env->misa_ext` 和 `env->misa_ext_mask` 启用：

```c
env->misa_ext      |= RVH;
env->misa_ext_mask |= RVH;
```

然而，QEMU RISC-V CPU 实现中，`misa_ext_mask` 控制的是哪些扩展允许被软件通过写 `misa` CSR 动态关闭。直接操作该字段会绕过 `riscv_cpu_cfg_merge()` 和 CPU profile 的标准配置通道，可能与后续 CPU reset 逻辑产生竞争：

- `cpu_reset()` 调用链会重新应用 CPU profile，若 profile 未包含 H 扩展，reset 后 H 可能被清除。
- 这在核心释放流程中尤为危险：`beta_soc_csr_release_core()` 调用 `cpu_reset(cpu)` 后再设置 PC，若 H 扩展被 reset 清除，虚拟化功能将在核心二次启动后失效。

**建议修复方向**：

通过 QEMU CPU property 接口启用 H 扩展，而非直接修改 `env` 字段：

```c
object_property_set_bool(OBJECT(cpu), "h", true, &error_abort);
```

或在 CPU 类型定义阶段通过 `riscv_cpu_cfg` 结构体设置 `cfg.ext_h = true`，确保扩展在 reset 后持久有效。

---

### M-02：DDR 8 通道交织仅用 alias 模拟，无 cache-line 粒度交织

**严重程度**：Medium  
**涉及文件**：`hw/riscv/beta.c:267–303`

**问题描述**：

生产配置要求 8 通道 DDR 以 64 字节（cache-line）粒度交织访问。当前实现将所有通道建模为一个连续内存区域，其余 7 个通道地址作为 alias 映射到同一底层 RAM：

```c
/* beta.c:288–301 示意 */
for (ch = 1; ch < num_channels; ch++) {
    memory_region_init_alias(&s->ddr_ch[ch], ...);
    memory_region_add_subregion(system_memory, channel_base[ch], &s->ddr_ch[ch]);
}
```

**对硅前验证的影响**：

- **可接受场景**：纯软件功能验证（Linux 启动、应用程序运行）不受影响。
- **不可验证场景**：
  - 软件依赖特定 DDR 通道地址的性能行为（`NUMA` 亲和性、内存带宽分配）
  - 测试 DDR 交织地址计算正确性的代码（如内存测试程序）
  - 多核高并发访问不同通道时的 coherency 行为

**建议修复方向**：

对于硅前 SW 验证，当前实现可接受；但应在文档中明确标注此限制，避免验证工程师误以为交织行为已被模拟。若需验证 DDR 地址计算，可为每个通道创建独立的 MemoryRegion 并实现翻译层。

---

### M-03：MCPU AHB 扩展地址寄存器无越界检测

**严重程度**：Medium  
**涉及文件**：`hw/riscv/beta.c:688–792`（AHB window 相关代码）

**问题描述**：

MCPU（RV32）通过写扩展地址寄存器（`0xE004_D00C`）的高 8 位来访问 40 位地址空间。当 MCPU 固件写入一个非法的高位值时（如超过物理地址范围），当前实现**没有任何范围检测**，直接将越界地址映射到 `system_memory`，可能访问到 MMIO 区域的随机位置，行为不可预测。

真实硅片上，此类越界访问会触发总线错误或产生未定义行为，应在仿真中复现。

**建议修复方向**：

在 AHB window 的写回调中添加地址范围校验，超范围时触发 QEMU 的 guest error 机制。

---

### M-04：ACPU FDT 中 CPU 拓扑不包含 MCPU，hartid 空间不连续

**严重程度**：Medium  
**涉及文件**：`hw/riscv/beta_dtb.c:839`（`beta_create_fdt()`）

**问题描述**：

ACPU 的 FDT 中，CPU 节点仅包含 ACPU（hartid 0–31），MCPU（hartid 32）完全不出现。ACPU hartid 空间连续（0–31），MCPU 跳过，形成不连续的 hartid 分配。

真实硅片上，`mhartid` 寄存器空间是连续的（MCPU 可能占用 hartid 0 或 32，取决于规格）。若规格要求 MCPU 使用 hartid 0，则当前 ACPU 从 hartid 0 开始的配置与硅片实际不符。

**对硅前验证的影响**：

- OpenSBI 的 `cold_boot` 入口基于 `mhartid==0` 判断主核；若 MCPU 实际占用 hartid 0，则 ACPU 的 hartid 分配需要对应调整。
- Linux `cpumask` 和 `cpu_online_mask` 基于 FDT 中的 hartid 构建；若 hartid 不连续，部分内核路径可能出现意外行为。

**建议修复方向**：

核实硬件规格中 MCPU 和 ACPU 的 `mhartid` 分配，确保 QEMU 配置与硅片一致。若 MCPU 应占用 hartid 0，则 ACPU 应从 hartid 1 开始（当前为 0）。

---

### M-05：`irq_map` 配置加载无完整性校验

**严重程度**：Medium  
**涉及文件**：`hw/riscv/beta_config.c`

**问题描述**：

JSON 配置中的 `irq_map` 数组被解析后直接使用，缺少以下校验：

1. **中断号范围**：irq_num 是否在 `[1, aplic.num_sources-1]` 范围内
2. **中断号唯一性**：是否有两个设备共享同一 irq_num（重复分配）
3. **设备引用完整性**：irq_map 中引用的设备名是否在 `devices` 列表中存在
4. **IMSIC vs APLIC 路由一致性**：MSI 设备的中断是否被错误路由到 APLIC wired 输入

**建议修复方向**：

在 `beta_config.c` 的 `beta_config_validate()` 函数中（或新增该函数）实现上述校验，在机器初始化早期报错，避免仿真运行后才发现中断配置错误。

---

## 6. Low 问题详述

### L-01：DTB 中声明了非标准 ISA 特性名 `ss1p13`

**严重程度**：Low  
**涉及文件**：`hw/riscv/beta_dtb.c:216–253`

**问题描述**：

`isa_exts` 数组中包含 `"ss1p13"`，这**不是**标准 RISC-V ISA 扩展命名规范中的条目。RISC-V International 的扩展命名规范使用如 `ss1p11`（Supervisor Spec version）等格式，但 `ss1p13` 当前不在已批准的扩展列表中，可能被 Linux 内核的 ISA 解析代码拒绝或忽略。

**建议修复方向**：

确认此条目是否为内部规范名称，若是，删除或替换为标准名称。

---

### L-02：L3 内存控制器注释与实现不一致

**严重程度**：Low  
**涉及文件**：`hw/riscv/beta_dtb.c`（L3_MEM_CTRL 相关注释）

**问题描述**：

代码注释表明"当前进程不支持 L3_MEM_CTRL_REG"，但 L3 RAM（8 个 1 MB OCM slice）已正常映射为可访问内存。控制器寄存器不可用意味着无法测试 L3 缓存管理相关软件路径（如 ECC 控制、锁定、分区）。

**建议修复方向**：

明确记录 L3 内存控制器的支持范围，或实现基本的 stub 寄存器（读返回合理默认值）。

---

### L-03：PCIe Link Status 硬编码为已连接状态

**严重程度**：Low  
**涉及文件**：`hw/riscv/beta.c`（PCIe 初始化部分）

**问题描述**：

PCIe 链路状态硬编码为"link up"，不模拟链路训练过程。虽然这对软件验证通常足够，但无法测试链路协商失败、重训练等异常处理路径。

---

### L-04：`beta.c` 单文件超过 2300 行，子系统高度耦合

**严重程度**：Low  
**涉及文件**：`hw/riscv/beta.c`

**问题描述**：

主机器文件将 ACPU 创建、MCPU 创建、DDR 映射、AHB window、APLIC/IMSIC、PCIe、IOMMU、启动流程等全部实现在单一文件中，共 2340 行。这使得：

- 局部修改的影响范围难以评估
- 代码审查困难
- 子系统之间的依赖关系隐式耦合

**建议修复方向**：

拆分为：`beta_acpu.c`、`beta_mcpu.c`、`beta_memory.c`、`beta_interrupt.c`、`beta_pcie.c`、`beta_boot.c`，每文件 300–500 行。

---

### L-05：多处关键魔数未定义为具名宏

**严重程度**：Low  
**涉及文件**：`hw/riscv/beta.c`，`hw/misc/beta_soc_csr.c`

**问题描述**：

以下数值在代码中直接使用，缺少具名宏：

| 数值 | 含义 | 出现位置 |
|------|------|----------|
| `32` | MCPU 的 hartid | `beta.c` 多处 |
| `3` | IMSIC guest_index_bits | `beta.c` |
| `4` | IMSIC VS guest files 数 | `beta.c` |
| `0xE0040000` | MCPU PLIC 基址 | `beta.c:670` |
| `0xA0000000` | MCPU AHB window 基址 | `beta.c:688` |

---

## 7. 专题审计：各模块现状

### 7.1 ACPU ISA 配置

| 项目 | 配置值 | 状态 | 说明 |
|------|--------|------|------|
| CPU Profile | RVA23S64 | ✓ | `TYPE_RISCV_CPU_RVA23S64`，`beta.c:159` |
| VLEN | 512 位 | ✓ | `cpu->cfg.vlenb = 512 >> 3`，`beta.c:229` |
| Hypervisor (H) | 启用 | ⚠ | 直接写 `env->misa_ext`，reset 安全性存疑，`beta.c:232` |
| Ztso | 启用 | ✓ | 通过 config 设置 |
| Zkn/Zks | **未声明** | ✗ | DTB `isa_exts` 中缺失，`beta_dtb.c:216` |
| marchid | **未设置** | ✗ | 全仓库无相关代码 |
| mvendorid | **未设置** | ✗ | 全仓库无相关代码 |
| 向量子扩展 | zvfhmin, zvbb, zvkt | ✓ | 已声明 |
| 位操作扩展 | Zba, Zbb, Zbs | ✓ | 已声明 |

### 7.2 AIA 中断系统

| 项目 | 状态 | 说明 |
|------|------|------|
| APLIC 三级拓扑 | ✓ | MLROOT→MLAPP→SLAPP，`beta.c:797–937` |
| IMSIC M/S 级 | ✓ | 每 hart 独立文件，255 IDs，4 VS guest |
| MSI 投递路径 | ✓ | APLIC→IMSIC，PCIe→IMSIC（绕过 APLIC） |
| IRQ 号来源 | ✓ | 全部来自 JSON `irq_map`，无硬编码 |
| irq_map 校验 | ✗ | 无越界/重复检测（H-03） |
| APLIC GPIO 连线 | ⚠ | 仅部分设备实际连线（UART、Timer、Mailbox、WDT） |

### 7.3 内存映射

| 区域 | 基址 | 大小 | 状态 |
|------|------|------|------|
| MCPU ITCM | `0xE460_0000` | 32 KB | ✓ |
| MCPU DTCM | `0xE480_0000` | 32 KB | ✓ |
| SOC_TOP_CSR | `0x0100_0000` | 4 KB | ✓ |
| ACLINT | `0x0600_0000` | 1 MB | ✓ |
| APLIC（三级） | 配置驱动 | 各 64 KB | ✓ |
| IMSIC（多集群） | 配置驱动 | 集群步进 16 MB | ✓ |
| DDR 通道 0–7 | `0x50–0xB0_0000_0000` | 每通道 4 GB | ⚠（交织未模拟）|
| PCIe DBI（4 控制器）| `0x140000000` 起 | 各 3 MB | ✓ |
| PCIe ECAM（4 控制器）| `0xC0–0xF000000000` | 各 256 MB | ✓ |
| IOMMU（4 实例）| `0x0700001800` 起 | 各 4 KB | ⚠（未接入 DMA）|
| L3 OCM（8 slice）| `0x7305_0000_00` 起 | 各 1 MB | ✓ |
| MCPU AHB 窗口 | `0xA000_0000` | 256 MB | ✓（无越界检测）|

### 7.4 启动流程

| 阶段 | 状态 | 说明 |
|------|------|------|
| MCPU BootROM 执行 | ✓ | hartid=32，复位向量 `0x00D0_0000_00` |
| 核心释放（SW_RST_CTRL1）| ✓ | 边沿触发检测，`beta_soc_csr.c:87–134` |
| 核心释放（CORE_RELEASE）| ✓ | 传统模式，自动检测，`beta.c:1096–1109` |
| per-core 启动地址 | ✓ | `ACPU_BOOT_ADDR_L/H_N` 寄存器，`beta_soc_csr.c:123` |
| a0/a1 寄存器设置 | ✓ | hartid→a0，FDT→a1，`beta.c:132–142` |
| OpenSBI FW_JUMP | ✓ | FDT 重定位到 `fw_jump + 32 MB` |
| ACPU 直接模式 | ✓ | 跳过 MCPU，快速开发用途 |
| hartid_base 硬编码 | ⚠ | `acpu_hartid_base=0` 硬编码（H-02）|

### 7.5 PCIe + IOMMU

| 项目 | 状态 | 说明 |
|------|------|------|
| DWC 控制器（4 个）| ✓ | 地址、分叉、MSI 配置正确 |
| ECAM 配置空间 | ✓ | 256 MB 窗口，标准 PCI 枚举 |
| IOMMU 设备创建 | ✓ | 4 个实例，`beta.c:2015–2025` |
| IOMMU→DMA 连线 | ✗ | **未实现（C-01）** |
| DTB `iommu-map` | ✗ | 故意省略，等待 DMA 修复 |
| Link Training | ⚠ | 硬编码"link up"（L-03）|

### 7.6 Device Tree 生成

| 项目 | 状态 | 说明 |
|------|------|------|
| CPU 拓扑节点 | ✓ | 8 cluster × 4 core，phandle 正确 |
| ISA 扩展声明 | ⚠ | 62 项，缺 Zkn/Zks（H-01） |
| APLIC/IMSIC 节点 | ✓ | 三级 APLIC + per-hart IMSIC |
| DDR 内存节点 | ✓ | 8 通道地址正确 |
| 中断映射 | ✓ | `interrupts-extended` 正确连线 |
| `iommu-map` | ✗ | 故意省略（C-01）|
| MCPU FDT | ✓ | 独立树，含 PLIC、AHB 窗口 |
| `ss1p13` 非标准名 | ⚠ | 可能被内核忽略（L-01）|

### 7.7 JSON 配置系统

| 项目 | 状态 | 说明 |
|------|------|------|
| JSON 解析 | ✓ | QEMU QObject API，Error** 模式 |
| 内存正确释放 | ✓ | `beta_config_free()` 完整清理 |
| ISA 格式校验 | ✓ | 必须以 "rv64"/"rv32" 开头 |
| irq_map 范围校验 | ✗ | 缺失（H-03，M-05）|
| 多配置文件支持 | ✓ | 4 个配置文件覆盖主要场景 |
| 配置版本管理 | ⚠ | `version` 字段存在但未做向后兼容处理 |

---

## 8. 总体结论与修复优先级

### 对硅前软件验证的整体评估

| 模块 | 可用于验证 | 备注 |
|------|-----------|------|
| Linux 启动（单核）| ✓ 完全可用 | |
| Linux SMP（多核）| ✓ 基本可用 | H-02 在当前配置下无影响 |
| RISC-V 向量指令 | ✓ 完全可用 | VLEN=512 正确 |
| 虚拟化（KVM/H-ext）| ⚠ 需验证 | H-ext reset 安全性（M-01）|
| 硬件密码加速 | ⚠ 部分受限 | Zkn/Zks 未声明（H-01）|
| PCIe 设备枚举 | ✓ 基本可用 | |
| PCIe DMA | ⚠ 受限 | 无 IOMMU 翻译（C-01）|
| IOMMU/VFIO 验证 | ✗ 不可用 | C-01 |
| SoC 身份识别 | ✗ 不可用 | C-02 |
| DDR 通道验证 | ⚠ 受限 | 交织未模拟（M-02）|

### 修复优先级建议

**立即修复（阻塞验证计划）：**
1. **C-02**：设置 `marchid`/`mvendorid`/`mimpid`——固件身份识别必需
2. **H-01**：补充 Zkn/Zks 到 `isa_exts`——对照规格文档确认后直接修改 `beta_dtb.c:226`
3. **H-02**：将 `acpu_hartid_base` 从配置读取，消除硬编码

**短期修复（影响验证质量）：**
4. **M-01**：通过 CPU property 接口启用 H-ext，避免 reset 清除风险
5. **H-03 + M-05**：添加 `irq_map` 完整性校验
6. **M-04**：核实 hartid 分配规格，确保 FDT 与硅片一致

**长期改进（不阻塞当前验证）：**
7. **C-01**：实现 IOMMU-DWC DMA 集成（工作量较大，优先级取决于是否需要验证 IOMMU 软件栈）
8. **L-01**：清理非标准 ISA 扩展名 `ss1p13`
9. **L-04**：重构 `beta.c` 为多文件结构

---

*报告生成时间：2026-04-09*  
*审计工具：静态代码分析 + 交叉对照规格文档*
