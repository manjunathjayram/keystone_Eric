/*
 * Copyright 2013 Texas Instruments, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <asm/irq.h>

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/signal.h>
#include <linux/pci.h>

#include "pci-pdata.h"

#define DRIVER_NAME	"keystone-pcie"
#define PCIE_REGS_INDEX			0
#define	PCIE_NON_PREFETCH_INDEX		1
#define PCIE_IO_INDEX			2
#define PCIE_INBOUND0_INDEX		3

/*
 *  Application Register Offsets
 */
#define PCISTATSET			0x010
#define CMD_STATUS			0x004
#define CFG_SETUP			0x008
#define IOBASE				0x00c
#define OB_SIZE				0x030
#define IRQ_EOI                         0x050
#define MSI_IRQ				0x054
/* 32 Registers */
#define OB_OFFSET_INDEX(n)		(0x200 + (8 * n))
/* 32 Registers */
#define OB_OFFSET_HI(n)			(0x204 + (8 * n))
#define IB_BAR0				0x300
#define IB_START0_LO			0x304
#define IB_START0_HI			0x308
#define IB_OFFSET0			0x30c
#define ERR_IRQ_STATUS_RAW		0x1c0
#define ERR_IRQ_STATUS			0x1c4
#define ERR_IRQ_ENABLE_SET		0x1c8
#define MSI0_IRQ_STATUS			0x104
#define MSI0_IRQ_ENABLE_SET		0x108
#define MSI0_IRQ_ENABLE_CLR		0x10c
#define IRQ_STATUS			0x184
#define IRQ_ENABLE_SET			0x188
#define IRQ_ENABLE_CLR			0x18c

#define MSI_IRQ_OFFSET			4

/*
 * PCIe Config Register Offsets (capabilities)
 */
#define LINK_CAP		        0x07c
/*
 * PCIe Config Register Offsets (misc)
 */
#define DEBUG0			        0x728
#define PL_GEN2			        0x80c

/* Various regions in PCIESS address space */
#define SPACE0_LOCAL_CFG_OFFSET		0x1000
#define SPACE0_REMOTE_CFG_OFFSET	0x2000
#define SPACE0_IO_OFFSET		0x3000

/* Application command register values */
#define DBI_CS2_EN_VAL		        BIT(5)
#define IB_XLAT_EN_VAL		        BIT(2)
#define OB_XLAT_EN_VAL		        BIT(1)
#define LTSSM_EN_VAL		        BIT(0)

/* Link training encodings as indicated in DEBUG0 register */
#define LTSSM_STATE_MASK	        0x1f
#define LTSSM_STATE_L0		        0x11

/* Directed Speed Change */
#define DIR_SPD				(1 << 17)

/* Outbound window size specified as power of 2 MB */
#define CFG_PCIM_WIN_SZ_IDX	        3
#define CFG_PCIM_WIN_CNT	        32

/* max 1GB DMA range */
#define MAX_DMA_RANGE			0x80000000
#define MAX_LEGACY_HOST_IRQS		4
#define MAX_MSI_IRQS			32
#define MAX_MSI_HOST_IRQS		8

/* error IRQ bits. Enable following error IRQs
 *  - ERR_AER  - ECRC error - BIT(5)
 *  - ERR_AXI  - AXI tag lookup fatal error BIT(4)
 *  - ERR_CORR - Correctable error BIT(3)
 *  - ERR_NONFATAL - Non fatal error BIT(2)
 *  - ERR_FATAL - Fatal error BIT(1)
 */
#define ERR_IRQ_ALL		(BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5))
#define ERR_FATAL_IRQ			BIT(1)

static struct keystone_pcie_pdata keystone2_data = {
	.setup = k2_pcie_platform_setup,
};

struct keystone_pcie_info {
	void __iomem *reg_cfg_virt;
	/* PCIE resources */
	int num_mem_resources;
	struct resource cfg_resource;
	struct resource	mem_resource;
	struct resource	io_resource;
	struct resource inbound_dma_res;
	/* legacy Host irqs */
	int num_legacy_irqs;
	int legacy_irqs[MAX_LEGACY_HOST_IRQS];
	struct irq_domain *legacy_irqd;
	int virqs[MAX_LEGACY_HOST_IRQS];
	/* MSI IRQs */
	int num_msi_host_irqs;
	int msi_host_irqs[MAX_MSI_HOST_IRQS];
	int num_msi_irqs;
	int msi_virq_base;
	struct irq_domain *msi_irqd;
	int error_irq;
	/* platform customization */
	struct keystone_pcie_pdata *pdata;
};

/* abort reg reference */
static u32 abort_check_base;

#ifdef XFER_TEST
#define TX_RX_BUF_SZ		40
#define IB_BUFFER_DEFAULT_SIZE		SZ_32K
#define IB_BUFFER_ALIGN_SIZE		0x100
#define IB_BUFFER_ALIGN_MASK		(IB_BUFFER_ALIGN_SIZE - 1)

u32 *ib_buffer;

void keystone_pcie_send_data(void)
{
	int i;
	u32 src_buf[TX_RX_BUF_SZ];
	void __iomem *pcie_data_regs;

	for (i = 0; i < TX_RX_BUF_SZ; i++)
		src_buf[i] = i;

	pcie_data_regs = ioremap(0x50000000, 0x1000);

	pr_info(DRIVER_NAME ": RC sending data (%08x) to EP ...",
		(u32)pcie_data_regs);
	for (i = 0; i < TX_RX_BUF_SZ; i++)
		__raw_writel(src_buf[i], pcie_data_regs + i*4);

	/* Signal end of transmit to EP */
	__raw_writel(1, pcie_data_regs + TX_RX_BUF_SZ*4);
	pr_info(DRIVER_NAME ": RC send data to EP done.\n");
}
EXPORT_SYMBOL(keystone_pcie_send_data);

