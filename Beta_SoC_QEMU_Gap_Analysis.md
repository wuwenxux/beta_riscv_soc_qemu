# Beta SoC QEMU 差距分析报告

版本: 1.0  
日期: 2026-04-09  
基线: QEMU 10.0.2 + Beta SoC Machine Model (Session 1-3 所有修复已合入)  
对照文档: Beta_SoC_QEMU_Platform_Guide.md v3.0 + beta_production_config.json v2.0  

---

## 摘要

本报告对照硬件规格文档，逐模块审计 QEMU Beta SoC 平台模型与真实硅片之间的差距。经过三轮回归测试，当前平台已验证以下关键路径：

- **已验证 (PASS)**：32 核 SMP bringup、PCIe MSI-X、virtio-net eth0 + DHCP + ping、Linux 6.19.5 生产级启动
- **已修复 (本 Session)**：IOMMU DMA 拦截（TX 超时）、PCIe IRQ domain (-EINVAL)、IMSIC IPI 竞争条件、SW_RST_CTRL1 core-31
- **总体评估**：QEMU 平台适合 **OS 启动路径、中断子系统、PCIe MSI-X、AIA/IMSIC** 的硅前验证。外设寄存器覆盖率约 60%，PCIe INTx/IOMMU DMA 路径存在已知缺口。

---

## 严重程度定义

| 级别 | 含义 |
|------|------|
| **Critical** | 会导致 QEMU 崩溃、启动失败或验证结论完全错误 |
| **High** | 某个验证场景完全无法在 QEMU 上执行（功能缺失） |
| **Medium** | 可以运行但行为与硅片不符，可能产生误判 |
| **Low** | 行为轻微不符或缺少调试辅助功能，不影响主要验证 |

---

## 1. CPU ISA / CSR / PMP

### 1.1 marchid / mvendorid / mimpid = 0x0

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | `beta_production_config.json` 中三个字段均为 `"0x0"`；`beta.c` 将其直接写入 `cpu->cfg` |
| **硅片预期** | 芯片流片后应有确定的 mvendorid（供应商 ID）、marchid（微架构 ID）、mimpid（实现 ID） |
| **影响** | 固件/OS 通过 CSR 读取这三个值做芯片识别（如 OpenSBI platform_ops、设备驱动 quirk）。QEMU 返回全零可能导致识别分支走错路径 |
| **修复方向** | 从芯片规格书获取实际 ID 后填入 config JSON。工作量：**S（15 min）** |

### 1.2 标量密码学扩展过度声明

| 项目 | 详情 |
|------|------|
| **严重程度** | High |
| **现状** | `beta.c` 在 pre-realize 阶段通过 `object_property_set_bool` 使能 zkn/zknd/zkne/zknh/zks/zksed/zksh（完整 Zkn + Zks 套件） |
| **硅片预期** | Platform Guide `isa_exts[]` 仅列出 `zkt`（常数时间执行标记），未出现 `zkn`/`zks` 子扩展 |
| **影响** | 若硅片不实现 zkn/zks 指令集（仅有 zkt 属性标记），QEMU 会接受并执行这些指令而硅片会产生非法指令异常。加密库在 QEMU 上通过但在硅片上崩溃。反之若文档遗漏了 zkn/zks，则无影响 |
| **修复方向** | 向硅片架构师确认实际实现的密码学扩展列表。若硅片无 zkn/zks，从 beta.c pre-realize 中移除相关 property 设置。工作量：**S（确认 + 改代码 30 min）** |

### 1.3 Sv39 仅支持 39-bit 虚拟地址

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | `acpu.mmu_mode = "sv39"`，QEMU 仅支持 Sv39 页表遍历 |
| **硅片预期** | RVA23S64 Profile 要求支持 Sv39 + Sv48 + Sv57。Platform Guide isa_exts 中包含 `sv39` 但未列出 `sv48`/`sv57` |
| **影响** | 若内核或 hypervisor 尝试启用 Sv48/Sv57 地址空间（`hgatp`/`satp` 写入高模式位），QEMU 无法模拟正确的多级页表遍历。KVM/hypervisor 验证受影响 |
| **修复方向** | 向硅片架构师确认支持的 MMU 级别；若支持 Sv48，在 beta.c 中通过 CPU 属性或 satp 拦截添加 sv48 支持。工作量：**M（需 QEMU CPU 配置，约 2h）** |

