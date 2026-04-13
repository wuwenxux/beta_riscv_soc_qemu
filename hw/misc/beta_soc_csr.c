/*
 * Beta SoC - Top-level Control/Status Registers
 *
 * Supports two core release mechanisms:
 *
 * 1. Legacy (test config): CORE_RELEASE bitmask + shared BOOT_ADDR
 *    - CORE_RELEASE: bit N → release core N at BOOT_ADDR
 *
 * 2. Production: SW_RST_CTRL1 + per-core ACPU_BOOT_ADDR_L/H
 *    - SW_RST_CTRL1: bit (N+1) 0→1 transition → release core N
 *    - ACPU_BOOT_ADDR_L_N / ACPU_BOOT_ADDR_H_N: per-core 64-bit boot address
 *
 * Auto-detected based on register names in JSON config.
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/beta_soc_csr.h"
#include "hw/intc/riscv_imsic.h"
#include "exec/cpu-common.h"
#include "system/runstate.h"

/*
 * Release a single ACPU core: reset, set PC/a0/a1, resume.
 */
static void beta_soc_csr_release_core(BetaSoCCSRState *s, uint32_t core_idx,
                                      uint64_t boot_addr, uint64_t boot_arg)
{
    if (core_idx >= s->num_acpu_cores || !s->acpu_cores[core_idx]) {
        return;
    }

    CPUState *cpu = s->acpu_cores[core_idx];

    qemu_log_mask(LOG_UNIMP,
                  "beta-soc-csr: releasing ACPU core %u, "
                  "boot_addr=0x%" PRIx64 " boot_arg=0x%" PRIx64 "\n",
                  core_idx, boot_addr, boot_arg);

    cpu_reset(cpu);
    cpu->halted = 0;
    cpu->exception_index = -1;
    cpu_set_pc(cpu, boot_addr);

    if (s->set_hartid_fn) {
        s->set_hartid_fn(cpu, s->acpu_hartid_base + core_idx);
    }
    if (boot_arg && s->set_boot_arg_fn) {
        s->set_boot_arg_fn(cpu, boot_arg);
    }

    /*
     * Pre-enable M-level IMSIC eidelivery for this hart.
     *
     * Problem: OpenSBI sends an IPI to wake secondary harts by writing the
     * hart's M-level IMSIC seteipnum_le register.  riscv_imsic_update() only
     * raises the CPU interrupt line when eidelivery[0] == 1.  The IMSIC reset
     * state is eidelivery=0, and in TCG multi-threaded mode the boot hart can
     * send the IPI before the secondary hart has executed even one instruction
     * (let alone written eidelivery=1 via its own CSR).  The IPI is then lost
     * and secondary hart bringup hangs.
     *
     * Fix: set eidelivery[0]=1 here, before cpu_resume().  The secondary hart
     * will unconditionally overwrite this during its own M-mode init (OpenSBI
     * sets eidelivery=1 as part of AIA init), so there is no lasting effect on
     * correct firmware.  The pre-set merely closes the window where an early
     * IPI would be silently dropped.
     */
    if (core_idx < s->num_m_imsic && s->acpu_m_imsic &&
        s->acpu_m_imsic[core_idx]) {
        RISCVIMSICState *imsic = RISCV_IMSIC(s->acpu_m_imsic[core_idx]);
        if (imsic->eidelivery) {
            imsic->eidelivery[0] = 1;
        }
        /*
         * Pre-set interrupt ID 1 (IPI) as ENABLED+PENDING in eistate.
         *
         * riscv_imsic_topei() requires BOTH bits to assert the external IRQ:
         *   ENABLED  (bit 1, 0x2) — interrupt is unmasked
         *   PENDING  (bit 0, 0x1) — interrupt write has been received
         *
         * Why PENDING too: with only ENABLED pre-set, the secondary hart must
         * wait for OpenSBI to actually write seteipnum_le (the HART_START IPI)
         * before riscv_imsic_topei() returns non-zero and raises the IRQ.  In
         * TCG multi-threaded mode the boot hart can issue that MMIO write while
         * the secondary hart is still in an early CSR-init window where it has
         * not yet called cpu_resume(), causing an undetected-window IPI drop.
         *
         * With PENDING also pre-set the hart sees a pending IPI the instant
         * OpenSBI writes eidelivery=1 (which calls riscv_imsic_update()).  This
         * triggers a spurious M-level external interrupt; OpenSBI's hart-park
         * IPI handler will find no SBI mailbox entry, acknowledge and discard it
         * harmlessly, then re-enter WFI.  The real HART_START IPI arriving later
         * follows the normal path.  OpenSBI's AIA init unconditionally rewrites
         * eistate (via eie CSR writes), so the pre-set has no lasting effect.
         */
        if (imsic->eistate && imsic->num_irqs > 1) {
            /* 0x3 = ENABLED(bit1) | PENDING(bit0) */
            qatomic_or(&imsic->eistate[1], 0x3);
        }
    }

    cpu_resume(cpu);
}