void keystone_pcie_send_receive_data(void)
{
	int i;
	u32 src_buf[TX_RX_BUF_SZ];
	void __iomem *pcie_data_regs;

	for (i = 0; i < TX_RX_BUF_SZ; i++)
		src_buf[i] = i;

	pcie_data_regs = ioremap(0x50000000, 0x1000);

	pr_info(DRIVER_NAME ": RC sending data to EP ...\n");
	for (i = 0; i < TX_RX_BUF_SZ; i++)
		__raw_writel(src_buf[i], pcie_data_regs + i*4);

	/* Signal end of transmit to EP */
	__raw_writel(1, pcie_data_regs + TX_RX_BUF_SZ*4);
	pr_info(DRIVER_NAME ": RC send data to EP done.\n");

	pr_info(DRIVER_NAME ": RC receiving data @ %08x - %08x from EP ...\n",
		(u32)ib_buffer, (u32)&(ib_buffer[TX_RX_BUF_SZ]));

	msleep(1000);

	while (ib_buffer[TX_RX_BUF_SZ] != 1)
		i = 0;

	pr_info(DRIVER_NAME ": RC received data from EP done.\n");

	pr_info(DRIVER_NAME ": RC validating received data ...\n");
	for (i = 0; i < TX_RX_BUF_SZ; i++) {
		if (ib_buffer[i] != src_buf[i]) {
			pr_info(DRIVER_NAME
				": Error in receiving %d-th entry ", i);
			return;
		}
	}
	pr_info(DRIVER_NAME ": RC received data validated\n");
}
EXPORT_SYMBOL(keystone_pcie_send_receive_data);

static u32 *alloc_inbound_buffer(u32 len)
{
	u32 align_sz;
	u32 *b = 0;

	/* 256B may be wasted here due to alignment requirement */
	align_sz = L1_CACHE_ALIGN(len) + IB_BUFFER_ALIGN_SIZE;
	b = kzalloc(align_sz, GFP_KERNEL);
	if (b) {
		if ((u32)b & IB_BUFFER_ALIGN_MASK) {
			/* buffer pointer is not aligned on 256B boundary,
			move to next 256B boundary */
			b = (u32 *)(((u32)b & ~IB_BUFFER_ALIGN_SIZE) +
						IB_BUFFER_ALIGN_SIZE);
		}
	} else
		pr_err(DRIVER_NAME
			": Inbound buffer allocation FAILED, align_sz=%d\n",
			align_sz);

	return b;
}
#endif

static inline struct keystone_pcie_info *pbus_to_kspci(struct pci_bus *bus)
{
	struct pci_sys_data *root = bus->sysdata;

	if (root)
		return root->private_data;
	return NULL;
}

static int __init keystone_pcie_get_resources(struct device_node *node,
					struct keystone_pcie_info *info)
{
	unsigned long long pci_addr, cpu_addr, size;
	int rlen, pna = of_n_addr_cells(node), err;
	int np = pna + 5, memno = 0, iono = 0;
	struct resource *res;
	u32 pci_space, temp;
	const u32 *ranges;

	err = of_address_to_resource(node, 0, &info->cfg_resource);
	if (err < 0) {
		pr_err(DRIVER_NAME ": Not found reg property\n");
		return -EINVAL;
	}

	info->reg_cfg_virt = ioremap_nocache(info->cfg_resource.start,
					 resource_size(&info->cfg_resource));
	if (info->reg_cfg_virt == 0) {
		pr_err(DRIVER_NAME ": Couldn't map reg cfg address\n");
		return -ENOMEM;
	}

	ranges = of_get_property(node, "ranges", &rlen);
	if (ranges == NULL) {
		pr_err(DRIVER_NAME ": no range property\n");
		err = -EINVAL;
		goto err;
	}

	/* Parse the ranges */
	while ((rlen -= np * 4) >= 0) {
		/* Read next ranges element */
		pci_space = of_read_number(&ranges[0], 1);
		pci_addr = of_read_number(ranges + 1, 2);
		cpu_addr = of_translate_address(node, ranges + 3);
		size = of_read_number(ranges + pna + 3, 2);
		ranges += np;

		if (cpu_addr == OF_BAD_ADDR || size == 0)
			continue;

		/* Act based on address space type */
		res = NULL;
		switch ((pci_space >> 24) & 0x3) {
		case 1:         /* PCI IO space */
			pr_info(DRIVER_NAME
				": IO 0x%016llx..0x%016llx -> 0x%016llx\n",
				cpu_addr, cpu_addr + size - 1, pci_addr);

			/* We support only one IO range */
			if (iono >= 1) {
				pr_info(DRIVER_NAME
					":--> Skipped (too many) IO res!\n");
				continue;
			}
			res = &info->io_resource;
			res->flags = IORESOURCE_IO;
			res->start = pci_addr;
			res->name = "PCI I/O";
			iono++;
			break;
		case 2:         /* PCI Memory space */
		case 3:         /* PCI 64 bits Memory space */
			pr_info(DRIVER_NAME
				": MEM 0x%016llx..0x%016llx -> 0x%016llx %s\n",
				cpu_addr, cpu_addr + size - 1, pci_addr,
				(pci_space & 0x40000000) ? "Prefetch" : "");

			/* We support only 2 memory ranges */
			if (memno >= 1) {
				pr_info(DRIVER_NAME ":--> Skipped (too many)!\n");
				continue;
			}
			res = &info->mem_resource;
			res->flags = IORESOURCE_MEM;
			if (pci_space & 0x40000000) {
				pr_info(DRIVER_NAME
				": Skipped, don't support prefetch memory!\n");
				continue;
			}

			res->start = cpu_addr;
			res->name = "PCI Memory";
			memno++;
			break;
		default:
			pr_warn(DRIVER_NAME ": Unknown resource\n");
			break;
		}
		info->num_mem_resources = memno;

		if (res != NULL) {
			res->end = res->start + size - 1;
			res->parent = NULL;
			res->sibling = NULL;
			res->child = NULL;
		}
	}

	if ((memno == 0) || iono == 0) {
		pr_err(DRIVER_NAME ": No Mem/IO resources defined for PCIE\n");
		err = -EINVAL;
		goto err;
	}

	/* Get dma range for inbound memory access */
	res = &info->inbound_dma_res;
	res->start = 0;
	/* maximum 1G space */
	size = MAX_DMA_RANGE;
	res->end = size - 1;
	res->flags = IORESOURCE_MEM | IORESOURCE_PREFETCH;

	/* Get dma-ranges property */
	ranges = of_get_property(node, "dma-ranges", &rlen);
	if (ranges == NULL) {
		pr_err(DRIVER_NAME ": no dma range property\n");
		err = -EINVAL;
		goto err;
	}

	/* read ranges element */
	pci_space = of_read_number(&ranges[0], 1);
	pci_addr = of_read_number(ranges + 1, 2);
	cpu_addr = of_translate_address(node, ranges + 3);
	size = of_read_number(ranges + pna + 3, 2);
	if (cpu_addr == OF_BAD_ADDR || size == 0) {
		pr_err(DRIVER_NAME ": Invalid cpu address in dma range\n");
		err = -EINVAL;
		goto err;
	}

	temp = (pci_space >> 24) & 3;
	/* We only care about memory */
	if (temp != 2 && temp != 3) {
		pr_err(DRIVER_NAME ": Invalid memory in dma range\n");
		err = -EINVAL;
		goto err;
	}

