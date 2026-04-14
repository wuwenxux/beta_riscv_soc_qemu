// SPDX-License-Identifier: GPL-2.0+
/*
 * RiVAI AXI PCIe Sub-System driver
 *
 * Copyright (c) 2024 RiVAI, Inc.
 *
 * Author: Hao Chen <hao.chen@rivai.ai>
 *
 * Bits taken from RiVAI PCIe Sub-System driver and
 * RISC-V PCI Sub-System generic driver.
 */

#include <linux/clk.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "pcie-rv-gen4.h"

#define MGMT_PCIE_CLK_RSTN_CTL	0x0000

#define P16_DBI_ACLK_EN	BIT(0)
#define P16_DBI_ACLK_RSTN	BIT(1)
#define PA_DBI_ACLK_EN	BIT(2)
#define PA_DBI_ACLK_RSTN	BIT(3)
#define PB_DBI_ACLK_EN	BIT(4)
#define PB_DBI_ACLK_RSTN	BIT(5)
#define PC_DBI_ACLK_EN	BIT(6)
#define PC_DBI_ACLK_RSTN	BIT(7)
#define P16_MST_ACLK_EN	BIT(8)
#define P16_MST_ACLK_RSTN	BIT(9)
#define PA_MST_ACLK_EN	BIT(10)
#define PA_MST_ACLK_RSTN	BIT(11)
#define PB_MST_ACLK_EN	BIT(12)
#define PB_MST_ACLK_RSTN	BIT(13)
#define PC_MST_ACLK_EN	BIT(14)
#define PC_MST_ACLK_RSTN	BIT(15)
#define P16_SLV_ACLK_EN	BIT(16)
#define P16_SLV_ACLK_RSTN	BIT(17)
#define PA_SLV_ACLK_EN	BIT(18)
#define PA_SLV_ACLK_RSTN	BIT(19)
#define PB_SLV_ACLK_EN	BIT(20)
#define PB_SLV_ACLK_RSTN	BIT(21)
#define PC_SLV_ACLK_EN	BIT(22)
#define PC_SLV_ACLK_RSTN	BIT(23)

#define MGMT_PCIE_OVRD_CFG_REG_01	0x0004

#define PHY_SRAM_BYPASS	GENMASK(3, 0)
#define PHY_SW_SRAM_ID_DONE	GENMASK(7, 4)
#define PHY_2ND_PERST_SRAM_BYPASS	GENMASK(11, 8)
#define PORT_X16_APP_SRIS_MODE	BIT(12)
#define PORT_X4_APP_SRIS_MODE	GENMASK(15, 13)
#define PORT_X16_BUTTON_RST_N	BIT(16)
#define PORT_X4_BUTTON_RST_N	GENMASK(19, 17)
#define PHY_SRAM_INIT_DONE_SYNC	GENMASK(23, 20)
#define HW_ADAPT_BF_EN	BIT(30)
#define HIGH12LANES_ALL_IDLE_SYNC	BIT(31)

#define MGMT_PCIE_OVRD_PHY_MATRIX_REG	0x0008

#define OVRD_PHY_SRC_SEL	GENMASK(7, 0)
#define OVRD_PHY_SRC_SEL_EN	BIT(8)
#define OVRD_PIPE_PHYSTATUS	GENMASK(24, 9)
#define OVRD_PIPE_PHYSTATUS_EN	BIT(25)
#define BIFURCATION_SEL_OVRD_VAL	BIT(26)
#define BIFURCATION_SEL_OVRD_EN	BIT(27)
#define CFG_PHY_CONTROL_OVRD_EN	BIT(28)

#define MGMT_PCIE_CFG_PHY_CONTROL_OVRD_VAL	0x000C
#define PHY_CONTROL_VAL	BIT(2)

#define APPL_PCIE_GPIO_POWER_CONTROL_REG	0x0010
#define PWR3P3_ENABLE	GENMASK(3, 0)
#define AUXPWR3P3_ENABLE	GENMASK(7, 4)
#define PWR12_ENABLE	GENMASK(11, 8)

#define MGMT_PCIE_GPIO_CONFIG_REG	0x0014