/*
 * Legacy callback: CORE_RELEASE register write.
 * Bitmask — bit N releases core N at shared BOOT_ADDR.
 */
static void beta_soc_csr_core_release(void *opaque, const char *reg_name,
                                      uint64_t value)
{
    BetaSoCCSRState *s = BETA_SOC_CSR(opaque);
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(s);

    uint64_t boot_addr = 0;
    if (s->boot_addr_reg) {
        boot_addr = beta_generic_mmio_get_reg(mmio, s->boot_addr_reg);
    }

    uint64_t boot_arg = 0;
    if (s->boot_arg_reg) {
        boot_arg = beta_generic_mmio_get_reg(mmio, s->boot_arg_reg);
    }

    for (uint32_t i = 0; i < s->num_acpu_cores && i < 64; i++) {
        if (value & (1ULL << i)) {
            beta_soc_csr_release_core(s, i, boot_addr, boot_arg);
        }
    }
}

/*
 * Production callback: SW_RST_CTRL1 register write.
 *
 * Hardware bit-to-core mapping (confirmed from MCPU BootROM source):
 *   bit[0]    = sw_bacpu_core_rst_n  → BACPU (ignored; not an ACPU core)
 *   bits[31:1] = sw_acpu_core_rst_n  → ACPU cores 0-30 (bit N+1 → core N)
 *
 * SW_RST_CTRL1 has 31 ACPU bits (bits[31:1]), so it can only address 31
 * ACPU cores (0-30).  On a 32-core SoC, core 31 has no bit in this register.
 *
 * Core-31 workaround: the MCPU BootROM writes 0xFFFFFFFE intending to
 * release all 32 ACPU cores, but bits[31:1] only cover cores 0-30.  We
 * release core 31 automatically the first time all 31 ACPU bits (bits[31:1])
 * become set (cumulatively).  A flag ensures exactly one release per boot.
 *
 * Detect 0→1 transitions. Read per-core ACPU_BOOT_ADDR_L/H.
 */
