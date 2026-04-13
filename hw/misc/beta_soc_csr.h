/*
 * Beta SoC - Top-level Control/Status Registers
 *
 * Extends BetaGenericMMIO with side-effect callbacks for:
 *   - Core release (powering on ACPU cores)
 *   - Reset control
 *   - Boot address configuration
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef HW_BETA_SOC_CSR_H
#define HW_BETA_SOC_CSR_H

#include "hw/misc/beta_generic_mmio.h"
#include "exec/cpu-common.h"

/*
 * Callback types for setting ACPU boot registers (a0, a1).
 * Implemented in target-specific code (e.g., beta.c) which can access
 * RISCVCPU::env directly.
 */
typedef void (*BetaSoCCSRSetBootArgFn)(CPUState *cpu, uint64_t arg);
typedef void (*BetaSoCCSRSetHartIdFn)(CPUState *cpu, uint64_t hartid);

#define TYPE_BETA_SOC_CSR "beta-soc-csr"
OBJECT_DECLARE_SIMPLE_TYPE(BetaSoCCSRState, BETA_SOC_CSR)

struct BetaSoCCSRState {
    /*< private >*/
    BetaGenericMMIOState parent_obj;

    /*< public >*/
    /* ACPU core pointers for core release */
    CPUState **acpu_cores;
    uint32_t num_acpu_cores;

    /* Register names for side-effect operations (from config) */
    char *core_release_reg;
    char *boot_addr_reg;
    char *boot_arg_reg;     /* optional: ACPU a1 (FDT/fw_dynamic addr) */
    char *reset_ctrl_reg;

    /* ACPU hartid base (hartid = acpu_hartid_base + core_index) */
    uint32_t acpu_hartid_base;

    /* Previous SW_RST_CTRL1 value for 0→1 transition detection */
    uint32_t prev_sw_rst_ctrl1;

    /*
     * True once core 31 has been released via the "all-bits-set" heuristic.
     * SW_RST_CTRL1 bits[31:1] can only address 31 ACPU cores (0-30); core 31
     * is released automatically when all 31 ACPU bits first become set.
     */
    bool core31_released;

    /* Previous SW_RST_CTRL2 value for 0→1 transition detection */
    uint32_t prev_sw_rst_ctrl2;

    /*
     * Previous SW_PLL_BYPASS_CTRL value for transition detection.
     * Initialised to reset value (0xFF = all PLLs bypassed at cold boot).
     */
    uint32_t prev_pll_bypass;

    /* Fallback FDT address (set by QEMU boot setup, used if BOOT_ARG is 0) */
    uint64_t fdt_load_addr;

    /* Target-specific callbacks to set ACPU boot registers */
    BetaSoCCSRSetHartIdFn set_hartid_fn;   /* a0 = hartid */
    BetaSoCCSRSetBootArgFn set_boot_arg_fn; /* a1 = FDT/boot arg */

    /*
     * M-level IMSIC device per ACPU hart.
     * Pre-set eidelivery=1 on release so OpenSBI IPI can wake secondary harts
     * before they have had a chance to enable their own IMSIC via CSR writes.
     */
    DeviceState **acpu_m_imsic;
    uint32_t num_m_imsic;
};

/* Setup function: connect ACPU cores and configure register names */
void beta_soc_csr_setup(BetaSoCCSRState *s,
                        CPUState **acpu_cores,
                        uint32_t num_acpu_cores,
                        uint32_t acpu_hartid_base,
                        const char *core_release_reg,
                        const char *boot_addr_reg,
                        const char *boot_arg_reg,
                        const char *reset_ctrl_reg,
                        BetaSoCCSRSetHartIdFn set_hartid_fn,
                        BetaSoCCSRSetBootArgFn set_boot_arg_fn);

/*
 * Connect M-level IMSIC devices so release_core() can pre-enable eidelivery.
 * devs[i] is the M-level IMSIC for ACPU core i (same order as acpu_cores).
 * Call this after beta_soc_csr_setup() and before sysbus_realize().
 */
void beta_soc_csr_set_m_imsic(BetaSoCCSRState *s,
                               DeviceState **devs,
                               uint32_t count);

#endif /* HW_BETA_SOC_CSR_H */
