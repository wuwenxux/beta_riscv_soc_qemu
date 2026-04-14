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


#include <linux/module.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "pcie-designware.h"
#include "pcie-rv-gen4.h"


#define APPL_RV_AHB_OVRD_REG_00	0x0000
#define APP_CLK_PM_EN	BIT(27)
#define APP_CLK_REQ_N	BIT(26)
#define APP_L1SUB_DISABLE	BIT(25)
#define APP_MARGINING_SOFTWARE_READY	BIT(24)
#define APP_MARGINING_READY	BIT(23)
#define SYS_AUX_PWR_DET	BIT(22)
#define APP_UNLOCK_MSG	BIT(21)
#define CFG_OVRD_AUXCIK_SEL_VALUE	BIT(20)
#define CFG_OVRD_AUXCIK_SEL	BIT(19)
#define APP_DBI_RO_WR_DISABLE_AHB	BIT(17)
#define CFG_AXI_RST_SELECT	BIT(15)
#define APP_LTSSM_ENABLE	BIT(14)
#define APP_HOLD_PHY_RST	BIT(13)
#define APP_REQ_EXIT_L1	BIT(12)
#define APP_READY_ENTR_L23	BIT(11)
#define APP_REQ_ENTR_L1	BIT(10)
#define APP_RAS_DES_SD_HOLD_LTSSM	BIT(9)
#define DIAG_CTRL_BUS	GENMASK(8, 6)
#define APPS_PM_XMT_TURNOFF	BIT(5)
#define APP_INIT_RST	BIT(4)
#define TX_LANE_FLIP_EN	BIT(3)
#define RX_LANE_FLIP_EN	BIT(2)
#define APP_XFER_PENDING	BIT(1)
#define PCIE_INTERRUPT_EN	BIT(0)


#define APPL_RV_AHB_OVRD_REG_01	0x0004
#define PM_DSTATE	GENMASK(31, 29)
#define EDMA_XFER_PENDING	BIT(28)
#define CFG_EXT_TAG_EN	BIT(27)
#define CFG_BUS_MASTER_EN	BIT(26)
#define CFG_MEM_SPACE_EN	BIT(25)
#define RADM_Q_NOT_EMPTY	GENMASK(24, 21)
#define RADM_QOVERFLOW	GENMASK(20, 17)
#define CFG_CRS_SW_VIS_EN	BIT(16)
#define BRDG_SLV_XFER_PENDING	BIT(15)
#define BRDG_DBI_XFER_PENDING	BIT(14)
#define RADM_XFER_PENDING	BIT(13)
#define RDLH_LINK_UP	BIT(12)
#define SMLH_LTSSM_STATE	GENMASK(11, 6)
#define PM_CUMT_STATE	GENMASK(5, 3)
#define SMLH_LTSSM_STATE_RCVRY_EQ	BIT(2)
#define SMLH_LINK_UP	BIT(1)
#define PM_XTIH_BLOCK_TLP	BIT(0)