static void beta_soc_csr_sw_rst_ctrl1(void *opaque, const char *reg_name,
                                      uint64_t value)
{
    BetaSoCCSRState *s = BETA_SOC_CSR(opaque);
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(s);
    uint32_t newly_released;

    /* Detect 0→1 transitions (cores being released from reset) */
    newly_released = (uint32_t)(value) & ~s->prev_sw_rst_ctrl1;
    s->prev_sw_rst_ctrl1 = (uint32_t)(value);

    /* Read boot argument (FDT address) — from register or QEMU fallback */
    uint64_t boot_arg = 0;
    if (s->boot_arg_reg) {
        boot_arg = beta_generic_mmio_get_reg(mmio, s->boot_arg_reg);
    }
    if (boot_arg == 0 && s->fdt_load_addr) {
        boot_arg = s->fdt_load_addr;
    }

    /* bits[31:1] → cores 0-30.  bit N+1 releases core N. */
    for (uint32_t bit = 1; bit <= 31; bit++) {
        if (!(newly_released & (1U << bit))) {
            continue;
        }

        uint32_t core_idx = bit - 1;
        if (core_idx >= s->num_acpu_cores) {
            continue;
        }

        /* Read per-core boot address: ACPU_BOOT_ADDR_L_N + ACPU_BOOT_ADDR_H_N */
        char reg_l[32], reg_h[32];
        snprintf(reg_l, sizeof(reg_l), "ACPU_BOOT_ADDR_L_%u", core_idx);
        snprintf(reg_h, sizeof(reg_h), "ACPU_BOOT_ADDR_H_%u", core_idx);

        uint64_t addr_l = beta_generic_mmio_get_reg(mmio, reg_l);
        uint64_t addr_h = beta_generic_mmio_get_reg(mmio, reg_h);
        uint64_t boot_addr = (addr_h << 32) | (addr_l & 0xFFFFFFFF);

        beta_soc_csr_release_core(s, core_idx, boot_addr, boot_arg);
    }

    /*
     * Core 31 release: triggered the first time all 31 ACPU bits (bits[31:1])
     * are set cumulatively.  SW_RST_CTRL1 can only address cores 0-30 via its
     * 31 ACPU bits; on a 32-core SoC the BootROM writes 0xFFFFFFFE to release
     * "all 32 cores" but bit 32 does not exist.  We infer the intent from the
     * "all-bits-set" state and release core 31 exactly once.
     */
    if (s->num_acpu_cores > 31 && !s->core31_released &&
        (s->prev_sw_rst_ctrl1 & 0xFFFFFFFE) == 0xFFFFFFFE) {
        s->core31_released = true;
        char reg_l[32], reg_h[32];
        snprintf(reg_l, sizeof(reg_l), "ACPU_BOOT_ADDR_L_31");
        snprintf(reg_h, sizeof(reg_h), "ACPU_BOOT_ADDR_H_31");
        uint64_t addr_l = beta_generic_mmio_get_reg(mmio, reg_l);
        uint64_t addr_h = beta_generic_mmio_get_reg(mmio, reg_h);
        uint64_t boot_addr = (addr_h << 32) | (addr_l & 0xFFFFFFFF);
        beta_soc_csr_release_core(s, 31, boot_addr, boot_arg);
    }
}

static void beta_soc_csr_reset_ctrl(void *opaque, const char *reg_name,
                                    uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
                  "beta-soc-csr: reset_ctrl write 0x%" PRIx64
                  " (not implemented)\n", value);
}

/*
 * SW_RST_CTRL0: system-level reset control.
 * Writing 0 to bit[0] (active-low) triggers a full system reset.
 */