	if (size > MAX_DMA_RANGE) {
		pr_err(DRIVER_NAME ": Invalid dma range size\n");
		err = -EINVAL;
		goto err;
	}

	if (!(pci_space & 0x40000000))
		res->flags &= ~IORESOURCE_PREFETCH;

	res->start = pci_addr;
	res->end = res->start + size - 1;

	/* support upto 4 legacy irqs */
	do {
		info->legacy_irqs[info->num_legacy_irqs] =
			irq_of_parse_and_map(node, info->num_legacy_irqs);
		if (info->legacy_irqs[info->num_legacy_irqs] < 0)
			break;
		info->num_legacy_irqs++;
	} while (info->num_legacy_irqs < MAX_LEGACY_HOST_IRQS);

	if (!info->num_legacy_irqs) {
		pr_err(DRIVER_NAME ": No host legacy irqs defined\n");
		err = -EINVAL;
		goto err;
	}
	pr_info(DRIVER_NAME
		": pcie - number of legacy irqs = %d\n", info->num_legacy_irqs);

#ifdef CONFIG_PCI_MSI
	/*
	 * support upto 32 MSI irqs. Actual numbers configured through
	 * dt from index 4 to 11
	 */
	do {
		info->msi_host_irqs[info->num_msi_host_irqs] =
			irq_of_parse_and_map(node, info->num_legacy_irqs +
						info->num_msi_host_irqs);
		if (info->msi_host_irqs[info->num_msi_host_irqs] < 0)
			break;
		info->num_msi_host_irqs++;
	} while (info->num_msi_host_irqs < MAX_MSI_HOST_IRQS);

	if (!info->num_msi_host_irqs) {
		pr_err(DRIVER_NAME ": No MSI host irqs defined\n");
		err = -EINVAL;
		goto err;
	}

	info->num_msi_irqs = info->num_msi_host_irqs * 4;
	if (info->num_msi_irqs > MAX_MSI_IRQS) {
		info->num_msi_irqs = MAX_MSI_IRQS;
		pr_warn(DRIVER_NAME
			": MSI irqs exceeds maximum capacity. Set to max.\n");
	}

	pr_info(DRIVER_NAME
		": pcie - number of MSI host irqs = %d, msi_irqs = %d\n",
		 info->num_msi_host_irqs, info->num_msi_irqs);
#endif
	info->error_irq = irq_of_parse_and_map(node, info->num_legacy_irqs +
						info->num_msi_host_irqs);
	if (info->error_irq < 0) {
		pr_err(DRIVER_NAME ": No error irq defined\n");
		goto err;
	}

	return 0;
err:
	iounmap((void __iomem *)info->reg_cfg_virt);
	return err;
}

static int get_and_clear_err(void __iomem *reg_virt)
{
	int status = __raw_readl(reg_virt + ERR_IRQ_STATUS_RAW);

	if (status) {
		/* The PCIESS interrupt status buts are "write 1 to clear" */
		__raw_writel(status, reg_virt + ERR_IRQ_STATUS);

		/*
		 * Clear all errors. We are not worried here about individual
		 * status as no specific handling is required.
		 */
		__raw_writew(0xffff, reg_virt + SPACE0_LOCAL_CFG_OFFSET +
				PCI_STATUS);
	}
	return status;
}

/**
 * set_dbi_mode() - Set DBI mode to access overlaid BAR mask registers
 *
 * Since modification of dbi_cs2 involves different clock domain, read the
 * status back to ensure the transition is complete.
 */
static inline void set_dbi_mode(void __iomem *reg_virt)
{
	u32 val;

	__raw_writel(DBI_CS2_EN_VAL | __raw_readl(reg_virt + CMD_STATUS),
		     reg_virt + CMD_STATUS);

	do {
		val = __raw_readl(reg_virt + CMD_STATUS);
	} while (!(val & DBI_CS2_EN_VAL));
}

/**
 * clear_dbi_mode() - Disable DBI mode
 *
 * Since modification of dbi_cs2 involves different clock domain, read the
 * status back to ensure the transition is complete.
 */
static inline void clear_dbi_mode(void __iomem *reg_virt)
{
	u32 val;

	__raw_writel(~DBI_CS2_EN_VAL & __raw_readl(reg_virt + CMD_STATUS),
		     reg_virt + CMD_STATUS);

	do {
		val = __raw_readl(reg_virt + CMD_STATUS);
	} while (val & DBI_CS2_EN_VAL);
}

static void disable_bars(void __iomem *reg_virt)
{
	set_dbi_mode(reg_virt);

	__raw_writel(0,
		reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_BASE_ADDRESS_0);
	__raw_writel(0,
		reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_BASE_ADDRESS_1);

	clear_dbi_mode(reg_virt);
}

/**
 * set_outbound_trans() - Set PHYADDR <-> BUSADDR mapping for outbound
 */
static void set_outbound_trans(struct keystone_pcie_info *info)
{
	u32 start = info->mem_resource.start, end = info->mem_resource.end;
	void __iomem *reg_virt = info->reg_cfg_virt;
	int i, tr_size;

	pr_debug(DRIVER_NAME ": Setting outbound translation for %#x-%#x\n",
		 start, end);

	/* Set outbound translation size per window division */
	__raw_writel(CFG_PCIM_WIN_SZ_IDX & 0x7, reg_virt + OB_SIZE);

	tr_size = (1 << (CFG_PCIM_WIN_SZ_IDX & 0x7)) * SZ_1M;

	/* Using Direct 1:1 mapping of RC <-> PCI memory space */
	for (i = 0; (i < CFG_PCIM_WIN_CNT) && (start < end); i++) {
		__raw_writel(start | 1, reg_virt + OB_OFFSET_INDEX(i));
		__raw_writel(0,	reg_virt + OB_OFFSET_HI(i));
		start += tr_size;
	}

	/* TODO: ensure unused translation regions are disabled */

	/* Enable OB translation */
	__raw_writel(OB_XLAT_EN_VAL |
		 __raw_readl(reg_virt + CMD_STATUS), reg_virt + CMD_STATUS);
}