#define APPL_RV_AHB_OVRD_REG_02	0x0008
#define AHB_OVRD_REG_02	BIT(31) /*W1C*/
#define SMIH_REQ_RST_NOT	BIT(30) /*W1C*/
#define CFG_AER_RC_ERR_INT	BIT(29)
#define RADM_MSG_UNLOCK	BIT(28) /*W1C*/
#define RADM_PM_TURNOFF	BIT(27) /*W1C*/
#define RADM_PM_TO_ACK	BIT(26) /*W1C*/
#define RADM_PM_PME	BIT(25) /*W1C*/
#define RADM_VENDOR_MSG	BIT(24) /*W1C*/
#define CFG_BW_MGT_MSI	BIT(23) /*W1C*/
#define CFG_BW_MGT_INT	BIT(22)
#define CFG_LINK_AUTO_BW_MSI	BIT(21) /*W1C*/
#define CFG_LINK_AUTO_BW_INT	BIT(20)
#define HP_MSI	BIT(19) /*W1C*/
#define HP_INT	BIT(18)
#define HP_PME	BIT(17) /*W1C*/
#define RADM_FATAL_ERR	BIT(16) /*W1C*/
#define RADM_NONFATAL_ERR	BIT(15) /*W1C*/
#define RADM_CORRECTABLE_ERR	BIT(14) /*W1C*/
#define RADM_INTD_DEASSERTED	BIT(13) /*W1C*/
#define RADM_INTC_DEASSERTED	BIT(12) /*W1C*/
#define RADM_INTB_DEASSERTED	BIT(11) /*W1C*/
#define RADM_INTA_DEASSERTED	BIT(10) /*W1C*/
#define RADM_INTD_ASSERTED	BIT(9) /*W1C*/
#define RADM_INTC_ASSERTED	BIT(8) /*W1C*/
#define RADM_INTB_ASSERTED	BIT(7) /*W1C*/
#define RADM_INTA_ASSERTED	BIT(6) /*W1C*/
#define CFG_PME_MSI	BIT(5) /*W1C*/
#define CFG_PME_INT	BIT(4)
#define CFG_SYS_ERR_RC	BIT(3) /*W1C*/
#define CFG_AER_RC_ERR_MSI	BIT(2) /*W1C*/
#define SURPRISE_DOWN_ERR	BIT(1)


#define APPL_RV_AHB_OVRD_REG_03	0x000C


#define APPL_RV_AHB_OVRD_REG_04	0x0010
#define FRSQ_MSI	BIT(27) /*W1C*/
#define CFG_UP_DRS_TO_FRS	BIT(26) /*W1C*/
#define CFG_DRS_MSI	BIT(25) /*W1C*/
#define USP_EQ_REDO_EXECUTED_INT_AHB	BIT(24) /*W1C*/
#define AXI_BRESP_ERROR_CAPTURE	BIT(22) /*W1C*/
#define AXI_RRESP_ERROR_CAPTURE	BIT(21) /*W1C*/
#define RDIH_LINK_UP	BIT(20) /*W1C*/
#define LANEACCEPT_TO_DETECT	BIT(19) /*W1C*/
#define RADM_CPL_TIMEOUT	BIT(18) /*W1C*/
#define TRGT_CPL_TIMEOUT	BIT(17) /*W1C*/
#define CFG_LINK_EQ_REQ_INT	BIT(16)
#define EDMA_INT1	GENMASK(15, 8)
#define EDMA_INT0	GENMASK(7, 0)


#define APPL_RV_AHB_OVRD_REG_05	0x0014

#define RV_GEN4_PCIE_LINKUP	(RDLH_LINK_UP|SMLH_LINK_UP)

#define to_platform_dev(d) \
			container_of(d, struct platform_device, dev)

#define to_rv_gen4_pcie(x) \
			dev_get_drvdata((x)->dev)

#define RV_PCIE_IRQ_HANDLER(d,r,s,w) \
	do { \
		if (r & s) { \
			dev_info(d, "rv gen4 pcie irq state: 0x%08lx\n", s); \
			w |= s; \
		} \
	} while (0)

#define RV_PCIE_INTX_HANDLER(d,r,s,e) \
	do { \
		if (r & s) { \
			dev_info(d, "rv gen4 pcie irq state: 0x%08lx\n", s); \
			rv_gen4_pcie_intx_handler(e, s); \
		} \
	} while (0)

struct rv_gen4_pcie {
    struct dw_pcie *pci;
	struct irq_domain *domain;
    void __iomem *appl_base;
	u32 controller_id;
	int irq;
};

static u32 rv_gen4_pcie_readl(struct rv_gen4_pcie *rv_pcie,
							u32 reg)
{
	return readl(rv_pcie->appl_base + reg);
}

static void rv_gen4_pcie_writel(struct rv_gen4_pcie *rv_pcie,
							u32 val, u32 reg)
{
	writel(val, rv_pcie->appl_base + reg);
}