static void beta_soc_csr_sw_rst_ctrl0(void *opaque, const char *reg_name,
                                      uint64_t value)
{
    /* Writing 0 to bit[0] (active-low) triggers system reset */
    if (!(value & 0x1)) {
        qemu_log_mask(LOG_UNIMP,
                      "beta-soc-csr: SW_RST_CTRL0=0x%" PRIx64
                      " → system reset requested\n", value);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

/*
 * SW_RST_CTRL2: per-peripheral reset control (active-low).
 * Detect 0→1 transitions (peripheral released from reset) and log them.
 */
static void beta_soc_csr_sw_rst_ctrl2(void *opaque, const char *reg_name,
                                      uint64_t value)
{
    BetaSoCCSRState *s = BETA_SOC_CSR(opaque);
    uint32_t released = (uint32_t)(value) & ~s->prev_sw_rst_ctrl2;
    s->prev_sw_rst_ctrl2 = (uint32_t)(value);

    if (released) {
        qemu_log_mask(LOG_UNIMP,
                      "beta-soc-csr: SW_RST_CTRL2=0x%08x → released: %s%s%s%s%s\n",
                      (uint32_t)value,
                      (released & (1U << 0))  ? "WDT " : "",
                      (released & (1U << 1))  ? "MBOX " : "",
                      (released & (1U << 4))  ? "UART " : "",
                      (released & (1U << 11)) ? "TRNG " : "",
                      (released & 0x01FE0000) ? "DDR " : "");
    }
}

/*
 * PLL CTRL0 callback: simulate instant PLL lock.
 *
 * Real hardware takes microseconds for PLL to lock after CTRL0 is written.
 * In the QEMU model we grant lock immediately so firmware polling loops
 * (waiting for PLL_STATUS[0] == 1) can exit without spinning.
 *
 * ACPU/MCPU/NOC PLL_STATUS are RW in the register map, so firmware could
 * accidentally clear them.  This callback restores the locked state whenever
 * the corresponding CTRL0 is re-written (i.e., after any PLL re-configuration).
 *
 * PCIE*_PLL_STATUS are RO with reset value 0x1, so they are always "locked"
 * without needing an explicit set here, but we still log the CTRL write.
 */
static void beta_soc_csr_pll_ctrl0(void *opaque, const char *reg_name,
                                    uint64_t value)
{
    BetaSoCCSRState *s = BETA_SOC_CSR(opaque);
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(s);
    const char *status_reg = NULL;

    if (g_strcmp0(reg_name, "ACPU_PLL_CTRL0") == 0) {
        status_reg = "ACPU_PLL_STATUS";
    } else if (g_strcmp0(reg_name, "MCPU_PLL_CTRL0") == 0) {
        status_reg = "MCPU_PLL_STATUS";
    } else if (g_strcmp0(reg_name, "NOC_PLL_CTRL0") == 0) {
        status_reg = "NOC_PLL_STATUS";
    } else if (g_strcmp0(reg_name, "PCIE0_PLL_CTRL0") == 0) {
        status_reg = "PCIE0_PLL_STATUS";
    } else if (g_strcmp0(reg_name, "PCIE1_PLL_CTRL0") == 0) {
        status_reg = "PCIE1_PLL_STATUS";
    } else if (g_strcmp0(reg_name, "PCIE2_PLL_CTRL0") == 0) {
        status_reg = "PCIE2_PLL_STATUS";
    } else if (g_strcmp0(reg_name, "PCIE3_PLL_CTRL0") == 0) {
        status_reg = "PCIE3_PLL_STATUS";
    }

    if (status_reg) {
        qemu_log_mask(LOG_UNIMP,
                      "beta-soc-csr: %s=0x%" PRIx64 " → %s forced locked\n",
                      reg_name, value, status_reg);
        beta_generic_mmio_set_reg(mmio, status_reg, 0x1);
    }
}

/*
 * XIP_CTRL: log mode changes.
 * bit[0] = xip_en — when set, MCPU boots from XIP flash.
 * No state-machine behavior needed in the QEMU model; software-visible
 * effect is that the flash MemoryRegion is already mapped unconditionally.
 */
static void beta_soc_csr_xip_ctrl(void *opaque, const char *reg_name,
                                    uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
                  "beta-soc-csr: XIP_CTRL=0x%08" PRIx64 " (xip_en=%d)\n",
                  value, (int)(value & 0x1));
}

/*
 * CG_CTRL: clock-gating control.
 * Log transitions so firmware can be traced; no clock model in QEMU.
 */
static void beta_soc_csr_cg_ctrl(void *opaque, const char *reg_name,
                                   uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
                  "beta-soc-csr: CG_CTRL=0x%08" PRIx64 "\n", value);
}

/*
 * BOOT_STATUS: firmware writes here to communicate boot progress.
 * Log writes for debug visibility; no hardware side effect modelled.
 */
static void beta_soc_csr_boot_status(void *opaque, const char *reg_name,
                                      uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
                  "beta-soc-csr: BOOT_STATUS=0x%08" PRIx64 "\n", value);
}