/**
 * keystone_pcie_fault() - ARM abort handler for PCIe non-posted completion aborts
 * @addr: Address target on which the fault generated
 * @fsr: CP15 fault status register value
 * @regs: Pointer to register structure on abort
 *
 * Handles precise abort caused due to PCIe operation.
 *
 * Note that we are relying on virtual address filtering to determine if the
 * target of the precise aborts was a PCIe module access (i.e., config, I/O,
 * register) and only handle such aborts. We could check PCIe error status to
 * confirm if the abort was caused due to non-posted completion status received
 * by PCIESS, but this may not always be true and aborts from some downstream
 * devices, such as PCI-PCI bridges etc may not result into error status bit
 * getting set.
 *
 * Ignores and returns abort as unhandled otherwise.
 *
 * Also note that, using error status check (as was done in earlier
 * implementation) would also handle failed memory accesses (non-posted), but
 * address filerting based handling will cause aborts for memory accesses as the
 * addresses will be outside the PCIESS module space. This seems OK, as any
 * memory access after enumeration is sole responsibility of the driver and the
 * system integrator (e.g., access failures due to hotplug, suspend etc). If
 * using error check based handling, we also need to clear PCIe error status on
 * detecting errors.
 *
 * Note: Due to specific h/w implementation, we can't be sure of what kind of
 * error occurred (UR Completion, CA etc) and all we get is raw error IRQ status
 * and probably SERR which indicate 'some kind of' error - fatal or non-fatal is
 * received/happened.
 */
static int
keystone_pcie_fault(unsigned long addr, unsigned int fsr,
		struct pt_regs *regs)
{
	unsigned long instr = *(unsigned long *) instruction_pointer(regs);

	/* Note: only handle such abort during PCI bus probing */
	if (!abort_check_base)
		return 0;

	get_and_clear_err((void __iomem *) abort_check_base);

	/*
	 * Mimic aborted read of all 1's as required to detect device/function
	 * absence - check if the instruction that caused abort was a LOAD,
	 */
	if ((instr & 0x0c100000) == 0x04100000) {
		int reg = (instr >> 12) & 15;
		unsigned long val;

		if (instr & 0x00400000)
			val = 255;
		else
			val = -1;

		regs->uregs[reg] = val;
		regs->ARM_pc += 4;
	}

	if ((instr & 0x0e100090) == 0x00100090) {
		int reg = (instr >> 12) & 15;

		regs->uregs[reg] = -1;
		regs->ARM_pc += 4;
	}

	pr_info(DRIVER_NAME ": Handled PCIe abort\n");

	return 0;
}

static void mask_legacy_irq(void __iomem *reg_virt, int i)
{
	__raw_writel(0x1, reg_virt + IRQ_ENABLE_CLR + (i << 4));
}

static void unmask_legacy_irq(void __iomem *reg_virt, int i)
{
	__raw_writel(0x1, reg_virt + IRQ_ENABLE_SET + (i << 4));
}

/**
 * keystone_legacy_irq_handler() - Handle legacy interrupt
 * @irq: IRQ line for legacy interrupts
 * @desc: Pointer to irq descriptor
 *
 * Traverse through pending legacy interrupts and invoke handler for each. Also
 * takes care of interrupt controller level mask/ack operation.
 */
static void keystone_legacy_irq_handler(unsigned int irq, struct irq_desc *desc)
{
	struct keystone_pcie_info *info = irq_desc_get_handler_data(desc);
	u32 irq_offset = irq - info->legacy_irqs[0], pending;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int virq;

	pr_debug(DRIVER_NAME ": Handling legacy irq %d\n", irq);

	/*
	 * The chained irq handler installation would have replaced normal
	 * interrupt driver handler so we need to take care of mask/unmask and
	 * ack operation.
	 */
	chip->irq_mask(&desc->irq_data);
	if (chip->irq_ack)
		chip->irq_ack(&desc->irq_data);

	pending = __raw_readl(info->reg_cfg_virt +
			IRQ_STATUS + (irq_offset << 4));

	if (BIT(0) & pending) {
		virq = irq_linear_revmap(info->legacy_irqd, irq_offset);
		pr_debug(DRIVER_NAME
			": irq: irq_offset %d, virq %d\n", irq_offset, virq);
		generic_handle_irq(virq);
	}

	/* EOI the INTx interrupt */
	__raw_writel(irq_offset, info->reg_cfg_virt + IRQ_EOI);

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
	chip->irq_unmask(&desc->irq_data);
}

static void ack_irq(struct irq_data *d)
{
}

static void mask_irq(struct irq_data *d)
{
}

static void unmask_irq(struct irq_data *d)
{
}

static struct irq_chip keystone_legacy_irq_chip = {
	.name = "PCIe-LEGACY-IRQ",
	.irq_ack = ack_irq,
	.irq_mask = mask_irq,
	.irq_unmask = unmask_irq,
};

#ifdef CONFIG_PCI_MSI
static DECLARE_BITMAP(msi_irq_bits, MAX_MSI_IRQS);

/**
 * keystone_msi_handler() - Handle MSI interrupt
 * @irq: IRQ line for MSI interrupts
 * @desc: Pointer to irq descriptor
 *
 * Traverse through pending MSI interrupts and invoke handler for each. Also
 * takes care of interrupt controller level mask/ack operation.
 */
static void keystone_msi_handler(unsigned int irq, struct irq_desc *desc)
{
	struct keystone_pcie_info *info = irq_desc_get_handler_data(desc);
	u32 offset = irq - info->msi_host_irqs[0], pending, vector;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int src, virq;

	pr_debug(DRIVER_NAME ": Handling MSI irq %d\n", irq);

	/*
	 * The chained irq handler installation would have replaced normal
	 * interrupt driver handler so we need to take care of mask/unmask and
	 * ack operation.
	 */
	chip->irq_mask(&desc->irq_data);
	if (chip->irq_ack)
		chip->irq_ack(&desc->irq_data);

	pending = __raw_readl(info->reg_cfg_virt +
			MSI0_IRQ_STATUS + (offset << 4));
	/*
	 * MSI0, Status bit 0-3 shows vectors 0, 8, 16, 24, MSI1 status bit
	 * shows 1, 9, 17, 25 and so forth
	 */
	for (src = 0; src < 4; src++) {
		if (BIT(src) & pending) {
			vector = offset + (src << 3);
			virq = irq_linear_revmap(info->msi_irqd, vector);
			pr_debug(DRIVER_NAME
				": irq: bit %d, vector %d, virq %d\n",
				 src, vector, virq);
			generic_handle_irq(virq);
		}
	}

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
	chip->irq_unmask(&desc->irq_data);
}

static void ack_msi(struct irq_data *d)
{
	struct keystone_pcie_info *info = irq_data_get_irq_chip_data(d);
	u32 offset, reg_offset, bit_pos;
	unsigned int irq = d->irq;

	offset = irq - irq_linear_revmap(info->msi_irqd, 0);

	reg_offset = offset % 8;
	bit_pos = offset >> 3;

	__raw_writel(BIT(bit_pos),
		info->reg_cfg_virt + MSI0_IRQ_STATUS + (reg_offset << 4));

	__raw_writel(reg_offset + MSI_IRQ_OFFSET, info->reg_cfg_virt + IRQ_EOI);
}