static void rv_gen4_pcie_intx_handler(struct irq_desc *desc, u32 intx)
{
	struct rv_gen4_pcie *rv_pcie = irq_desc_get_handler_data(desc);
    struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long val = intx;
	unsigned long pos = find_first_bit(&val, BITS_PER_LONG);
	u32 hwirq = pos;

    chained_irq_enter(chip, desc);
	generic_handle_domain_irq(rv_pcie->domain, hwirq);
    chained_irq_exit(chip, desc);
}

static irqreturn_t rv_gen4_pcie_irq_handler(int irq, void *data)
{
	struct rv_gen4_pcie *rv_pcie = (struct rv_gen4_pcie *)data;
	struct device *dev = rv_pcie->pci->dev;
	struct irq_desc *desc = irq_to_desc(irq);
	u32 rdata, wdata = 0;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_02);

	RV_PCIE_IRQ_HANDLER(dev, rdata, AHB_OVRD_REG_02, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, SMIH_REQ_RST_NOT, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_MSG_UNLOCK, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_PM_TURNOFF, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_PM_TO_ACK, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_PM_PME, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_VENDOR_MSG, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, CFG_BW_MGT_MSI, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, CFG_LINK_AUTO_BW_MSI, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, HP_MSI, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, HP_PME, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_FATAL_ERR, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_NONFATAL_ERR, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_CORRECTABLE_ERR, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_INTD_DEASSERTED, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_INTC_DEASSERTED, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_INTB_DEASSERTED, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_INTA_DEASSERTED, wdata);
	RV_PCIE_INTX_HANDLER(dev, rdata, RADM_INTD_ASSERTED, desc);
	RV_PCIE_INTX_HANDLER(dev, rdata, RADM_INTC_ASSERTED, desc);
	RV_PCIE_INTX_HANDLER(dev, rdata, RADM_INTB_ASSERTED, desc);
	RV_PCIE_INTX_HANDLER(dev, rdata, RADM_INTA_ASSERTED, desc);
	RV_PCIE_IRQ_HANDLER(dev, rdata, CFG_PME_MSI, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, CFG_SYS_ERR_RC, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, CFG_AER_RC_ERR_MSI, wdata);

	rv_gen4_pcie_writel(rv_pcie, wdata, APPL_RV_AHB_OVRD_REG_02);

	wdata = 0;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_04);

	RV_PCIE_IRQ_HANDLER(dev, rdata, FRSQ_MSI, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, CFG_UP_DRS_TO_FRS, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, CFG_DRS_MSI, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, USP_EQ_REDO_EXECUTED_INT_AHB, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, AXI_BRESP_ERROR_CAPTURE, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, AXI_RRESP_ERROR_CAPTURE, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RDIH_LINK_UP, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, LANEACCEPT_TO_DETECT, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, RADM_CPL_TIMEOUT, wdata);
	RV_PCIE_IRQ_HANDLER(dev, rdata, TRGT_CPL_TIMEOUT, wdata);

	rv_gen4_pcie_writel(rv_pcie, wdata, APPL_RV_AHB_OVRD_REG_04);

	return IRQ_HANDLED;
}



static void rv_gen4_pcie_intx_ack(struct irq_data *data)
{
	struct rv_gen4_pcie *rv_pcie = irq_data_get_irq_chip_data(data);
	u32 rdata;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_02);
	rdata |= BIT(data->hwirq);
	rv_gen4_pcie_writel(rv_pcie, rdata, APPL_RV_AHB_OVRD_REG_02);
}

static void rv_gen4_pcie_intx_mask(struct irq_data *data)
{
	struct rv_gen4_pcie *rv_pcie = irq_data_get_irq_chip_data(data);
	u32 rdata;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_03);
	rdata |= BIT(data->hwirq);
	rv_gen4_pcie_writel(rv_pcie, rdata, APPL_RV_AHB_OVRD_REG_03);
}

static void rv_gen4_pcie_intx_unmask(struct irq_data *data)
{
	struct rv_gen4_pcie *rv_pcie = irq_data_get_irq_chip_data(data);
	u32 rdata;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_03);
	rdata &= ~(BIT(data->hwirq));
	rv_gen4_pcie_writel(rv_pcie, rdata, APPL_RV_AHB_OVRD_REG_03);
}