#define PORT_X16_RC_WARM_RESETN	BIT(0)
#define PORT_X4A_RC_WARM_RESETN	BIT(1)
#define PORT_X4B_RC_WARM_RESETN	BIT(2)
#define PORT_X4C_RC_WARM_RESETN	BIT(3)
#define PORT_X4_RC_WARM_RESETN	GENMASK(3, 1)
#define PORT_X16_CLKREQN_OE_OVRD	BIT(4)
#define PORT_X16_CLKREQN_OE_VAL	BIT(5)
#define PORT_X16_WAKEN_OE_OVRD	BIT(6)
#define PORT_X16_WAKEN_OE_VAL	BIT(7)
#define PORT_PA_CLKREQN_OE_OVRD	BIT(8)
#define PORT_PA_CLKREQN_OE_VAL	BIT(9)
#define PORT_PA_WAKEN_OE_OVRD	BIT(10)
#define PORT_PA_WAKEN_OE_VAL	BIT(11)
#define PORT_PB_CLKREQN_OE_OVRD	BIT(12)
#define PORT_PB_CLKREQN_OE_VAL	BIT(13)
#define PORT_PB_WAKEN_OE_OVRD	BIT(14)
#define PORT_PB_WAKEN_OE_VAL	BIT(15)
#define PORT_PC_CLKREQN_OE_OVRD	BIT(16)
#define PORT_PC_CLKREQN_OE_VAL	BIT(17)
#define PORT_PC_WAKEN_OE_OVRD	BIT(18)
#define PORT_PC_WAKEN_OE_VAL	BIT(19)
#define PORT_X16_PERSTN_OUT_ENABLE	BIT(20)
#define PORT_X4_PERSTN_OUT_ENABLE	GENMASK(23, 21)
#define PORT_X16_PERSTN_OUT_VAL	BIT(24)
#define PORT_X4A_PERSTN_OUT_VAL	BIT(25)
#define PORT_X4B_PERSTN_OUT_VAL	BIT(26)
#define PORT_X4C_PERSTN_OUT_VAL	BIT(27)
#define PORT_X4_PERSTN_OUT_VAL	GENMASK(27, 25)

#define MGMT_PCIE_TOP_MEM_EMA_CFG_LSB	0x0018
#define MGMT_PCIE_TOP_MEM_EMA_CFG_MSB	0x001C

#define MGMT_PCIE_PHY_PERST_N_OVRD	0x0020

#define X16_PHY_PERST_N_OVRD_EN	BIT(0)
#define X16_PHY_PERST_N_OVRD_VAL	BIT(1)
#define PORTA_PHY_PERST_N_OVRD_EN	BIT(2)
#define PORTA_PHY_PERST_N_OVRD_VAL	BIT(3)
#define PORTB_PHY_PERST_N_OVRD_EN	BIT(4)
#define PORTB_PHY_PERST_N_OVRD_VAL	BIT(5)
#define PORTC_PHY_PERST_N_OVRD_EN	BIT(6)
#define PORTC_PHY_PERST_N_OVRD_VAL	BIT(7)

#define SOC_TOP_CSR_BASE	0x0100000000
#define SOC_TOP_CSR_SIZE	SZ_4K
#define SOC_PCIE_MODE_CTRL	0xB04
#define PCIE3_MODE_CTRL_4X4	BIT(7)
#define PCIE2_MODE_CTRL_4X4	BIT(6)
#define PCIE1_MODE_CTRL_4X4	BIT(5)
#define PCIE0_MODE_CTRL_4X4	BIT(4)


#define to_platform_dev(d) \
			container_of(d, struct platform_device, dev)

#define to_rv_mgmt_dev(x) \
			dev_get_drvdata((x))

#define ACLK_RSTN(r,m,l) \
			(r|m|l)
#define ACLK_EN(r,m,l) \
			(r|m|l)
#define BIF_EN(r,m,l) \
			(r|m|l)
#define WARM_RSTN(r,l) \
			(r|l)
#define PERST_N(r,l) \
			(r|l)

enum rv_pcie_mgmt_bif {
	PCIE_MGMT_BIF_DISABLE = 0,
	PCIE_MGMT_BIF_ENABLE,
};

struct rv_pcie_mgmt_dev {
	struct device *dev;
	void __iomem *mgmt_base;
	void __iomem *csr_base;
	u32 bifurcation;
	u32 system_id;
};