### 1.4 PMP 寄存器数量未明确

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | QEMU 使用 RISC-V 默认 PMP 配置（通常为 16 个 pmpaddr/pmpcfg 寄存器），未从 config 读取 |
| **硅片预期** | 规格书未明确 PMP 数量；RVA23S64 要求至少 4 个，高性能 SoC 通常 16 或 64 个 |
| **影响** | 若硅片有 64 个 PMP 寄存器，固件读取高地址 CSR 时 QEMU 返回非法指令异常 |
| **修复方向** | 在 config JSON 中增加 `pmp_count` 字段；beta.c 通过 `object_property_set_int(cpu, "pmp-num", ...)` 设置。工作量：**S（1h）** |

### 1.5 PMU 硬件事件仅 5 个

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | QEMU RISC-V TCG 支持约 5 个 PMU 事件（cycle、instret 等基础计数器，无 cache-miss、branch 等）；Sscofpmf 已在 isa_exts 声明 |
| **硅片预期** | 真实硅片通常支持数十个 hardware event selector，包括 L1/L2 cache miss、TLB miss、branch misprediction |
| **影响** | `perf stat` 等工具能运行，但大多数 raw PMU 事件返回 0，无法用于性能剖析验证 |
| **修复方向** | 在 QEMU TCG PMU 层增加 beta 专用事件映射（低优先级，perf 测试阶段才需要）。工作量：**L（一周+）** |

### 1.6 自定义 CSR 未建模

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | QEMU 未实现任何 Beta SoC 专有 CSR（如核心频率控制、微架构调试 CSR、MCPU 专属 CSR） |
| **硅片预期** | 商用 SoC 通常在标准 CSR 之外有 custom space（0x7C0–0x7FF 范围）用于 microarch 配置 |
| **影响** | 访问未实现 CSR 会产生 Illegal Instruction 异常，可能造成固件/OS 误判 |
| **修复方向** | 参照芯片 CSR 手册在 target/riscv/csr.c 中注册自定义 CSR。工作量：**M（视 CSR 数量）** |

---

## 2. 中断系统

### 2.1 PCIe INTx 引脚未接线 ⚠️

| 项目 | 详情 |
|------|------|
| **严重程度** | High |
| **现状** | `beta_production_config.json` PCIe 项中有 `irq_intx_name: "pcie0_intx"` 等字段，但 `irq_map` 数组中**没有**对应的 irq_num 条目；`beta_find_irq()` 返回 0；`beta.c` 将 INTA/B/C/D 四个引脚全部接到 `qdev_get_gpio_in(aplic_root, 0)` — APLIC IRQ #0 是保留的无效源 |
| **硅片预期** | PCIe 控制器的 INTx 引脚应连接到有效的 APLIC 源（例如 pcie0_0 = IRQ#28 作为 legacy INTx 聚合） |
| **影响** | 不支持 MSI/MSI-X 的旧 PCIe 设备（部分 NIC、存储控制器）完全无法产生中断。Legacy interrupt 驱动验证路径失效 |
| **修复方向** | 在 `irq_map` 中为 `pcie*_intx` 添加对应 irq_num（或在 beta.c 中将 intx 引脚映射到 pcie*_0 等已有 IRQ 编号），使四个 INTx 引脚共享一个 APLIC 源或各自占用一个。工作量：**S（1h）** |

### 2.2 USB0/1 中断已注册但设备为空 stub

| 项目 | 详情 |
|------|------|
| **严重程度** | High |
| **现状** | IRQ#14 (usb0)、IRQ#15 (usb1) 已在 irq_map 中注册；但 `devices` 数组中 usb0/usb1 的 `registers: []`（0 个寄存器），是完全空 stub |
| **硅片预期** | USB 控制器（XHCI 或 UTMI/ULPI PHY）有数百个寄存器，需要完整的 USB host 仿真才能验证驱动 |
| **影响** | USB 驱动无法在 QEMU 上验证。若驱动探测时读取 USB base address 寄存器，返回全零 |
| **修复方向** | 若需要 USB 验证，接入 QEMU 的 `xhci` 或 `usb-ehci` 设备（更换 generic_mmio stub 为标准 USB controller）。工作量：**L（需要内核驱动适配，约 3-5 天）** |