static void mask_msi(struct irq_data *d)
{
	struct keystone_pcie_info *info = irq_data_get_irq_chip_data(d);
	u32 offset, reg_offset, bit_pos;
	unsigned int irq = d->irq;

	offset = irq - irq_linear_revmap(info->msi_irqd, 0);

	reg_offset = offset % 8;
	bit_pos = offset >> 3;

	__raw_writel(BIT(bit_pos),
		info->reg_cfg_virt + MSI0_IRQ_ENABLE_CLR + (reg_offset << 4));
}

static void unmask_msi(struct irq_data *d)
{
	struct keystone_pcie_info *info = irq_data_get_irq_chip_data(d);
	u32 offset, reg_offset, bit_pos;
	unsigned int irq = d->irq;

	offset = irq - irq_linear_revmap(info->msi_irqd, 0);

	reg_offset = offset % 8;
	bit_pos = offset >> 3;

	__raw_writel(BIT(bit_pos),
		info->reg_cfg_virt + MSI0_IRQ_ENABLE_SET + (reg_offset << 4));
}

/*
 * TODO: Add support for mask/unmask on remote devices (mask_msi_irq and
 * unmask_msi_irq). Note that, this may not work always - requires endpoints to
 * support mask bits capability.
 */
static struct irq_chip keystone_msi_chip = {
	.name = "PCIe-MSI",
	.irq_ack = ack_msi,
	.irq_mask = mask_msi,
	.irq_unmask = unmask_msi,
};

/**
 * get_free_msi() - Get a free MSI number
 * @msi_irq_num - Maximum number of MSI irqs supported
 *
 * Checks for availability of MSI and returns the first available.
 */
static int get_free_msi(int msi_irq_num)
{
	int bit;

	do {
		bit = find_first_zero_bit(msi_irq_bits, msi_irq_num);

		if (bit >= msi_irq_num)
			return -ENOSPC;

	} while (test_and_set_bit(bit, msi_irq_bits));

	pr_debug(DRIVER_NAME ": MSI %d available\n", bit);

	return bit;
}

/**
 * arch_setup_msi_irq() - Set up an MSI for Endpoint
 * @pdev: Pointer to PCI device structure of requesting EP
 * @desc: Pointer to MSI descriptor data
 *
 * Assigns an MSI to endpoint and sets up corresponding irq. Also passes the MSI
 * information to the endpoint.
 *
 * TODO: Add 64-bit addressing support
 */
int arch_setup_msi_irq(struct pci_dev *pdev, struct msi_desc *desc)
{
	/* assume we have rc bus set in pdev->bus */
	struct keystone_pcie_info *info = pbus_to_kspci(pdev->bus);
	int ret, irq;
	struct msi_msg msg;

	if (!info) {
		pr_err(DRIVER_NAME ": Can't get the controller private_data\n");
		return -ENODEV;
	}

	if (!info->num_msi_host_irqs) {
		pr_err(DRIVER_NAME ": Number of MSI IRQs not configured\n");
		return -EINVAL;
	}

	ret = get_free_msi(info->num_msi_irqs);
	if (ret < 0) {
		pr_err(DRIVER_NAME ": Failed to get free MSI\n");
	} else {
		irq = irq_linear_revmap(info->msi_irqd, ret);
		msg.data = ret;

		ret = irq_set_msi_desc(irq, desc);

		if (!ret) {
			msg.address_hi = 0;
			msg.address_lo = info->cfg_resource.start + MSI_IRQ;

			pr_debug(DRIVER_NAME ": MSI %d @%#x:%#x, irq = %d\n",
				 msg.data, msg.address_hi,
				 msg.address_lo, irq);

			write_msi_msg(irq, &msg);

			irq_set_chip_data(irq, info);
			irq_set_chip_and_handler(irq, &keystone_msi_chip,
						 handle_level_irq);
			set_irq_flags(irq, IRQF_VALID);
		}
	}
	return ret;
}

void arch_teardown_msi_irq(unsigned int irq)
{
	struct msi_desc *entry = irq_get_msi_desc(irq);
	struct pci_dev *pdev = entry->dev;
	struct keystone_pcie_info *info = pbus_to_kspci(pdev->bus);

	if (info) {
		int pos = irq - irq_linear_revmap(info->msi_irqd, 0);
		dynamic_irq_cleanup(irq);
		clear_bit(pos, msi_irq_bits);
		return;
	}
	pr_err(DRIVER_NAME
		": arch_teardown_msi_irq, can't find driver data\n");
}
#endif

static irqreturn_t pcie_err_irq_handler(int irq, void *reg_virt)
{
	int ret = IRQ_NONE, status;

	status = __raw_readl(reg_virt + ERR_IRQ_STATUS_RAW) & ERR_IRQ_ALL;
	if (status) {
		/* The PCIESS interrupt status buts are "write 1 to clear" */
		if (status & ERR_FATAL_IRQ)
			pr_err(DRIVER_NAME ": PCIE fatal error detected\n");

		/* AER driver will handle the error. Just ack the irq event */
		__raw_writel(status, reg_virt + ERR_IRQ_STATUS);
		ret = IRQ_HANDLED;
	}
	return ret;
}

/**
 * keystone_pcie_setup() - Perform PCIe system setup.
 * @nr: PCI controller index
 * @sys: PCI data for the controller
 *
 * Initialize and configure PCIe Root Complex h/w and fill up resource data to
 * be used by PCI core enumeration layer.
 *
 * H/W initializations consist mainly of:
 * - Establish link with downstream.
 * - Set up outbound access.
 * - Enable memory and IO access.
 *
 * Following resources are allotted for bios enumeration:
 * - Non-Prefetch memory
 * - 32-bit IO
 * - Legacy interrupt
 * - MSI (if enabled)
 *
 * TODO: Add
 * - Prefetchable memory
 * - 64-bit addressing support
 * - Additional delay/handshaking for EPs indulging in CRRS
 */

