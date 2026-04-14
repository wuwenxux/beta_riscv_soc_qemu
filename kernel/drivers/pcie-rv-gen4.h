// SPDX-License-Identifier: GPL-2.0+
/*
 * RiVAI AXI PCIe host controller driver
 *
 * Copyright (c) 2024 RiVAI, Inc.
 *
 * Author: Hao Chen <hao.chen@rivai.ai>
 *
 * Bits taken from Synopsys DesignWare Host controller driver and
 * RISC-V PCI Host generic driver.
 */

enum rv_gen4_system_id {
	RV_GEN4_PCIE_SYS_0 = 0,
	RV_GEN4_PCIE_SYS_1,
	RV_GEN4_PCIE_SYS_2,
	RV_GEN4_PCIE_SYS_3,
};

enum rv_gen4_controller_id {
	RV_GEN4_PCIE_X16 = 0,
	RV_GEN4_PCIE_X4_A,
	RV_GEN4_PCIE_X4_B,
	RV_GEN4_PCIE_X4_C,
};

void rv_pcie_mgmt_deassert_reset(struct device *dev, u32 id);