static struct irq_chip pcie_intx_irq_chip = {
	.name = "INTx",
	.irq_ack = rv_gen4_pcie_intx_ack,
	.irq_mask = rv_gen4_pcie_intx_mask,
	.irq_unmask = rv_gen4_pcie_intx_unmask,
	.flags = IRQCHIP_SKIP_SET_WAKE
		| IRQCHIP_MASK_ON_SUSPEND,
};

static int rv_gen4_pcie_intx_map(struct irq_domain *domain,
					unsigned int virq, irq_hw_number_t hwirq)
{
	struct device *dev = domain->dev;
	int ret;

    irq_set_chip_and_handler(virq, &pcie_intx_irq_chip,
					handle_level_irq);

    ret = irq_set_chip_data(virq, domain->host_data);
	if (ret < 0) {
		dev_err(dev, "rv gen4 pcie set chip data error, ret: %d\n",
					ret);
		return ret;
	}

    return 0;
}

static const struct irq_domain_ops pcie_intx_domain_ops = {
    .map = rv_gen4_pcie_intx_map,
};

static int rv_gen4_pcie_init_irq_domain(struct rv_gen4_pcie *rv_pcie)
{
    struct device *dev = rv_pcie->pci->dev;
    struct device_node *np;

    np = of_get_child_by_name(dev->of_node, "interrupt-controller");
    if (IS_ERR_OR_NULL(np)) {
		dev_err(dev,
			"rv gen4 pcie get interrupt-controller node error, ret: %ld\n",
						PTR_ERR(np));
		return -EINVAL;
	}

    rv_pcie->domain = irq_domain_add_linear(np, PCI_NUM_INTX,
							&pcie_intx_domain_ops,
							rv_pcie);

    of_node_put(np);

    if (IS_ERR_OR_NULL(rv_pcie->domain)) {
		dev_err(dev,
			"rv gen4 pcie add intx irq domain error, ret: %ld\n",
						PTR_ERR(rv_pcie->domain));
		return -EINVAL;
	}

    return 0;
}

static void rv_gen4_pcie_enable_interrupt(struct rv_gen4_pcie *rv_pcie)
{
	u32 rdata;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_00);
    rdata |= PCIE_INTERRUPT_EN;
    rv_gen4_pcie_writel(rv_pcie, rdata, APPL_RV_AHB_OVRD_REG_00);
}

static void rv_gen4_pcie_disable_interrupt(struct rv_gen4_pcie *rv_pcie)
{
	u32 rdata;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_00);
	rdata &= ~(PCIE_INTERRUPT_EN);
    rv_gen4_pcie_writel(rv_pcie, rdata, APPL_RV_AHB_OVRD_REG_00);
}

static void rv_gen4_pcie_host_init(struct dw_pcie_rp *pp)
{
    struct dw_pcie *pcie = to_dw_pcie_from_pp(pp);
    struct rv_gen4_pcie *rv_pcie = to_rv_gen4_pcie(pcie);
    struct device *dev = rv_pcie->pci->dev;
    int irq, ret;

    irq = of_irq_get_byname(dev->of_node, "legacy");
    if (irq < 0) {
		dev_err(dev, "rv gen4 pcie get legacy irq error, ret: %d\n",
						irq);
        return;
    }
	rv_pcie->irq = irq;



	ret = devm_request_irq(dev, irq, rv_gen4_pcie_irq_handler,
				IRQF_SHARED, dev_name(dev), rv_pcie);
	if (ret < 0) {
		dev_err(dev, "rv gen4 pcie request irq error, ret: %d\n",
					ret);
		return;
	}

    ret = rv_gen4_pcie_init_irq_domain(rv_pcie);
    if (ret < 0) {
        dev_err(dev, "rv gen4 pcie init irq domain error, ret: %d\n",
						ret);
		devm_free_irq(dev, irq, rv_pcie);
		return;
    }



    rv_gen4_pcie_enable_interrupt(rv_pcie);

	dev_info(dev, "rv pcie gen4 host init ok\n");


}