static __init int keystone_pcie_setup(int nr, struct pci_sys_data *sys)
{
	struct keystone_pcie_info *info;
	void __iomem *reg_virt;
	int i, err;
	u32 temp;

	if (nr != 0)
		return 0;

	info = (struct keystone_pcie_info *)sys->private_data;
	reg_virt = info->reg_cfg_virt;

	/* Not able to pass this to the fault code cleanly */
	abort_check_base = (u32)reg_virt;

	err = insert_resource(&iomem_resource, &info->mem_resource);
	if (err < 0) {
		pr_err(DRIVER_NAME
			": Failed to reserve mem resource, err %x\n",
			err);
		return err;
	}
	pci_add_resource(&sys->resources, &info->mem_resource);

	err = insert_resource(&ioport_resource, &info->io_resource);
	if (err < 0) {
		pr_err(DRIVER_NAME
			": Failed to reserve io resource, err %x\n",
			err);
		goto free_resource_0;
	}
	pci_add_resource(&sys->resources, &info->io_resource);

	if (info->pdata->en_link_train) {
		/*
		 * KeyStone devices do not support h/w autonomous
		 * link up-training to GEN2 from GEN1 in either EP/RC modes.
		 * The software needs to initiate speed change.
		 */
		temp = __raw_readl(
				reg_virt + SPACE0_LOCAL_CFG_OFFSET + PL_GEN2);
		__raw_writel(DIR_SPD | temp,
				reg_virt + SPACE0_LOCAL_CFG_OFFSET + PL_GEN2);
		/*
		 * Initiate Link Training. We will delay for L0 as specified by
		 * standard, but will still proceed and return success
		 * irrespective of L0 status as this will be handled by explicit
		 * L0 state checks during enumeration.
		 */
		temp = __raw_readl(reg_virt + CMD_STATUS);
		__raw_writel(LTSSM_EN_VAL | temp, reg_virt + CMD_STATUS);

		/* 100ms */
		msleep(100);
	}

	/*
	 * Identify ourselves as 'Bridge' for enumeration purpose. This also
	 * avoids "Invalid class 0000 for header type 01" warnings from "lspci".
	 *
	 * If at all we want to restore the default class-subclass values, the
	 * best place would be after returning from pci_common_init ().
	 */
	__raw_writew(PCI_CLASS_BRIDGE_PCI,
		     reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_CLASS_DEVICE);

	/*
	 * Prevent the enumeration code from assigning resources to our BARs. We
	 * will set up them after the scan is complete.
	 */
	disable_bars(reg_virt);

	set_outbound_trans(info);

	/* Enable 32-bit IO addressing support */
	__raw_writew(PCI_IO_RANGE_TYPE_32 | (PCI_IO_RANGE_TYPE_32 << 8),
		     reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_IO_BASE);

	/*
	 * FIXME: The IO Decode size bits in IO base and limit registers are
	 * writable from host any time and during enumeration, the Linux PCI
	 * core clears the lower 4-bits of these registers while writing lower
	 * IO address. This makes IO upper address and limit registers to show
	 * value as '0' and not the actual value as configured by the core
	 * during enumeration. We need to re-write bits 0 of IO limit and base
	 * registers again. Need to find if a post configuration hook is
	 * possible. An easier and clear but possibly inefficient WA is to snoop
	 * each config write and restore 32-bit IO decode configuration.
	 */

	for (i = 0; i < info->num_legacy_irqs; i++) {
		if (info->legacy_irqs[i] >= 0)
			unmask_legacy_irq(reg_virt, i);
		else
			mask_legacy_irq(reg_virt, i);
	}

#ifdef CONFIG_PCI_MSI
	for (i = 0; i < info->num_msi_host_irqs; i++) {
		irq_set_chained_handler(info->msi_host_irqs[i],
			keystone_msi_handler);
		irq_set_handler_data(info->msi_host_irqs[i], info);
	}
#endif

	get_and_clear_err(reg_virt);

	/*
	 * PCIe access errors that result into OCP errors are caught by ARM as
	 * "External aborts" (Precise).
	 */
	hook_fault_code(17, keystone_pcie_fault, SIGBUS, 0,
			"external abort on linefetch");

	if (request_irq(info->error_irq, pcie_err_irq_handler, IRQF_SHARED,
			"pcie-error-irq", reg_virt) < 0) {
		pr_err(DRIVER_NAME ": Failed to request error irq\n");
		goto free_resource_0;
	}
	/* enable the fatal error irq */
	__raw_writel(ERR_IRQ_ALL, reg_virt + ERR_IRQ_ENABLE_SET);

	pr_info(DRIVER_NAME ": Doing PCI Setup...Done\n");
	return 1;
free_resource_0:
	release_resource(&info->mem_resource);
	return -1;
}

/**
 * set_inbound_trans() - Setup inbound access
 *
 * Configure BAR0 and BAR1 for inbound access. BAR0 is set up in h/w to have
 * access to PCIESS application register space and just needs to set up inbound
 * address (mainly used for MSI). While BAR1 is set up to provide translation
 * into specified (SoC/Board level) internal address space.
 *
 * Note: 1:1 mapping for internal addresses is used.
 *
 * TODO: Add 64-bit support
 */
static void set_inbound_trans(struct keystone_pcie_info *info)
{
	u32 ram_base = info->inbound_dma_res.start,
	reg_cfg_phys = info->cfg_resource.start,
	ram_end = info->inbound_dma_res.end;
	void __iomem *reg_virt = info->reg_cfg_virt;

	/* Configure and set up BAR0 */
	set_dbi_mode(reg_virt);

	/* Enable BAR0 */
	__raw_writel(1,
		reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_BASE_ADDRESS_0);
	__raw_writel(SZ_4K - 1,
		reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_BASE_ADDRESS_0);

	clear_dbi_mode(reg_virt);

	 /*
	  * For BAR0, just setting bus address for inbound writes (MSI) should
	  * be sufficient. Use physical address to avoid any conflicts.
	  */
	__raw_writel(reg_cfg_phys,
		reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_BASE_ADDRESS_0);

	/* Configure BAR1 only if inbound window is specified */
	if (ram_base == ram_end)
		return;

	/*
	 * Set Inbound translation. Skip BAR0 as it will have h/w
	 * default set to open application register space.
	 *
	 * The value programmed in IB_STARTXX registers must be same as
	 * the one set in corresponding BAR from PCI config space.
	 *
	 * We use translation 'offset' value to yield 1:1 mapping so as
	 * to have physical address on RC side = Inbound PCIe link
	 * address. This also ensures no overlapping with base/limit
	 * regions (outbound).
	 */
	__raw_writel(ram_base, reg_virt + IB_START0_LO);
	__raw_writel(0, reg_virt + IB_START0_HI);
	__raw_writel(1, reg_virt + IB_BAR0);
#ifdef XFER_TEST
	ib_buffer = alloc_inbound_buffer(IB_BUFFER_DEFAULT_SIZE);
	__raw_writel((u32)ib_buffer, reg_virt + IB_OFFSET0);
#else
	__raw_writel(ram_base, reg_virt + IB_OFFSET0);
#endif

	/*
	 * Set BAR1 mask to accomodate inbound window
	 */
	set_dbi_mode(reg_virt);

	__raw_writel(1,
		reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_BASE_ADDRESS_1);

	__raw_writel(ram_end - ram_base,
		reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_BASE_ADDRESS_1);

	clear_dbi_mode(reg_virt);

	/* Set BAR1 attributes and value in config space */
	__raw_writel(ram_base | PCI_BASE_ADDRESS_MEM_PREFETCH,
		     reg_virt + SPACE0_LOCAL_CFG_OFFSET + PCI_BASE_ADDRESS_1);

	/*
	 * Enable IB translation only if BAR1 is set. BAR0 doesn't
	 * require enabling IB translation as it is set up in h/w
	 */
	__raw_writel(IB_XLAT_EN_VAL | __raw_readl(reg_virt + CMD_STATUS),
			     reg_virt + CMD_STATUS);
}