static u32 rv_pcie_mgmt_readl(struct rv_pcie_mgmt_dev *mdev,
							u32 reg)
{
    return readl(mdev->mgmt_base + reg);
}

static void rv_pcie_mgmt_writel(struct rv_pcie_mgmt_dev *mdev,
							u32 val, u32 reg)
{
     writel(val, mdev->mgmt_base + reg);
}

static int rv_pcie_mgmt_get_resource(struct rv_pcie_mgmt_dev *mdev)
{
	struct device *dev = mdev->dev;
	struct platform_device *pdev = to_platform_dev(dev);
	struct device_node *np = dev->of_node;
	struct resource *res;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mgmt");
	if (IS_ERR_OR_NULL(res)) {
		dev_err(dev, "rv pcie mgmt get mgmt rg-region error, ret: %ld\n",
						PTR_ERR(res));
		return -EINVAL;
	}

	mdev->mgmt_base = devm_ioremap_resource(dev, res);
	if (IS_ERR_OR_NULL(mdev->mgmt_base)) {
		dev_err(dev, "rv pcie mgmt ioremap rg-region error, ret: %ld\n",
					PTR_ERR(mdev->mgmt_base));
		return -EIO;
	}

	mdev->csr_base = devm_ioremap(dev, SOC_TOP_CSR_BASE, SOC_TOP_CSR_SIZE);
	if (IS_ERR_OR_NULL(mdev->csr_base)) {
		dev_err(dev, "rv pcie mgmt ioremap csr-region error, ret: %ld\n",
					PTR_ERR(mdev->csr_base));
		return -EIO;
	}

	ret = of_property_read_u32(np, "pcie-bifurcation",
					&mdev->bifurcation);
	if (ret)
		mdev->bifurcation = PCIE_MGMT_BIF_DISABLE;

	ret = of_property_read_u32(np, "system-id",
					&mdev->system_id);
	if (ret)
		mdev->system_id = RV_GEN4_PCIE_SYS_0;

	return 0;
}

static void rv_pcie_mgmt_put_resource(struct rv_pcie_mgmt_dev *mdev)
{
	struct device *dev = mdev->dev;

	devm_iounmap(dev, mdev->mgmt_base);
	devm_iounmap(dev, mdev->csr_base);
}

static void rv_pcie_mgmt_axi_clk_init(struct rv_pcie_mgmt_dev *mdev)
{
	u32 bif = mdev->bifurcation;
	u32 rdata;

	rdata = rv_pcie_mgmt_readl(mdev, MGMT_PCIE_CLK_RSTN_CTL);

	rdata |= ACLK_RSTN(P16_DBI_ACLK_RSTN,
				P16_MST_ACLK_RSTN,
				P16_SLV_ACLK_RSTN);
	rdata |= ACLK_EN(P16_DBI_ACLK_EN,
				P16_MST_ACLK_EN,
				P16_SLV_ACLK_EN);

	if (bif) {
		rdata |= ACLK_RSTN(PA_DBI_ACLK_RSTN,
					PA_MST_ACLK_RSTN,
					PA_SLV_ACLK_RSTN);
		rdata |= ACLK_EN(PA_DBI_ACLK_EN,
					PA_MST_ACLK_EN,
					PA_SLV_ACLK_EN);

		rdata |= ACLK_RSTN(PB_DBI_ACLK_RSTN,
					PB_MST_ACLK_RSTN,
					PB_SLV_ACLK_RSTN);
		rdata |= ACLK_EN(PB_DBI_ACLK_EN,
					PB_MST_ACLK_EN,
					PB_SLV_ACLK_EN);

		rdata |= ACLK_RSTN(PC_DBI_ACLK_RSTN,
					PC_MST_ACLK_RSTN,
					PC_SLV_ACLK_RSTN);
		rdata |= ACLK_EN(PC_DBI_ACLK_EN,
					PC_MST_ACLK_EN,
					PC_SLV_ACLK_EN);
	}

	rv_pcie_mgmt_writel(mdev, rdata, MGMT_PCIE_CLK_RSTN_CTL);
}