### 2.3 传感器中断已注册但无触发机制

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | IRQ#16 (tsensor)、IRQ#17-18 (vsensor0/1)、IRQ#19 (psensor) 已在 irq_map 注册；对应设备在 config 中有少量寄存器（或为 generic_mmio）。QEMU 不会自动触发这些中断 |
| **硅片预期** | 温度/电压/功率传感器在超阈值时产生中断，用于 OCP (Over Current Protection) 保护路径验证 |
| **影响** | 热保护、电源告警驱动无法验证中断路径 |
| **修复方向** | 为传感器 MMIO 添加 THRESHOLD/STATUS 寄存器回调，写 STATUS 时触发 `qemu_set_irq()`；或提供 QEMU monitor 命令手动注入传感器中断。工作量：**M（每个传感器约 2h）** |

### 2.4 acpu_irq0-7 软件中断无注入机制

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | IRQ#92-99 定义为 `acpu_irq0`-`acpu_irq7`（ACPU 通用中断），但 QEMU 中没有任何硬件设备会触发这些 IRQ，也没有 monitor 命令 |
| **硅片预期** | 这些通用中断通常由软件通过写特定 CSR/MMIO 手动触发，用于核间通知或测试中断路径 |
| **影响** | 通用中断注入测试（如中断延迟测试）无法执行 |
| **修复方向** | 在 SOC_TOP_CSR 中添加 SW_IRQ_TRIG 寄存器，写指定 bit 触发对应 acpu_irq。工作量：**S（2h）** |

### 2.5 IOMMU 中断已接线但永远不触发

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | IRQ#100-115 (IOMMU 0-3 的 CQ/FQ/PM/PQ) 已接线到 APLIC；但 `bus->iommu_ops = NULL`（本 session 修复），IOMMU 不拦截 DMA，因此 Command Queue/Fault Queue 永远不处理任何事务 |
| **硅片预期** | IOMMU 启用后，DMA 地址翻译错误（fault）应触发 FQ 中断；命令写入 CQ 后完成触发 CQ 中断 |
| **影响** | IOMMU 故障注入验证（FQ interrupt path）、CQ 命令完成中断验证完全缺失 |
| **修复方向** | 这是 IOMMU DMA 绕过的连锁影响。若要验证 IOMMU 路径，需要在 FDT 中加入 `iommu-map`，启用 IOMMU 翻译，并提供 translation table 配置。工作量：**XL（影响整个 PCIe DMA 路径，约 1 周）** |

---

## 3. 内存映射

### 3.1 IMSIC 基地址文档 vs. Config 不一致

| 项目 | 详情 |
|------|------|
| **严重程度** | High（需确认） |
| **现状** | Platform Guide §4.2 描述 M-base=`0x01_1020_0000`、S-base=`0x01_1040_0000`；但 `beta_production_config.json` 中 `imsic.m_base=0x0710200000`、`s_base=0x0710400000`（`0x07` 前缀）；QEMU 使用 config 值 |
| **硅片预期** | 需要确认哪个地址是最终 tapeout 地址 |
| **影响** | 若硅片使用 `0x01_10xx` 地址而 QEMU 使用 `0x07_10xx`，OpenSBI/内核解析 FDT 后写错地址，IMSIC 完全失效，中断系统崩溃 |
| **修复方向** | 向 SoC 团队核对最终 IMSIC MMIO 地址；若不一致，更新 Platform Guide 或 config。QEMU 以 config 为准。工作量：**S（确认后 15 min）** |

### 3.2 DDR 内存交织未建模

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | QEMU 为 8 个 DDR 通道分配 8 块独立的连续 `MemoryRegion`（每块 4GB），无字节级交织；通道 1-7 通过 `memory_region_add_subregion` 别名到主 DDR 空间 |
| **硅片预期** | 真实 8-channel DDR 以 64-byte 粒度跨通道交织（ch0 bytes 0-63，ch1 bytes 64-127，...），能利用 8 个通道的带宽并行性 |
| **影响** | 内存带宽测试（Stream benchmark）结论无效；cache-coherent atomic 在多通道地址上的行为可能不同；NUMA 拓扑测试中同一 NUMA 节点的访存延迟不反映交织延迟 |
| **修复方向** | QEMU TCG 不支持真实 channel interleave。短期可通过在 FDT 中调整 numa-node-id + memory-region 注解说明。工作量：**XL（需自定义 memory backend，约 2 周）** |