/**
 * check_device() - Checks device availability
 * @bus: Pointer to bus to check the device availability on
 * @dev: Device number
 *
 * Checks for the possibility of device being present. Relies on following
 * logic to indicate success:
 * - downstream link must be established to traverse PCIe fabric
 * - treating RC as virtual PCI bridge, first (and only) device on bus 1 will be
 *   numbered as 0
 * - don't check device number beyond bus 1 as device on our secondary side may
 *   as well be a PCIe-PCI bridge
 */
static int check_device(void __iomem *reg_virt, struct pci_bus *bus, int dev)
{
	if ((__raw_readl(reg_virt + SPACE0_LOCAL_CFG_OFFSET + DEBUG0)
		& LTSSM_STATE_MASK) != LTSSM_STATE_L0)
		return 0;

	if (bus->number <= 1) {
		if (dev == 0)
			return 1;
		else
			return 0;
	}
	return 1;
}

/**
 * setup_config_addr() - Set up configuration space address for a device
 * @bus: Bus number the device is residing on
 * @device: Device number
 * @function: Function number in device
 * @where: Offset of configuration register
 *
 * Forms and returns the address of configuration space mapped in PCIESS
 * address space 0. Also configures CFG_SETUP for remote configuration space
 * access.
 *
 * The address space has two regions to access configuration - local and remote.
 * We access local region for bus 0 (as RC is attached on bus 0) and remote
 * region for others with TYPE 1 access when bus > 1. As for device on bus = 1,
 * we will do TYPE 0 access as it will be on our secondary bus (logical).
 * CFG_SETUP is needed only for remote configuration access.
 *
 * _NOTE_: Currently only supports device 0 on bus = 0 which is OK as PCIESS has
 * single root port.
 */
static inline void __iomem *setup_config_addr(void __iomem *reg_virt,
					u8 bus, u8 device, u8 function)
{
	u32 regval;

	if (bus == 0)
		return reg_virt + SPACE0_LOCAL_CFG_OFFSET;

	regval = (bus << 16) | (device << 8) | function;
	/*
	 * Since Bus#1 will be a virtual bus, we need to have TYPE0
	 * access only.
	 */
	/* TYPE 1 */
	if (bus != 1)
		regval |= BIT(24);

	__raw_writel(regval, reg_virt + CFG_SETUP);

	return reg_virt + SPACE0_REMOTE_CFG_OFFSET;
}

/**
 * keystone_pci_read_config() - Perform PCI configuration read from a device
 * @bus: Pointer to bus to access device on
 * @devfn: Device number of the bus and function number within
 * @where: Configuration space register offset
 * @size: Width of the register in bytes
 * @value: Pointer to hold the read value
 *
 * Note: We skip alignment check and locking since it is taken care by PCI
 * access wrappers.
 */