static void rv_pcie_mgmt_bif_init(struct rv_pcie_mgmt_dev *mdev)
{
	u32 bif = mdev->bifurcation;
	u32 rdata;

	if (bif) {
		rdata = rv_pcie_mgmt_readl(mdev, MGMT_PCIE_OVRD_PHY_MATRIX_REG);
		
		rdata |= BIF_EN(CFG_PHY_CONTROL_OVRD_EN,
					BIFURCATION_SEL_OVRD_EN,
					BIFURCATION_SEL_OVRD_VAL);

		rv_pcie_mgmt_writel(mdev, rdata, MGMT_PCIE_OVRD_PHY_MATRIX_REG);

		rdata = rv_pcie_mgmt_readl(mdev, MGMT_PCIE_CFG_PHY_CONTROL_OVRD_VAL);
		rdata |= PHY_CONTROL_VAL;
		rv_pcie_mgmt_writel(mdev, rdata, MGMT_PCIE_CFG_PHY_CONTROL_OVRD_VAL);
	}
}

static void rv_pcie_mgmt_mode_ctrl_init(struct rv_pcie_mgmt_dev *mdev)
{
	struct device *dev = mdev->dev;
	u32 bif = mdev->bifurcation;
	u32 sys_id = mdev->system_id;
	void __iomem *csr_mode_ctrl_addr = mdev->csr_base + SOC_PCIE_MODE_CTRL;
	u32 rdata;

	if (bif) {
		rdata = readl(csr_mode_ctrl_addr);
		dev_info(dev, "rv pcie mgmt: %u, mode ctrl rdata: 0x%x\n", sys_id, rdata);

		switch (sys_id) {
		case RV_GEN4_PCIE_SYS_0:
			rdata |= PCIE0_MODE_CTRL_4X4;
			break;
		case RV_GEN4_PCIE_SYS_1:
			rdata |= PCIE1_MODE_CTRL_4X4;
			break;
		case RV_GEN4_PCIE_SYS_2:
			rdata |= PCIE2_MODE_CTRL_4X4;
			break;
		case RV_GEN4_PCIE_SYS_3:
			rdata |= PCIE3_MODE_CTRL_4X4;
			break;
		default:
			dev_warn(dev, "rv pcie mgmt unknown system id: %u\n",
							sys_id);
			break;
		}

		dev_info(dev, "rv pcie mgmt: %u, mode ctrl wdata: 0x%x\n", sys_id, rdata);
		writel(rdata, csr_mode_ctrl_addr);
	}
}

static void rv_pcie_mgmt_phy_sram_init(struct rv_pcie_mgmt_dev *mdev)
{
	u32 rdata;

    rdata = rv_pcie_mgmt_readl(mdev, MGMT_PCIE_OVRD_CFG_REG_01);
    rdata |= PHY_SRAM_BYPASS;
    rv_pcie_mgmt_writel(mdev, rdata, MGMT_PCIE_OVRD_CFG_REG_01);
}