static void rv_gen4_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pcie = to_dw_pcie_from_pp(pp);
    struct rv_gen4_pcie *rv_pcie = to_rv_gen4_pcie(pcie);
    struct device *dev = rv_pcie->pci->dev;

	rv_gen4_pcie_disable_interrupt(rv_pcie);
	irq_domain_remove(rv_pcie->domain);
	devm_free_irq(dev, rv_pcie->irq, rv_pcie);
}



static const struct dw_pcie_host_ops rv_gen4_pcie_host_ops = {
	.post_init = rv_gen4_pcie_host_init,
};

static void rv_gen4_pcie_enable_ltssm(struct rv_gen4_pcie *rv_pcie)
{
	u32 rdata;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_00);
	rdata |= APP_LTSSM_ENABLE;
    rv_gen4_pcie_writel(rv_pcie, rdata, APPL_RV_AHB_OVRD_REG_00);
}

static void rv_gen4_pcie_disable_ltssm(struct rv_gen4_pcie *rv_pcie)
{
	u32 rdata;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_00);
	rdata &= ~(APP_LTSSM_ENABLE);
    rv_gen4_pcie_writel(rv_pcie, rdata, APPL_RV_AHB_OVRD_REG_00);
}

static bool rv_gen4_pcie_link_up(struct dw_pcie *pcie)
{
    struct rv_gen4_pcie *rv_pcie = to_rv_gen4_pcie(pcie);
    u32 rdata;

	rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_01);
    return (rdata & RV_GEN4_PCIE_LINKUP) == RV_GEN4_PCIE_LINKUP;
}

static int rv_gen4_pcie_start_link(struct dw_pcie *pcie)
{
    struct rv_gen4_pcie *rv_pcie = to_rv_gen4_pcie(pcie);

    rv_gen4_pcie_enable_ltssm(rv_pcie);

    return 0;
}

static void rv_gen4_pcie_stop_link(struct dw_pcie *pcie)
{
	struct rv_gen4_pcie *rv_pcie = to_rv_gen4_pcie(pcie);

    rv_gen4_pcie_disable_ltssm(rv_pcie);
}

static const struct dw_pcie_ops rv_gen4_pcie_ops = {
	.link_up = rv_gen4_pcie_link_up,
	.start_link = rv_gen4_pcie_start_link,
	.stop_link = rv_gen4_pcie_stop_link,
};

static int rv_gen4_pcie_get_resource(struct rv_gen4_pcie *rv_pcie)
{
	struct device *dev = rv_pcie->pci->dev;
	struct device_node *np = dev->of_node;
	struct platform_device *pdev = to_platform_dev(dev);
	struct resource *res;
	u32 controller_id;
	int ret;

	ret = of_property_read_u32(np, "controller-id", &controller_id);
	if (ret || controller_id > RV_GEN4_PCIE_X4_C) {
		dev_err(dev,
			"rv gen4 pcie get controller-id property error or invalid, ret: %d\n",
					ret);
		return ret;
	}
	rv_pcie->controller_id = controller_id;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "appl");
	if (IS_ERR_OR_NULL(res)) {
		dev_err(dev,
			"rv gen4 pcie get appl rg-region error, ret: %ld\n",
						PTR_ERR(res));
		return -EINVAL;
	}

	rv_pcie->appl_base = devm_ioremap_resource(dev, res);
	if (IS_ERR_OR_NULL(rv_pcie->appl_base)) {
		dev_err(dev,
			"rv gen4 pcie appl ioremap rg-region error, ret: %ld\n",
					PTR_ERR(rv_pcie->appl_base));
		return -EIO;
	}
	
	return 0;
}

static void rv_gen4_pcie_put_resource(struct rv_gen4_pcie *rv_pcie)
{
	struct device *dev = rv_pcie->pci->dev;

	devm_iounmap(dev, rv_pcie->appl_base);
}