static int keystone_pci_read_config(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 *value)
{
	struct keystone_pcie_info *info = pbus_to_kspci(bus);
	u8 bus_num = bus->number;
	void __iomem *addr;

	if (!info) {
		pr_err(DRIVER_NAME ": Can't get driver data\n");
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	if (!check_device(info->reg_cfg_virt, bus, PCI_SLOT(devfn))) {
		*value = ~0;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	addr = setup_config_addr(info->reg_cfg_virt, bus_num, PCI_SLOT(devfn),
				PCI_FUNC(devfn));
	*value = __raw_readl(addr + (where & ~3));
	if (size == 1)
		*value = (*value >> (8 * (where & 3))) & 0xff;
	else if (size == 2)
		*value = (*value >> (8 * (where & 3))) & 0xffff;

	return PCIBIOS_SUCCESSFUL;
}

/**
 * keystone_pci_write_config() - Perform PCI configuration write to a device
 * @bus: Pointer to bus to access device on
 * @devfn: Device number of the bus and function number within
 * @where: Configuration space register offset
 * @size: Width of the register in bytes
 * @value: Value to write
 *
 * Note: We skip alignment check and locking since it is taken care by PCI
 * access wrappers.
 */
static int keystone_pci_write_config(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 value)
{
	struct keystone_pcie_info *info = pbus_to_kspci(bus);
	u8 bus_num = bus->number;
	void __iomem *addr;

	if (!info) {
		pr_err(DRIVER_NAME ": Can't get driver data\n");
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	if (!check_device(info->reg_cfg_virt, bus, PCI_SLOT(devfn)))
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = setup_config_addr(info->reg_cfg_virt, bus_num, PCI_SLOT(devfn),
				PCI_FUNC(devfn));

	if (size == 4)
		__raw_writel(value, addr + where);
	else if (size == 2)
		__raw_writew(value, addr + where);
	else
		__raw_writeb(value, addr + where);

	/*
	 * The h/w has a limitation where Config Writes don't signal aborts to
	 * processor. Clear explicitly to avoid stale status.
	 */
	get_and_clear_err(info->reg_cfg_virt);

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops keystone_pci_ops = {
	.read	= keystone_pci_read_config,
	.write	= keystone_pci_write_config,
};

static __init struct pci_bus *keystone_pcie_scan(int nr,
					struct pci_sys_data *sys)
{
	struct keystone_pcie_info *info;
	struct pci_bus *bus = NULL;

	pr_info(DRIVER_NAME ": Starting PCI scan, nr %d...\n", nr);
	if (nr != 0)
		return bus;

	info = (struct keystone_pcie_info *)sys->private_data;

	bus = pci_scan_root_bus(NULL, sys->busnr, &keystone_pci_ops, sys,
				&sys->resources);

	/* Post enumeration fixups */
	set_inbound_trans(info);

	pr_info(DRIVER_NAME ": Ending PCI scan...\n");
	return bus;
}

/**
 * keystone_pcie_map_irq() - Map a legacy interrupt to an IRQ
 * @dev: Device structure of End Point (EP) to assign IRQ to.
 * @slot: Device slot
 * @pin: Pin number for the function
 *
 */
static __init int keystone_pcie_map_irq(const struct pci_dev *dev, u8 slot,
					u8 pin)
{
	struct keystone_pcie_info *info = pbus_to_kspci(dev->bus);

	if (!info) {
		pr_err(DRIVER_NAME ": can get pci bus info\n");
		return -1;
	}

	pr_info(DRIVER_NAME
		": keystone_pcie_map_irq: slot %d, pin %d\n", slot, pin);
	if (!pin || pin > info->num_legacy_irqs) {
		pr_err(DRIVER_NAME ": pci irq pin out of range\n");
		return -1;
	}
	pr_info(DRIVER_NAME
		": keystone_pcie_map_irq: legacy_irq %d\n",
		info->virqs[pin - 1]);

	/* pin has values from 1-4 */
	return (info->virqs[pin - 1] >= 0) ?
		info->virqs[pin - 1] : -1;
}

#define PCIE_VENDOR_TI	0x104c
static void aer_use_platirq(struct pci_dev *dev)
{
	struct keystone_pcie_info *info = pbus_to_kspci(dev->bus);

	/* use platform IRQ for AER */
	if (PCI_FUNC(dev->devfn == 0) && PCI_SLOT(dev->devfn) == 0) {
		if (info)
			dev->irq = info->error_irq;
	}
}
DECLARE_PCI_FIXUP_FINAL(PCIE_VENDOR_TI, PCI_ANY_ID, aer_use_platirq);

static struct hw_pci __initdata keystone_pcie_hw = {
	.nr_controllers	= 1,
	.setup		= keystone_pcie_setup,
	.scan		= keystone_pcie_scan,
	.swizzle        = pci_common_swizzle,
	.map_irq	= keystone_pcie_map_irq,
};

/* keystone pcie device tree match tables */
static const struct of_device_id keystone_pci_match_ids[] __initconst = {
	{
		.type = "pci",
		.compatible = "ti,keystone2-pci",
		.data = &keystone2_data,
	},
	{}
};

static int __init keystone_pcie_controller_init(struct device_node *np,
						int domain)
{
	struct keystone_pcie_info *rc_info;
	const struct of_device_id *of_id;
	int err = -EINVAL, i;
	struct clk *pcie_clk;
	int port = 0;

	pr_info(DRIVER_NAME ": keystone_pcie_rc_init - start\n");

	rc_info = kzalloc(sizeof(*rc_info), GFP_KERNEL);
	if (!rc_info) {
		pr_err(DRIVER_NAME ": Memory alloc failure\n");
		return -ENOMEM;
	}

	of_id = of_match_node(keystone_pci_match_ids, np);

	if (of_id)
		rc_info->pdata = (struct keystone_pcie_pdata *)of_id->data;

	/* Setup platform specific initialization */
	if (rc_info->pdata) {
		of_property_read_u32(np, "ti,pcie-port", &port);
		err = rc_info->pdata->setup(rc_info->pdata, np, port);

		if (err < 0)
			goto err;
	}

	/* Enable controller Power and Clock domains */
	pcie_clk = of_clk_get(np, 0);
	if (IS_ERR(pcie_clk)) {
		pr_err(DRIVER_NAME ": Failed to get PCIESS clock\n");
		goto err;
	}

	if (clk_prepare_enable(pcie_clk)) {
		pr_err(DRIVER_NAME ": Failed to get enable clock\n");
		goto err1;
	}

	err = keystone_pcie_get_resources(np, rc_info);
	if (err < 0) {
		pr_err(DRIVER_NAME ": Unable to get resources\n");
		goto err1;
	}

	if (rc_info->num_legacy_irqs) {
		/* need to set up legacy irq chip */
		rc_info->legacy_irqd = irq_domain_add_linear(np,
					rc_info->num_legacy_irqs,
					&irq_domain_simple_ops, NULL);
		if (!rc_info->legacy_irqd) {
			pr_err(DRIVER_NAME
			       ": failed to add irq domain for legacy irqs\n");
			goto err1;
		}

		for (i = 0; i < rc_info->num_legacy_irqs; i++) {
			rc_info->virqs[i] =
				irq_create_mapping(rc_info->legacy_irqd, i);
			irq_set_chip_and_handler(rc_info->virqs[i],
				&keystone_legacy_irq_chip, handle_level_irq);
			irq_set_chip_data(rc_info->virqs[i], rc_info);
			set_irq_flags(rc_info->virqs[i], IRQF_VALID);

			if (rc_info->legacy_irqs[i] >= 0) {
				irq_set_handler_data(rc_info->legacy_irqs[i],
					rc_info);
				irq_set_chained_handler(rc_info->legacy_irqs[i],
					keystone_legacy_irq_handler);
			}
			set_irq_flags(rc_info->legacy_irqs[i], IRQF_VALID);
		}
	}

	if (rc_info->num_msi_irqs) {
		/* need to set up msi irq chip */
		rc_info->msi_irqd = irq_domain_add_linear(np,
					rc_info->num_msi_irqs,
					&irq_domain_simple_ops, NULL);
		if (!rc_info->msi_irqd) {
			pr_err(DRIVER_NAME
				": failed to add irq domain for msi irqs\n");
			goto err1;
		}

		for (i = 0; i < rc_info->num_msi_irqs; i++)
			irq_create_mapping(rc_info->msi_irqd, i);
	}
	keystone_pcie_hw.private_data = (void **)&rc_info;
	keystone_pcie_hw.domain = domain;
	pci_common_init(&keystone_pcie_hw);
	abort_check_base = 0;
	pr_info(DRIVER_NAME ": keystone_pcie_rc_init - end\n");

	return 0;
err1:
	clk_put(pcie_clk);
err:
	kfree(rc_info);
	of_node_put(np);
	return err;
}


static int __init keystone_pcie_rc_init(void)
{
	struct device_node *np = NULL;
	int ret = 0, domain = 0;

	pcibios_min_mem = 0;

	for_each_matching_node(np, keystone_pci_match_ids) {
		if (of_device_is_available(np))
			ret = keystone_pcie_controller_init(np, domain);
		domain++;
	}
	return ret;
}
subsys_initcall(keystone_pcie_rc_init);