void rv_pcie_mgmt_deassert_reset(struct device *dev, u32 id)
{
	struct rv_pcie_mgmt_dev *mdev = to_rv_mgmt_dev(dev);
	u32 rdata;

	dev_info(mdev->dev, "controller-id: %u\n", id);
	if (mdev->bifurcation) {
		rdata = rv_pcie_mgmt_readl(mdev, MGMT_PCIE_GPIO_CONFIG_REG);
		dev_info(mdev->dev, "mgmt_pcie_gpio_config_reg(x16-rd): 0x%x\n",
					rdata);
		rdata |= PORT_X16_RC_WARM_RESETN;
		rdata |= PORT_X16_PERSTN_OUT_VAL;
		dev_info(mdev->dev, "mgmt_pcie_gpio_config_reg(x16-wr): 0x%x\n",
					rdata);
		rv_pcie_mgmt_writel(mdev, rdata, MGMT_PCIE_GPIO_CONFIG_REG);

		rdata = rv_pcie_mgmt_readl(mdev, MGMT_PCIE_GPIO_CONFIG_REG);
		dev_info(mdev->dev, "mgmt_pcie_gpio_config_reg(x4-rd): 0x%x\n",
					rdata);
		switch (id) {
		case RV_GEN4_PCIE_X4_A:
			rdata |= PORT_X4A_RC_WARM_RESETN;
			rdata |= PORT_X4A_PERSTN_OUT_VAL;
			break;
		case RV_GEN4_PCIE_X4_B:
			rdata |= PORT_X4B_RC_WARM_RESETN;
			rdata |= PORT_X4B_PERSTN_OUT_VAL;
			break;
		case RV_GEN4_PCIE_X4_C:
			rdata |= PORT_X4C_RC_WARM_RESETN;
			rdata |= PORT_X4C_PERSTN_OUT_VAL;
			break;
		default:
			dev_warn(dev, "unknown controller id: %u\n",
						id);
			break;
		}
		dev_info(mdev->dev, "mgmt_pcie_gpio_config_reg(x4-wr): 0x%x\n",
					rdata);
		rv_pcie_mgmt_writel(mdev, rdata, MGMT_PCIE_GPIO_CONFIG_REG);
	} else {
		rdata = rv_pcie_mgmt_readl(mdev, MGMT_PCIE_GPIO_CONFIG_REG);
		dev_info(mdev->dev, "mgmt_pcie_gpio_config_reg(x16-rd): 0x%x\n",
					rdata);
		rdata |= PORT_X16_RC_WARM_RESETN;
		rdata |= PORT_X16_PERSTN_OUT_VAL;
		dev_info(mdev->dev, "mgmt_pcie_gpio_config_reg(x16-wr): 0x%x\n",
					rdata);
		rv_pcie_mgmt_writel(mdev, rdata, MGMT_PCIE_GPIO_CONFIG_REG);
	}
}
EXPORT_SYMBOL(rv_pcie_mgmt_deassert_reset);

static void rv_pcie_mgmt_init(struct rv_pcie_mgmt_dev *mdev)
{
	rv_pcie_mgmt_axi_clk_init(mdev);
	rv_pcie_mgmt_mode_ctrl_init(mdev);
	rv_pcie_mgmt_bif_init(mdev);
	rv_pcie_mgmt_phy_sram_init(mdev);
}

static int rv_pcie_mgmt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rv_pcie_mgmt_dev *mgmt_dev;
	int ret;

	mgmt_dev = devm_kzalloc(dev, sizeof(*mgmt_dev), GFP_KERNEL);
	if (IS_ERR_OR_NULL(mgmt_dev))
		return -ENOMEM;

	mgmt_dev->dev = dev;

	ret = rv_pcie_mgmt_get_resource(mgmt_dev);
	if (ret) {
		dev_err(dev,
			"rv pcie mgmt get dts resource error, ret: %d\n",
					ret);
		goto err_get_dts_resource;
	}

	rv_pcie_mgmt_init(mgmt_dev);
	platform_set_drvdata(pdev, mgmt_dev);

	ret = devm_of_platform_populate(dev);
	if (ret) {
		dev_err(dev,
			"rv pcie mgmt populate pcie rc controller device error, ret: %d\n",
					ret);
		return -EINVAL;
	}

	dev_info(dev, "rv pcie mgmt init ok.\n");

	return 0;

err_get_dts_resource:
	devm_kfree(dev, mgmt_dev);

	return ret;
}


static int rv_pcie_mgmt_remove(struct platform_device *pdev)
{
	struct rv_pcie_mgmt_dev *mgmt_dev;
	struct device *dev = &pdev->dev;

	mgmt_dev = platform_get_drvdata(pdev);

	devm_of_platform_depopulate(dev);
	rv_pcie_mgmt_put_resource(mgmt_dev);

	devm_kfree(dev, mgmt_dev);

	return 0;
}

static const struct of_device_id rv_pcie_mgmt_of_match[] = {
	{ .compatible = "rivai,rv-mgmt-pcie", },
	{},
};

static struct platform_driver rv_pcie_mgmt_driver = {
	.driver = {
		.name	= "rv-pcie-mgmt",
		.of_match_table = rv_pcie_mgmt_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = rv_pcie_mgmt_probe,
	.remove = rv_pcie_mgmt_remove,
};

builtin_platform_driver(rv_pcie_mgmt_driver);

MODULE_DEVICE_TABLE(of, rv_pcie_mgmt_of_match);
MODULE_AUTHOR("Hao Chen <hao.chen@rivai.ai>");
MODULE_DESCRIPTION("ASR PCIe MGMT driver for RiVAI SoCs");
MODULE_LICENSE("GPL v2");