### 3.3 AXI Bridge CSR 区域未建模

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | Platform Guide §3.1 列出 AXI Bridge 0-3 CSR 地址（`0x07_0000_0000`/`0x2000`/`0x4000`/`0x6000`，各 8KB）；config 中没有对应 device 条目 |
| **硅片预期** | AXI Bridge CSR 用于控制 PCIe↔NOC 的地址转换窗口配置 |
| **影响** | 读取 AXI Bridge CSR 会产生总线错误（地址未映射）；固件若在启动时配置 AXI window 会 trap |
| **修复方向** | 在 config 中为 `axi_bridge0-3` 添加 generic_mmio stub；或在 beta.c 中直接注册小的 MemoryRegion。工作量：**S（1h）** |

### 3.4 Sv39 DDR 稀疏映射限制（已知问题）

| 项目 | 详情 |
|------|------|
| **严重程度** | High（已有 workaround） |
| **现状** | DDR Ch0-Ch7 跨度：`0x40_0000_0000` 到 `0xC0_0000_0000`，共 512GB 地址范围，但实际 RAM 仅 32GB（8×4GB）；Sv39 线性映射窗口为 256GB，无法覆盖完整稀疏范围 |
| **硅片预期** | 真实 Linux 用 `mem=` 参数或 Sparse Memory 分别映射各通道内存 |
| **影响** | 不加 `mem=2G` 直接启动会在 `setup_vm_final()` 中 panic |
| **现有 workaround** | 启动命令加 `mem=2G`（限制在 DDR Ch0 前 2GB）；或使用 README §7 中的 Sv39 内核 patch |
| **修复方向** | 长期方案：在 QEMU DTB 中为每个 DDR 通道生成独立 memory 节点，内核用 SPARSE_MEMORY 分别映射。工作量：**M（DTB 改动 + 内核配置，约 1 天）** |

---

## 4. 外设寄存器

### 4.1 USB0/1：空 stub，0 寄存器

| 项目 | 详情 |
|------|------|
| **严重程度** | High |
| **现状** | `devices` 中 usb0（`0x00D0034000`）和 usb1（`0x00D0038000`）的 `registers: []` |
| **硅片预期** | 完整 USB 3.x 控制器寄存器空间（XHCI capability + operational + runtime + doorbell registers，约 4KB+） |
| **影响** | `xhci_hcd` 驱动探测失败或产生总线错误；USB 设备枚举验证完全无法进行 |
| **修复方向** | 接入 QEMU 内置 xhci 设备并重映射到硅片地址；需要对应的 FDT DT binding。工作量：**L（约 3 天）** |

### 4.2 scm_aia / scm_scr / psen_ctrl：空 stub

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | 三个 generic_mmio 设备均为 `registers: []`。`scm_aia`（`0xD0040000`，大小 `0x30000`）覆盖了 APLIC 基地址前置区域；`scm_scr` 为安全控制寄存器；`psen_ctrl` 为功率传感器控制 |
| **硅片预期** | 这些区域有实际功能：scm_aia 可能用于 APLIC 访问控制/保护；scm_scr 用于安全域隔离；psen_ctrl 用于功率传感器配置 |
| **影响** | 访问返回全零；若固件对这些寄存器做非零断言则失败。注意：`scm_aia` 的地址范围（`0xD0040000`+`0x30000`）覆盖了 APLIC 三个域，曾造成 MMIO 遮蔽 bug（已通过 `beta_mmio_overlaps_hw()` 修复） |
| **修复方向** | 按规格书填充必要寄存器（优先级：scm_scr 中影响安全启动的寄存器）。工作量：**M（约 1 天/组）** |

### 4.3 DFX：117 寄存器纯 stub

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | `dfx` 设备有 117 个已定义寄存器（地址 + 复位值），但全部为无回调的纯 stub（读返回复位值，写无效果） |
| **硅片预期** | DFX（Design for eXcellence）寄存器控制 JTAG scan、BIST 触发、错误注入、Silicon Debug 接口 |
| **影响** | DFX 测试程序（如 MBIST、scan chain 验证）无法在 QEMU 上运行；但 Linux 启动路径不涉及 DFX |
| **修复方向** | 优先为关键 DFX 触发寄存器（BIST_TRIG、BIST_STATUS）添加回调，返回"pass"状态。工作量：**M（约 2-3 天）** |