/*
 * PERIPH_CLK_DIV: peripheral clock divider.
 * bits[7:0] = divider ratio; reset value 0x1 (no division).
 * No functional clock model in QEMU; log for trace visibility.
 */
static void beta_soc_csr_periph_clk_div(void *opaque, const char *reg_name,
                                         uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
                  "beta-soc-csr: PERIPH_CLK_DIV=0x%08" PRIx64
                  " (div=%u)\n", value, (unsigned)(value & 0xff));
}

/*
 * SW_PLL_BYPASS_CTRL: select reference clock vs PLL output per domain.
 * Reset value 0xFF = all PLLs bypassed (system uses ref clock at cold boot).
 * Firmware clears a bit after PLL lock to switch that domain to PLL output.
 * No clock gating in QEMU; log transitions for trace visibility.
 *
 * Assumed bit mapping (verify against SOC_TOP_CSR register spec):
 *   bit[0]=MCPU, bit[1]=ACPU, bit[2]=NOC, bit[3]=PCIE0..bit[6]=PCIE3
 * TODO: confirm exact bit-to-domain assignment from hardware documentation.
 */
static void beta_soc_csr_sw_pll_bypass_ctrl(void *opaque, const char *reg_name,
                                             uint64_t value)
{
    static const char * const pll_names[] = {
        "MCPU", "ACPU", "NOC", "PCIE0", "PCIE1", "PCIE2", "PCIE3",
    };
    BetaSoCCSRState *s = BETA_SOC_CSR(opaque);
    uint32_t newly_bypassed   = (uint32_t)value  & ~s->prev_pll_bypass;
    uint32_t newly_unbypassed = ~(uint32_t)value &  s->prev_pll_bypass;
    s->prev_pll_bypass = (uint32_t)value;

    for (unsigned i = 0; i < ARRAY_SIZE(pll_names); i++) {
        if (newly_unbypassed & (1U << i)) {
            qemu_log_mask(LOG_UNIMP,
                          "beta-soc-csr: %s PLL bypass cleared"
                          " → PLL output selected\n", pll_names[i]);
        }
        if (newly_bypassed & (1U << i)) {
            qemu_log_mask(LOG_UNIMP,
                          "beta-soc-csr: %s PLL bypass set"
                          " → ref clock selected\n", pll_names[i]);
        }
    }
}

/*
 * SW_PLL_LOCK_FORCE_CTRL: software-force PLL lock status bits.
 * When bit N is set, the corresponding PLL_STATUS lock bit is forced high.
 * Used in bringup/test flows when the real PLL is absent or not started.
 *
 * Assumed bit-to-PLL mapping (same order as SW_PLL_BYPASS_CTRL):
 *   bit[0]=MCPU, bit[1]=ACPU, bit[2]=NOC, bit[3]=PCIE0..bit[6]=PCIE3
 * TODO: confirm exact bit-to-domain assignment from hardware documentation.
 */
static void beta_soc_csr_sw_pll_lock_force_ctrl(void *opaque,
                                                  const char *reg_name,
                                                  uint64_t value)
{
    static const struct { const char *status; } pll_status[] = {
        { "MCPU_PLL_STATUS"  },
        { "ACPU_PLL_STATUS"  },
        { "NOC_PLL_STATUS"   },
        { "PCIE0_PLL_STATUS" },
        { "PCIE1_PLL_STATUS" },
        { "PCIE2_PLL_STATUS" },
        { "PCIE3_PLL_STATUS" },
    };
    BetaSoCCSRState *s = BETA_SOC_CSR(opaque);
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(s);

    for (unsigned i = 0; i < ARRAY_SIZE(pll_status); i++) {
        if (value & (1U << i)) {
            beta_generic_mmio_set_reg(mmio, pll_status[i].status, 0x1);
            qemu_log_mask(LOG_UNIMP,
                          "beta-soc-csr: SW_PLL_LOCK_FORCE bit[%u]"
                          " → %s forced locked\n",
                          i, pll_status[i].status);
        }
    }
}

