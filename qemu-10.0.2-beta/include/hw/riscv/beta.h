/*
 * QEMU RISC-V Beta SoC machine interface
 *
 * Copyright (c) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_RISCV_BETA_H
#define HW_RISCV_BETA_H

#include "hw/boards.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/block/flash.h"
/* BetaSoCConfig is defined in hw/riscv/beta_config.h (local header) */
typedef struct BetaSoCCSRState BetaSoCCSRState;
typedef struct BetaSoCConfig BetaSoCConfig;

#define TYPE_BETA_MACHINE MACHINE_TYPE_NAME("beta")
typedef struct BetaMachineState BetaMachineState;
DECLARE_INSTANCE_CHECKER(BetaMachineState, BETA_MACHINE,
                         TYPE_BETA_MACHINE)

struct BetaMachineState {
    /*< private >*/
    MachineState parent;

    /*< public >*/
    BetaSoCConfig *config;

    /* MCPU: RV32 management processor */
    RISCVHartArrayState mcpu_soc;

    /* ACPU: RV64 application processors */
    RISCVHartArrayState acpu_soc;

    /* ACPU interrupt controllers */
    DeviceState *aplic_m;
    DeviceState *aplic_s;

    /* Root APLIC (has GPIO inputs for external interrupt sources) */
    DeviceState *aplic_root;

    /* ACPU timer */
    DeviceState *clint;

    /* NOR flash */
    PFlashCFI01 *flash;

    /* SOC CSR device (for passing FDT address to core release callback) */
    BetaSoCCSRState *soc_csr;

    /* ACPU FDT load address in DDR (set during beta_boot_setup) */
    hwaddr fdt_load_addr;

    /* MCPU FDT (separate device tree for management processor) */
    void *mcpu_fdt;
    hwaddr mcpu_fdt_addr;

    /* MCPU AHB master window for 40-bit address translation */
    /* MCPU subsystem devices */
    void *mcpu_plic;  /* BetaMcpuPLICState */
    qemu_irq *mcpu_plic_irqs;  /* MCPU PLIC input IRQ lines (64 sources) */
    void *mcpu_ahb;   /* BetaMcpuAHBState */

    /* M-level IMSIC devices per ACPU hart (for SMP bringup pre-enable) */
    DeviceState **acpu_m_imsic;
    uint32_t num_acpu_m_imsic;

    /* Configuration file path */
    char *cfg_file;

    /* Memory model control: true = TSO (Ztso), false = RVWMO */
    bool ztso;

    /*
     * PCIe host bridge devices, indexed [subsystem][controller].
     * Populated by beta_create_pcie(); used by beta_create_iommu() to
     * wire each RISC-V IOMMU to the correct set of PCIe buses.
     * Up to 4 PCIe subsystems, each with up to 4 DWC controllers (4x4 mode).
     */
    DeviceState *pcie_host_devs[4][4];
    int          pcie_num_ctrl[4];
};

/* FDT generation (implemented in beta_dtb.c) */
void beta_create_fdt(BetaMachineState *s);
void beta_create_mcpu_fdt(BetaMachineState *s);

#endif