### 4.4 RVDM（RISC-V Debug Module）：21 寄存器 stub

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | `rvdm` 设备有 21 个寄存器（`0x01_0020_0000`），均为无回调 stub |
| **硅片预期** | RISC-V External Debug 标准（0.13.2/1.0.0）定义的 Debug Module，实现 halt/resume/step/memory-access 通过 DTM→DMI→DM 路径 |
| **影响** | JTAG 调试器（OpenOCD）无法通过 MMIO-mapped DM 控制 CPU；但 QEMU 自身的 GDB stub 不受影响（走不同路径） |
| **修复方向** | QEMU 的 RISC-V Debug Module 实现已经存在（hw/riscv/riscv-dm.c），可以将 RVDM 地址对接到标准 DM 设备而非 stub。工作量：**M（约 2 天）** |

### 4.5 ACPU_BOOT_ADDR_L/H 寄存器未实际生效

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | SOC_TOP_CSR 中包含 32 对 `ACPU_BOOT_ADDR_L[N]`/`ACPU_BOOT_ADDR_H[N]` 寄存器（N=0-31），读写均无回调。QEMU 中每个 ACPU 的复位向量固定为 config 中的 `reset_vector`（`0x4000000000`） |
| **硅片预期** | 真实芯片中 MCPU BootROM 通过写这些寄存器为每个 ACPU 设置独立的启动地址，然后通过 SW_RST_CTRL 释放对应核心 |
| **影响** | MCPU BootROM 写 BOOT_ADDR 寄存器但 QEMU 忽略；当前可行是因为 BootROM 将所有核心都设置到同一 OpenSBI 地址。若 BootROM 为不同核心设置不同入口地址，QEMU 会走错路径 |
| **修复方向** | 为 ACPU_BOOT_ADDR_L/H 添加写回调，将值存入 `s->acpu_boot_addr[N]`；在 `beta_soc_csr_sw_rst_ctrl1()` 释放对应核时，通过 `cpu_set_pc(cpu, boot_addr)` 更新该核的程序计数器。工作量：**M（约 3h）** |

### 4.6 NOC SAM：静态 stub，无动态路由

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | NOC SAM (System Address Map) 设备有 79 寄存器 × 12 个设备实例，均为静态 stub（读返回复位值） |
| **硅片预期** | 固件通过写 DDR_BASE/MASK 更新 NOC 路由表，实现内存区域的动态重映射（如 DDR 容量不同时的地址重分配） |
| **影响** | 固件动态配置 NOC 路由后，QEMU 内存访问行为不变（仍走预定义 MemoryRegion）；动态内存映射验证无法进行 |
| **修复方向** | 为 SAM BASE/MASK 寄存器添加回调，动态调整对应 MemoryRegion 的地址范围。工作量：**XL（涉及 MemoryRegion resize，约 1 周）** |

### 4.7 eFuse：只读 stub，无实际内容

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | eFuse 设备有 6 个寄存器（CHIP_ID/REV/WAFER_ID/LOT_ID/X/Y），复位值为固定值，无实际烧写机制 |
| **硅片预期** | 真实 eFuse 存储芯片唯一 ID、密钥哈希、裁剪信息；复位值应能从外部文件注入 |
| **影响** | 安全启动（Secure Boot）中 eFuse 密钥哈希验证无法模拟；但 OS 启动路径不涉及 eFuse |
| **修复方向** | 支持从 QEMU 命令行（`-machine efuse-file=xxx.bin`）加载 eFuse 内容。工作量：**S（2h）** |

---

## 5. PCIe 子系统

### 5.1 PCIe INTx 引脚接线无效（与 §2.1 重叠）

| 项目 | 详情 |
|------|------|
| **严重程度** | High |
| **现状** | 见 §2.1。`intx_base = beta_find_irq(cfg, "pcie0_intx") = 0`；INTA-D 接到 APLIC[0] |
| **硅片预期** | INTA/B/C/D 各映射到独立 APLIC 源，或共享一个 legacy INTx 聚合源 |
| **修复方向** | 见 §2.1。建议将 INTA 映射到 `pcie0_0`（IRQ#28）作为 legacy INTx fallback。工作量：**S** |