static void rv_gen4_pcie_deassert_phy_rst(struct rv_gen4_pcie *rv_pcie)
{
	u32 rdata;

    rdata = rv_gen4_pcie_readl(rv_pcie, APPL_RV_AHB_OVRD_REG_00);
    rdata &= ~(APP_HOLD_PHY_RST);
    rv_gen4_pcie_writel(rv_pcie, rdata, APPL_RV_AHB_OVRD_REG_00);
}

static int rv_gen4_add_pcie_port(struct rv_gen4_pcie *rv_pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = rv_pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	pp->irq = platform_get_irq(pdev, 0);
	if (pp->irq < 0)
		return pp->irq;

	pp->num_vectors = MSI_DEF_NUM_VECTORS;
	pp->ops = &rv_gen4_pcie_host_ops;
	pp->native_ecam = true;  /* Use iATU for config access, not ECAM */

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "rv gen4 PCIe host init error, ret: %d\n",
						ret);
		return ret;
	}

	return 0;
}

static void rv_gen4_del_pcie_port(struct rv_gen4_pcie *rv_pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = rv_pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;

	dw_pcie_host_deinit(pp);
}

static int rv_gen4_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rv_gen4_pcie *rv_pcie;
	struct dw_pcie *pci;
	int ret;
	
	rv_pcie = devm_kzalloc(dev, sizeof(*rv_pcie), GFP_KERNEL);
	if (IS_ERR_OR_NULL(rv_pcie))
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (IS_ERR_OR_NULL(pci)) {
		ret = -ENOMEM;
		goto err_alloc_dw_pcie;
	}

	pci->dev = dev;
	pci->ops = &rv_gen4_pcie_ops;

	rv_pcie->pci = pci;

	platform_set_drvdata(pdev, rv_pcie);

	ret = rv_gen4_pcie_get_resource(rv_pcie);
	if (ret) {
		dev_err(dev, "rv get gen4 pcie resource error, ret: %d\n",
						ret);
		goto err_get_resource;
	}

	rv_pcie_mgmt_deassert_reset(dev->parent, rv_pcie->controller_id);
	rv_gen4_pcie_deassert_phy_rst(rv_pcie);

	ret = rv_gen4_add_pcie_port(rv_pcie, pdev);
	if (ret) {
		dev_err(dev, "rv add gen4 PCIe host port error, ret: %d\n",
						ret);
		goto err_add_pcie_port;
	}

	dev_info(dev, "rv gen4 pcie host controller init ok\n");

	return 0;

err_add_pcie_port:
err_get_resource:
	devm_kfree(dev, pci);
err_alloc_dw_pcie:
	devm_kfree(dev, rv_pcie);

	return ret;
}


static void rv_gen4_pcie_remove(struct platform_device *pdev)
{
	struct rv_gen4_pcie *rv_pcie;
	struct device *dev = &pdev->dev;

	rv_pcie = platform_get_drvdata(pdev);

	rv_gen4_del_pcie_port(rv_pcie, pdev);
	rv_gen4_pcie_put_resource(rv_pcie);

	devm_kfree(dev, rv_pcie->pci);
	devm_kfree(dev, rv_pcie);
}


static const struct of_device_id rv_gen4_pcie_of_match[] = {
	{ .compatible = "rivai,rv-gen4-pcie", },
	{},
};

static struct platform_driver rv_gen4_pcie_driver = {
	.driver = {
		.name	= "rv-gen4-pcie",
		.of_match_table = rv_gen4_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = rv_gen4_pcie_probe,
	.remove = rv_gen4_pcie_remove,
};
builtin_platform_driver(rv_gen4_pcie_driver);

MODULE_DEVICE_TABLE(of, rv_gen4_pcie_of_match);
MODULE_AUTHOR("Hao Chen <hao.chen@rivai.ai>");
MODULE_DESCRIPTION("SNPS PCIe RC Host driver for RiVAI SoCs");
MODULE_LICENSE("GPL v2");