static void beta_soc_csr_realize(DeviceState *dev, Error **errp)
{
    BetaSoCCSRState *s = BETA_SOC_CSR(dev);
    DeviceClass *parent_dc =
        DEVICE_CLASS(object_class_by_name(TYPE_BETA_GENERIC_MMIO));

    /* Chain to parent realize */
    if (parent_dc->realize) {
        parent_dc->realize(dev, errp);
        if (*errp) {
            return;
        }
    }

    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(s);

    if (s->core_release_reg) {
        if (g_strcmp0(s->core_release_reg, "SW_RST_CTRL1") == 0) {
            /* Production mode: per-core boot address */
            beta_generic_mmio_add_callback(mmio, s->core_release_reg,
                                           beta_soc_csr_sw_rst_ctrl1, s);
        } else {
            /* Legacy mode: shared boot address */
            beta_generic_mmio_add_callback(mmio, s->core_release_reg,
                                           beta_soc_csr_core_release, s);
        }
    }

    if (s->reset_ctrl_reg) {
        beta_generic_mmio_add_callback(mmio, s->reset_ctrl_reg,
                                       beta_soc_csr_reset_ctrl, s);
    }

    /* System-level reset control */
    beta_generic_mmio_add_callback(mmio, "SW_RST_CTRL0",
                                   beta_soc_csr_sw_rst_ctrl0, s);

    /* Per-peripheral reset control */
    beta_generic_mmio_add_callback(mmio, "SW_RST_CTRL2",
                                   beta_soc_csr_sw_rst_ctrl2, s);

    /*
     * PLL auto-lock stubs: set PLL_STATUS[0] (lock bit) on any CTRL0 write
     * so that firmware PLL-lock polling loops do not spin forever.
     */
    static const char * const pll_ctrl0_regs[] = {
        "MCPU_PLL_CTRL0",
        "ACPU_PLL_CTRL0",
        "NOC_PLL_CTRL0",
        "PCIE0_PLL_CTRL0",
        "PCIE1_PLL_CTRL0",
        "PCIE2_PLL_CTRL0",
        "PCIE3_PLL_CTRL0",
    };
    for (size_t i = 0; i < ARRAY_SIZE(pll_ctrl0_regs); i++) {
        beta_generic_mmio_add_callback(mmio, pll_ctrl0_regs[i],
                                       beta_soc_csr_pll_ctrl0, s);
    }

    /* XIP mode, clock gating, boot progress — log-only stubs */
    beta_generic_mmio_add_callback(mmio, "XIP_CTRL",
                                   beta_soc_csr_xip_ctrl, s);
    beta_generic_mmio_add_callback(mmio, "CG_CTRL",
                                   beta_soc_csr_cg_ctrl, s);
    beta_generic_mmio_add_callback(mmio, "BOOT_STATUS",
                                   beta_soc_csr_boot_status, s);

    /* Peripheral clock divider — log-only stub */
    beta_generic_mmio_add_callback(mmio, "PERIPH_CLK_DIV",
                                   beta_soc_csr_periph_clk_div, s);

    /*
     * Power management: PLL bypass and force-lock controls.
     *
     * SW_PLL_BYPASS_CTRL (reset=0xFF): all PLLs bypassed on cold boot; firmware
     * clears bits one by one after each PLL locks.  Transitions are logged;
     * no clock model exists in QEMU so there is no functional side effect.
     *
     * SW_PLL_LOCK_FORCE_CTRL: firmware (or test code) can force any PLL's
     * STATUS lock bit high without waiting for a real PLL to settle.  We
     * honour this by writing the corresponding *_PLL_STATUS register.
     *
     * WFI behaviour: standard RISC-V WFI is handled by the upstream QEMU
     * RISC-V target — it halts the vCPU until any pending interrupt is
     * delivered.  No SoC-specific WFI trap register exists in this config,
     * so no additional MMIO callback is needed here.
     */
    beta_generic_mmio_add_callback(mmio, "SW_PLL_BYPASS_CTRL",
                                   beta_soc_csr_sw_pll_bypass_ctrl, s);
    beta_generic_mmio_add_callback(mmio, "SW_PLL_LOCK_FORCE_CTRL",
                                   beta_soc_csr_sw_pll_lock_force_ctrl, s);
}