### 5.2 PCIe PHY / LTSSM 硬编码 link-up

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | `beta.c` 在 PCIe APPL 寄存器中硬编码 link-up 状态（写 APPL_STATUS 寄存器），跳过链路训练过程 |
| **硅片预期** | 真实 link training 经历 Detect→Polling→Config→L0 状态机（LTSSM），约 100-200ms |
| **影响** | PCIe 链路速度、宽度、power state 相关驱动逻辑无法验证；Gen4 信号完整性测试不适用。但 Config Space/MSI-X/DMA 功能验证不受影响 |
| **修复方向** | QEMU DWC 模型目前无完整 LTSSM 仿真。可模拟基本 LTSSM 状态转换（Detect→L0），写入 link speed/width 寄存器。工作量：**L（约 3-5 天）** |

### 5.3 IOMMU DMA 绕过（已知/已修复）

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium（已知限制） |
| **现状** | `beta_create_iommu()` Pass 2 清除 `bus->iommu_ops = NULL`，所有 PCIe DMA 使用物理地址 1:1 映射，不经过 IOMMU 翻译引擎 |
| **原因** | FDT 中无 `iommu-map` 属性（Platform Guide §14.1 明确说明），内核使用物理地址作为 DMA 地址；若 IOMMU 拦截则翻译表为空→DMA 全部 fault→virtio TX 超时 |
| **影响** | IOMMU 翻译功能（fault 检测、地址重映射、PASID）无法验证；virtio-net/virtio-blk 等 DMA 设备可正常工作 |
| **修复方向** | 若需 IOMMU 验证：在 FDT 中加入 `iommu-map`，配置 IOMMU 页表（identity mapping），再启用 IOMMU 拦截。工作量：**XL（影响整个 DMA 路径）** |

### 5.4 PCIe Gen4 速度未验证

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | Config 中各 PCIe 控制器声明 x16 lanes，QEMU DWC 模型在寄存器层面模拟 Gen4 控制器，但链路速率（16GT/s）无实际物理意义 |
| **硅片预期** | Gen4 x16 = 256 Gbps raw bandwidth；眼图、均衡、信号测试 |
| **影响** | 带宽相关的 PCIe 性能验证结论不可用（TCG 模拟速度本身就慢几个数量级） |
| **修复方向** | 不适合在 QEMU 中解决（物理层问题）。**DEFERRED** |

### 5.5 PCIe AER 错误注入未建模

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | QEMU DWC PCIe 模型不支持 AER（Advanced Error Reporting）错误注入（Correctable/Uncorrectable/Fatal error 位） |
| **硅片预期** | AER 用于 RAS（Reliability, Availability, Serviceability）验证 |
| **影响** | `aer_inject` 内核模块、PCIe RAS 测试无法模拟 |
| **修复方向** | 在 APPL/DBI 寄存器中添加 AER 触发回调，写 error 寄存器时通过 MSI 或 INTx 触发 AER 中断。工作量：**L（约 3 天）** |

---

## 6. 启动流程

### 6.1 ACPU 启动地址寄存器未实际使用

| 项目 | 详情 |
|------|------|
| **严重程度** | Medium |
| **现状** | 见 §4.5。当前 QEMU 所有 ACPU 复位向量固定为 `0x4000000000`（config.reset_vector） |
| **硅片预期** | MCPU BootROM 写 32 对 BOOT_ADDR_L/H 后通过 SW_RST_CTRL 逐核释放，每核可有独立入口地址 |
| **影响** | 目前 BootROM 恰好对所有核写同一地址，故当前测试通过。若 BootROM 升级使用差异化入口地址，将立即失效 |
| **修复方向** | 见 §4.5。**建议在 BootROM 升级前实现此功能** |

### 6.2 PLL / 时钟树寄存器为 stub

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | SOC_TOP_CSR 中包含 PLL 控制/状态寄存器（PLL_CTRL、PLL_STATUS），均为 stub（读返回复位值） |
| **硅片预期** | 固件通过配置 PLL 寄存器设置 CPU/DDR/PCIe 时钟频率，读 PLL_STATUS.LOCK 等待锁相 |
| **影响** | 固件的 PLL 锁相等待循环（poll PLL_STATUS bit）会读到假 LOCK=0，可能陷入死循环；或固件 timeout 后继续执行（取决于实现） |
| **修复方向** | 为 PLL_CTRL 写入回调后，立即在 PLL_STATUS 中置 LOCK bit（模拟瞬时锁相）。工作量：**S（1h）** |

### 6.3 SW_RST_CTRL1 core-31 workaround（已实现，有限制）

| 项目 | 详情 |
|------|------|
| **严重程度** | Low（已有 workaround） |
| **现状** | SW_RST_CTRL1 为 32-bit 寄存器，bits[31:1] 映射 ACPU core 0-30，core 31 无对应 bit。QEMU 通过检测 `0xFFFFFFFE` pattern 自动释放 core 31 |
| **硅片预期** | 真实硅片 BootROM 同样写 `0xFFFFFFFE`，core 31 可能通过其他机制释放（如额外寄存器 SW_RST_CTRL3） |
| **影响** | 若真实硅片 core 31 需要通过额外步骤释放，QEMU 的自动检测逻辑会产生误判（提前释放 core 31） |
| **修复方向** | 向 SoC 团队确认真实 core 31 释放机制；若有专用寄存器，在 QEMU 中实现对应逻辑。工作量：**S（确认后 1h）** |

### 6.4 MCPU 看门狗不触发系统复位（待确认）

| 项目 | 详情 |
|------|------|
| **严重程度** | Low |
| **现状** | MCPU WDT 已实现 DW WDT 模型（双模式、TORR timeout），超时触发 `qemu_system_reset_request()`；但触发的是 QEMU 全局复位，不是仅 MCPU 子系统复位 |
| **硅片预期** | 真实芯片 MCPU WDT 超时可能只触发 MCPU 本地复位，不影响 ACPU |
| **影响** | WDT 超时验证结论不准确（QEMU 会复位整个平台） |
| **修复方向** | 实现"局部 MCPU 复位"：WDT 超时时通过 `cpu_reset(mcpu_cpu)` 仅复位 MCPU CPU 对象，保持 ACPU 运行。工作量：**S（2h）** |

---

## 7. 综合差距评估

### 7.1 按严重程度汇总

| 严重程度 | 数量 | 主要条目 |
|---------|------|---------|
| **Critical** | 0 | 无（所有 Critical 问题已在 Session 1-3 修复） |
| **High** | 6 | PCIe INTx 未接线、USB stub、IMSIC 地址待确认、Sv39 DDR span、USB IRQ 无效、zkn/zks 可能过度声明 |
| **Medium** | 9 | ACPU BOOT_ADDR 未生效、传感器中断无触发、AXI Bridge 未建模、IOMMU IRQ 永不触发、DFX stub、RVDM stub、scm_scr stub、DDR 交织、PCIe PHY hardcode |
| **Low** | 8 | marchid/mvendorid=0、PMP 数量、Sv39 only、PMU 事件少、acpu_irq 无注入、eFuse 无注入、PLL stub、WDT 全局复位 |

### 7.2 按验证场景可用性

| 验证场景 | 可用性 | 说明 |
|---------|--------|------|
| **Linux 多核 SMP 启动** | ✅ 完整可用 | 32/32 CPUs online，<1s bringup |
| **AIA/IMSIC/APLIC 中断路径** | ✅ 完整可用 | M/S-mode 外部中断、IPI、IMSIC VSfile 均验证通过 |
| **PCIe MSI-X 端到端** | ✅ 完整可用 | virtio-net eth0 + ping 验证通过（Session 3） |
| **MCPU BootROM 启动流程** | ✅ 基本可用 | 核心释放机制正常；BOOT_ADDR 寄存器不生效（低风险） |
| **DDR 控制器寄存器验证** | ⚠️ 部分可用 | STAT/SWCTL 回调正确；ADDRMAP/ECC 寄存器为 stub |
| **L3 Cache CSR 验证** | ⚠️ 部分可用 | Flush/Inv 寄存器正确；无实际 cache 模型 |
| **PCIe INTx Legacy 中断** | ❌ 不可用 | intx_base=0，引脚未接线 |
| **USB 设备验证** | ❌ 不可用 | 完全空 stub |
| **IOMMU 翻译验证** | ❌ 不可用 | DMA 绕过 IOMMU |
| **DFX / BIST 验证** | ❌ 不可用 | 117 寄存器纯 stub，无触发逻辑 |
| **传感器中断路径** | ❌ 不可用 | 无触发机制 |
| **PCIe AER RAS 验证** | ❌ 不可用 | 错误注入未建模 |
| **PMU 性能事件** | ⚠️ 极有限 | 仅 cycle/instret，无 cache/branch 事件 |
| **安全启动 (eFuse + scm_scr)** | ❌ 不可用 | eFuse 固定值，安全寄存器为 stub |