static void beta_soc_csr_init(Object *obj)
{
    BetaSoCCSRState *s = BETA_SOC_CSR(obj);
    s->acpu_cores = NULL;
    s->num_acpu_cores = 0;
    s->core_release_reg = NULL;
    s->boot_addr_reg = NULL;
    s->boot_arg_reg = NULL;
    s->reset_ctrl_reg = NULL;
    s->acpu_hartid_base = 0;
    s->set_hartid_fn = NULL;
    s->set_boot_arg_fn = NULL;
    s->prev_sw_rst_ctrl1 = 0;
    s->prev_sw_rst_ctrl2 = 0;
    s->prev_pll_bypass = 0xff; /* matches SW_PLL_BYPASS_CTRL reset value */
    s->core31_released = false;
    s->acpu_m_imsic = NULL;
    s->num_m_imsic = 0;
}

static void beta_soc_csr_finalize(Object *obj)
{
    BetaSoCCSRState *s = BETA_SOC_CSR(obj);
    g_free(s->core_release_reg);
    g_free(s->boot_addr_reg);
    g_free(s->boot_arg_reg);
    g_free(s->reset_ctrl_reg);
    g_free(s->acpu_cores);
    g_free(s->acpu_m_imsic);
}

static void beta_soc_csr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = beta_soc_csr_realize;
    dc->desc = "Beta SoC top-level control/status registers";
}

static const TypeInfo beta_soc_csr_typeinfo = {
    .name = TYPE_BETA_SOC_CSR,
    .parent = TYPE_BETA_GENERIC_MMIO,
    .instance_size = sizeof(BetaSoCCSRState),
    .instance_init = beta_soc_csr_init,
    .instance_finalize = beta_soc_csr_finalize,
    .class_init = beta_soc_csr_class_init,
};

static void beta_soc_csr_register_types(void)
{
    type_register_static(&beta_soc_csr_typeinfo);
}
type_init(beta_soc_csr_register_types)

/* Public API */

void beta_soc_csr_setup(BetaSoCCSRState *s,
                        CPUState **acpu_cores,
                        uint32_t num_acpu_cores,
                        uint32_t acpu_hartid_base,
                        const char *core_release_reg,
                        const char *boot_addr_reg,
                        const char *boot_arg_reg,
                        const char *reset_ctrl_reg,
                        BetaSoCCSRSetHartIdFn set_hartid_fn,
                        BetaSoCCSRSetBootArgFn set_boot_arg_fn)
{
    s->acpu_cores = g_new0(CPUState *, num_acpu_cores);
    memcpy(s->acpu_cores, acpu_cores,
           num_acpu_cores * sizeof(CPUState *));
    s->num_acpu_cores = num_acpu_cores;
    s->acpu_hartid_base = acpu_hartid_base;

    s->core_release_reg = g_strdup(core_release_reg);
    s->boot_addr_reg = g_strdup(boot_addr_reg);
    s->boot_arg_reg = g_strdup(boot_arg_reg);
    s->reset_ctrl_reg = g_strdup(reset_ctrl_reg);
    s->set_hartid_fn = set_hartid_fn;
    s->set_boot_arg_fn = set_boot_arg_fn;
}

void beta_soc_csr_set_m_imsic(BetaSoCCSRState *s,
                               DeviceState **devs,
                               uint32_t count)
{
    g_free(s->acpu_m_imsic);
    s->acpu_m_imsic = g_new0(DeviceState *, count);
    memcpy(s->acpu_m_imsic, devs, count * sizeof(DeviceState *));
    s->num_m_imsic = count;
}