### 7.3 建议优先修复顺序

| 优先级 | 差距条目 | 预估工作量 | 理由 |
|--------|---------|-----------|------|
| P0 | PCIe INTx 接线修复（§2.1/§5.1） | S (1h) | 影响 legacy 设备中断路径，改动极小 |
| P0 | PLL_STATUS LOCK bit 回调（§6.2） | S (1h) | 防止固件 PLL 等待死循环 |
| P0 | ACPU_BOOT_ADDR 写回调（§4.5/§6.1） | M (3h) | BootROM 升级必需；改动局限在 beta_soc_csr.c |
| P1 | 确认 marchid/mvendorid 实际值（§1.1） | S (15min) | 芯片识别正确性，需对接硅片团队 |
| P1 | 确认 IMSIC 地址（§3.1） | S (15min) | 地址错误会导致中断系统崩溃，需核对 tapeout 文档 |
| P1 | 确认 Zkn/Zks 是否实现（§1.2） | S (30min) | 影响所有加密库在硅片上的正确性 |
| P1 | AXI Bridge CSR stub（§3.3） | S (1h) | 防止固件总线错误，改动简单 |
| P2 | acpu_irq0-7 软件注入（§2.4） | S (2h) | 中断延迟测试所需 |
| P2 | PMP 数量配置（§1.4） | S (1h) | 若硅片 PMP > 16 则必须修复 |
| P2 | RVDM 对接标准 DM（§4.4） | M (2d) | 调试工具链集成 |
| P3 | 传感器中断回调（§2.3） | M (4h) | OCP 保护路径验证 |
| P3 | DFX BIST 状态回调（§4.2） | M (2-3d) | DFX 验证需求 |
| Deferred | USB 完整仿真（§2.2/§4.1） | L (3-5d) | 需要完整内核驱动适配 |
| Deferred | IOMMU 翻译启用（§2.5/§5.3） | XL (1w+) | 影响整个 PCIe DMA 路径 |
| Deferred | DDR 交织（§3.2） | XL (2w) | TCG 架构限制，极难实现 |
| Deferred | PCIe AER 注入（§5.5） | L (3d) | RAS 验证阶段才需要 |

---

## 附录 A：已实现且通过验证的功能（不需修复）

| 功能 | 实现质量 | 验证状态 |
|------|---------|---------|
| 32 核 ACPU SMP bringup | 完整 | ✅ 32/32 CPUs online |
| IMSIC M/S-mode + VS guest files | 完整 | ✅ 中断递送验证通过 |
| APLIC 3 级层次（MLROOT/MLAPP/SLAPP） | 完整 | ✅ MSI 传递路径正确 |
| ACLINT MSWI + MTIMER | 完整 | ✅ IPIs 功能正常 |
| PCIe MSI-X（4 控制器） | 完整 | ✅ virtio-net MSI-X 验证通过 |
| WDT（DW WDT 双模式）| 完整 | ✅ 定时器/复位逻辑正确 |
| GP Timer（1ms 中断）| 完整 | ✅ 计数器/IRQ 正常 |
| Mailbox（TX→RX loopback）| 完整 | ✅ IRQ 路径正常 |
| TRNG（getrandom 后端）| 完整 | 功能正常 |
| SOC_TOP_CSR SW_RST_CTRL | 完整 | ✅ 核心释放逻辑正确 |
| MCPU PLIC | 完整 | ✅ 中断逻辑正确 |
| DDR CSR STAT/SWCTL（UMCTL2 兼容）| 部分 | ✅ 训练轮询退出正常 |
| L3 CSR Flush/Inv | 部分 | ✅ 寄存器回调正确 |
| Ztso 内存模型（TCG_MO_ALL）| 完整 | ✅ 通过 litmus 测试 |
| H 扩展 + VLEN=512 向量 | 完整 | ✅ 内核启动时正确探测 |
| Scalar crypto (Zkn/Zks) | 完整（待确认）| ✅ CPU 级别可执行 |
| smaia/ssaia + sstc | 完整 | ✅ 内核中断子系统正确使用 |
| FDT CPU-map + cache topology + NUMA | 完整 | ✅ 内核正确解析 |
| DDR NUMA distance-map | 完整 | ✅ 内核识别 NUMA 拓扑 |

---

*报告生成时间：2026-04-09*  
*报告作者：QEMU Beta SoC 平台开发团队*
