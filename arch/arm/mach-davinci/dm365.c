/*
 * TI DaVinci DM365 chip specific setup
 *
 * Copyright (C) 2009 Texas Instruments
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/serial_8250.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/spi/spi.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/platform_data/clk-davinci-pll.h>
#include <linux/platform_data/clk-davinci-psc.h>
#include <linux/platform_data/davinci-clock.h>


#include <asm/mach/map.h>

#include <mach/cputype.h>
#include <mach/edma.h>
#include <mach/psc.h>
#include <mach/mux.h>
#include <mach/irqs.h>
#include <mach/time.h>
#include <mach/serial.h>
#include <mach/common.h>
#include <linux/platform_data/keyscan-davinci.h>
#include <linux/platform_data/spi-davinci.h>
#include <mach/gpio-davinci.h>
#include <mach/pll.h>

#include "davinci.h"
#include "mux.h"
#include "asp.h"

#define PLLM		0x110
#define PREDIV          0x114
#define POSTDIV         0x128
#define PLLM_PLLM_MASK  0xff

#define DM365_REF_FREQ		24000000	/* 24 MHz on the DM365 EVM */
#define DM365_RTC_BASE			0x01c69000
#define DM365_KEYSCAN_BASE		0x01c69400
#define DM365_OSD_BASE			0x01c71c00
#define DM365_VENC_BASE			0x01c71e00
#define DAVINCI_DM365_VC_BASE		0x01d0c000
#define DAVINCI_DMA_VC_TX		2
#define DAVINCI_DMA_VC_RX		3
#define DM365_EMAC_BASE			0x01d07000
#define DM365_EMAC_MDIO_BASE		(DM365_EMAC_BASE + 0x4000)
#define DM365_EMAC_CNTRL_OFFSET		0x0000
#define DM365_EMAC_CNTRL_MOD_OFFSET	0x3000
#define DM365_EMAC_CNTRL_RAM_OFFSET	0x1000
#define DM365_EMAC_CNTRL_RAM_SIZE	0x2000

static struct clk_fixed_rate_data oscin_data = {
	.rate		= DM365_REF_FREQ,
	.flags		= CLK_IS_ROOT,
};

/* oscillator in as well as aux clock domain */
static struct davinci_clk ref_clk_oscin = {
	.name		= "oscin",
	.type		=  DAVINCI_FIXED_RATE_CLK,
	.clk_data	=  {
		.data	= &oscin_data,
	},
};

static struct clk_davinci_pll_data pll1_data = {
	.phy_pllm	= DAVINCI_PLL1_BASE + PLLM,
	.phy_prediv	= DAVINCI_PLL1_BASE + PREDIV,
	.phy_postdiv	= DAVINCI_PLL1_BASE + POSTDIV,
	.pllm_mask	= PLLM_PLLM_MASK,
	.prediv_mask	= PLLDIV_RATIO_MASK,
	.postdiv_mask	= PLLDIV_RATIO_MASK,
	.num		= 1,
	.pll_flags	= CLK_DAVINCI_PLL_HAS_PREDIV |
				 CLK_DAVINCI_PLL_HAS_POSTDIV,
};

static struct davinci_clk pll1_clk = {
	.name		= "pll1",
	.parent		= &ref_clk_oscin,
	.type		= DAVINCI_MAIN_PLL_CLK,
	.clk_data = {
		.data	= &pll1_data,
	},
};

static const char *pll1_plldiv_clk_mux_parents[] = {
						"oscin", "pll1"};

static struct clk_mux_data pll1_plldiv_clk_mux_data = {
	.shift		= PLLCTL_PLLEN_SHIFT,
	.width		= PLLCTL_PLLEN_WIDTH,
	.num_parents	= ARRAY_SIZE(pll1_plldiv_clk_mux_parents),
	.parents	= pll1_plldiv_clk_mux_parents,
	.phys_base	= DAVINCI_PLL1_BASE + PLLCTL,
};

static struct davinci_clk pll1_plldiv_clk_mux = {
	.name		= "pll1_plldiv_clk_mux",
	.parent		= &pll1_clk,
	.type		= DAVINCI_MUX_CLK,
	.clk_data	= {
		.data	= &pll1_plldiv_clk_mux_data,
	},
};

#define define_pll1_div_clk(__pll, __div, __name)		\
	static struct clk_divider_data pll1_div_data##__div = {	\
		.div_reg	= DAVINCI_PLL1_BASE + PLLDIV##__div,	\
		.width		= 5,				\
	};							\
								\
	static struct davinci_clk __name = {			\
		.name		= #__name,			\
		.parent		= &__pll,			\
		.type		= DAVINCI_PRG_DIV_CLK,		\
		.clk_data	= {				\
			.data	=  &pll1_div_data##__div,	\
		},						\
	};

static struct clk_fixed_factor_data fixed_clk_data = {
	.mult		= 1,
	.div		= 1,
};

static struct davinci_clk pll1_aux_clk = {
	.name			= "pll1_aux_clk",
	.parent			= &ref_clk_oscin,
	.type			= DAVINCI_FIXED_FACTOR_CLK,
	.clk_data		= {
		.fixed_factor	=  &fixed_clk_data,
	},
};

static struct clk_divider_data pll1_sysclkbp_data = {
	.div_reg		= DAVINCI_PLL2_BASE + BPDIV,
	.width			= 5,
};

static struct davinci_clk pll1_sysclkbp = {
	.name		= "pll1_sysclkbp",
	.parent		= &ref_clk_oscin,
	.type		= DAVINCI_PRG_DIV_CLK,
	.clk_data	= {
		.data	= &pll1_sysclkbp_data,
	},
};

static struct clk_divider_data pll1_obsclk_data = {
	.div_reg	= DAVINCI_PLL1_BASE + OSCDIV1,
	.width		= 5,
};

static struct davinci_clk pll1_clkout0 = {
	.name		= "clkout0",
	.parent		= &ref_clk_oscin,
	.type		= DAVINCI_PRG_DIV_CLK,
	.clk_data	= {
		.data	= &pll1_obsclk_data,
	},
};

define_pll1_div_clk(pll1_plldiv_clk_mux, 1, pll1_sysclk1);
define_pll1_div_clk(pll1_plldiv_clk_mux, 2, pll1_sysclk2);
define_pll1_div_clk(pll1_plldiv_clk_mux, 3, pll1_sysclk3);
define_pll1_div_clk(pll1_plldiv_clk_mux, 4, pll1_sysclk4);
define_pll1_div_clk(pll1_plldiv_clk_mux, 5, pll1_sysclk5);
define_pll1_div_clk(pll1_plldiv_clk_mux, 6, pll1_sysclk6);
define_pll1_div_clk(pll1_plldiv_clk_mux, 7, pll1_sysclk7);
define_pll1_div_clk(pll1_plldiv_clk_mux, 8, pll1_sysclk8);
define_pll1_div_clk(pll1_plldiv_clk_mux, 9, pll1_sysclk9);

static struct clk_davinci_pll_data pll2_data = {
	.phy_pllm	= DAVINCI_PLL2_BASE + PLLM,
	.phy_prediv	= DAVINCI_PLL2_BASE + PREDIV,
	.phy_postdiv	= DAVINCI_PLL2_BASE + POSTDIV,
	.pllm_mask	= PLLM_PLLM_MASK,
	.prediv_mask	= PLLDIV_RATIO_MASK,
	.postdiv_mask	= PLLDIV_RATIO_MASK,
	.num = 2,
	.pll_flags	= CLK_DAVINCI_PLL_HAS_PREDIV |
				CLK_DAVINCI_PLL_HAS_POSTDIV,
};

static struct davinci_clk pll2_clk = {
	.name		= "pll2",
	.parent		= &ref_clk_oscin,
	.type		= DAVINCI_MAIN_PLL_CLK,
	.clk_data	= {
		.data	= &pll2_data,
	},
};

static struct davinci_clk pll2_aux_clk = {
	.name			= "pll2_aux_clk",
	.parent			= &ref_clk_oscin,
	.type			= DAVINCI_FIXED_FACTOR_CLK,
	.clk_data		= {
		.fixed_factor	=  &fixed_clk_data,
	},
};

static const char *pll2_plldiv_clk_mux_parents[] = {
						"ref_clk_mux", "pll2"};

static struct clk_mux_data pll2_plldiv_clk_mux_data = {
	.shift		= PLLCTL_PLLEN_SHIFT,
	.width		= PLLCTL_PLLEN_WIDTH,
	.num_parents	= ARRAY_SIZE(pll2_plldiv_clk_mux_parents),
	.parents	= pll2_plldiv_clk_mux_parents,
	.phys_base	= DAVINCI_PLL2_BASE + PLLCTL,
};

static struct davinci_clk pll2_plldiv_clk_mux = {
	.name		= "pll2_plldiv_clk_mux",
	.parent		= &pll2_clk,
	.type		= DAVINCI_MUX_CLK,
	.clk_data	= {
		.data	= &pll2_plldiv_clk_mux_data,
	},
};

#define define_pll2_div_clk(__pll, __div, __name)	\
	static struct clk_divider_data pll2_div_data##__div = {	\
		.div_reg	= DAVINCI_PLL2_BASE + PLLDIV##__div,	\
		.width		= 5,				\
	};							\
								\
	static struct davinci_clk __name = {			\
		.name		= #__name,			\
		.parent		= &__pll,			\
		.type		= DAVINCI_PRG_DIV_CLK,		\
		.clk_data	= {				\
			.data	=  &pll2_div_data##__div,	\
		},						\
	};

static struct clk_divider_data pll2_obsclk_data = {
	.div_reg	= DAVINCI_PLL2_BASE + OSCDIV1,
	.width		= 5,
};

static struct davinci_clk pll2_clkout1 = {
	.name		= "clkout1",
	.parent		= &ref_clk_oscin,
	.type		= DAVINCI_PRG_DIV_CLK,
	.clk_data	= {
		.data	= &pll2_obsclk_data,
	},
};

define_pll2_div_clk(pll2_plldiv_clk_mux, 1, pll2_sysclk1);
define_pll2_div_clk(pll2_plldiv_clk_mux, 2, pll2_sysclk2);
define_pll2_div_clk(pll2_plldiv_clk_mux, 3, pll2_sysclk3);
define_pll2_div_clk(pll2_plldiv_clk_mux, 4, pll2_sysclk4);
define_pll2_div_clk(pll2_plldiv_clk_mux, 5, pll2_sysclk5);

#define __lpsc_clk(cname, _parent, mod, flgs, _flgs, dom)	\
	static struct clk_davinci_psc_data clk_psc_data##cname = {	\
		.domain	= DAVINCI_GPSC_##dom,			\
		.lpsc	= DAVINCI_LPSC_##mod,			\
		.flags	= flgs,					\
	};							\
								\
	static struct davinci_clk clk_##cname = {		\
		.name		= #cname,			\
		.parent		= &_parent,			\
		.flags		= _flgs,			\
		.type		= DAVINCI_PSC_CLK,		\
		.clk_data	= {				\
			.data	= &clk_psc_data##cname		\
		},						\
	};

#define lpsc_clk_enabled(cname, parent, mod)		\
	__lpsc_clk(cname, parent, mod, 0, ALWAYS_ENABLED, ARMDOMAIN)

#define lpsc_clk(cname, flgs, parent, mod, dom)		\
	__lpsc_clk(cname, parent, mod, flgs, 0, dom)

#define __dm365_lpsc_clk(cname, _parent, mod, flgs, _flgs, dom)	\
	static struct clk_davinci_psc_data clk_psc_data##cname = {	\
		.domain	= DAVINCI_GPSC_##dom,			\
		.lpsc	= DM365_LPSC_##mod,			\
		.flags	= flgs,					\
	};							\
								\
	static struct davinci_clk clk_##cname = {		\
		.name		= #cname,			\
		.parent		= &_parent,			\
		.flags		= _flgs,			\
		.type		= DAVINCI_PSC_CLK,		\
		.clk_data	= {				\
			.data	= &clk_psc_data##cname		\
		},						\
	};

#define dm365_lpsc_clk(cname, flgs, parent, mod, dom)		\
	__dm365_lpsc_clk(cname, parent, mod, flgs, 0, dom)

dm365_lpsc_clk(vpss_dac, 0, pll1_sysclk3, DAC_CLK, ARMDOMAIN);
dm365_lpsc_clk(vpss_master, 0, pll1_sysclk5, VPSSMSTR, ARMDOMAIN);
lpsc_clk_enabled(arm, pll2_sysclk2, ARM);
lpsc_clk(uart0, 0, pll1_aux_clk, UART0, ARMDOMAIN);
lpsc_clk(uart1, 0, pll1_sysclk4, UART1, ARMDOMAIN);
lpsc_clk(i2c, 0, pll1_aux_clk, I2C, ARMDOMAIN);
lpsc_clk(mmcsd0, 0, pll1_sysclk8, MMC_SD, ARMDOMAIN);
dm365_lpsc_clk(mmcsd1, 0, pll1_sysclk4, MMC_SD1, ARMDOMAIN);
lpsc_clk(spi0, 0, pll1_sysclk4, SPI, ARMDOMAIN);
dm365_lpsc_clk(spi1, 0, pll1_sysclk4, SPI1, ARMDOMAIN);
dm365_lpsc_clk(spi2, 0, pll1_sysclk4, SPI2, ARMDOMAIN);
dm365_lpsc_clk(spi3, 0, pll1_sysclk4, SPI3, ARMDOMAIN);
dm365_lpsc_clk(spi4, 0, pll1_aux_clk, SPI4, ARMDOMAIN);
lpsc_clk(gpio, 0, pll1_sysclk4, GPIO, ARMDOMAIN);
lpsc_clk(aemif, 0, pll1_sysclk4, AEMIF, ARMDOMAIN);
lpsc_clk(pwm0, 0, pll1_aux_clk, PWM0, ARMDOMAIN);
lpsc_clk(pwm1, 0, pll1_aux_clk, PWM1, ARMDOMAIN);
lpsc_clk(pwm2, 0, pll1_aux_clk, PWM2, ARMDOMAIN);
dm365_lpsc_clk(pwm3, 0, pll1_aux_clk, PWM3, ARMDOMAIN);
lpsc_clk(timer0, 0, pll1_aux_clk, TIMER0, ARMDOMAIN);
lpsc_clk(timer1, 0, pll1_aux_clk, TIMER1, ARMDOMAIN);
lpsc_clk(timer2, CLK_IGNORE_UNUSED, pll1_aux_clk, TIMER2, ARMDOMAIN);
dm365_lpsc_clk(timer3, 0, pll1_aux_clk, TIMER3, ARMDOMAIN);
lpsc_clk(usb, 0, pll1_aux_clk, USB, ARMDOMAIN);
dm365_lpsc_clk(emac, 0, pll1_sysclk4, EMAC, ARMDOMAIN);
dm365_lpsc_clk(voice_codec, 0, pll2_sysclk4, VOICE_CODEC, ARMDOMAIN);
dm365_lpsc_clk(asp0, 0, pll1_sysclk4, McBSP1, ARMDOMAIN);
dm365_lpsc_clk(rto, 0, pll1_sysclk4, RTO, ARMDOMAIN);
dm365_lpsc_clk(mjcp, 0, pll1_sysclk3, MJCP, ARMDOMAIN);

static struct davinci_clk_lookup dm365_clks[] = {
	CLK(NULL, "oscin", &ref_clk_oscin),
	CLK(NULL, "pll1", &pll1_clk),
	CLK(NULL, "pll1_plldiv_clk_mux", &pll1_plldiv_clk_mux),
	CLK(NULL, "pll1_aux", &pll1_aux_clk),
	CLK(NULL, "pll1_sysclkbp", &pll1_sysclkbp),
	CLK(NULL, "clkout0", &pll1_clkout0),
	CLK(NULL, "pll1_sysclk1", &pll1_sysclk1),
	CLK(NULL, "pll1_sysclk2", &pll1_sysclk2),
	CLK(NULL, "pll1_sysclk3", &pll1_sysclk3),
	CLK(NULL, "pll1_sysclk4", &pll1_sysclk4),
	CLK(NULL, "pll1_sysclk5", &pll1_sysclk5),
	CLK(NULL, "pll1_sysclk6", &pll1_sysclk6),
	CLK(NULL, "pll1_sysclk7", &pll1_sysclk7),
	CLK(NULL, "pll1_sysclk8", &pll1_sysclk8),
	CLK(NULL, "pll1_sysclk9", &pll1_sysclk9),
	CLK(NULL, "pll2", &pll2_clk),
	CLK(NULL, "pll2_plldiv_clk_mux", &pll2_plldiv_clk_mux),
	CLK(NULL, "pll2_aux", &pll2_aux_clk),
	CLK(NULL, "clkout1", &pll2_clkout1),
	CLK(NULL, "pll2_sysclk1", &pll2_sysclk1),
	CLK(NULL, "pll2_sysclk2", &pll2_sysclk2),
	CLK(NULL, "pll2_sysclk3", &pll2_sysclk3),
	CLK(NULL, "pll2_sysclk4", &pll2_sysclk4),
	CLK(NULL, "pll2_sysclk5", &pll2_sysclk5),
	CLK(NULL, "vpss_dac", &clk_vpss_dac),
	CLK(NULL, "vpss_master", &clk_vpss_master),
	CLK(NULL, "arm", &clk_arm),
	CLK(NULL, "uart0", &clk_uart0),
	CLK(NULL, "uart1", &clk_uart1),
	CLK("i2c_davinci.1", NULL, &clk_i2c),
	CLK("davinci_mmc.0", NULL, &clk_mmcsd0),
	CLK("davinci_mmc.1", NULL, &clk_mmcsd1),
	CLK("spi_davinci.0", NULL, &clk_spi0),
	CLK("spi_davinci.1", NULL, &clk_spi1),
	CLK("spi_davinci.2", NULL, &clk_spi2),
	CLK("spi_davinci.3", NULL, &clk_spi3),
	CLK("spi_davinci.4", NULL, &clk_spi4),
	CLK(NULL, "gpio", &clk_gpio),
	CLK(NULL, "aemif", &clk_aemif),
	CLK(NULL, "pwm0", &clk_pwm0),
	CLK(NULL, "pwm1", &clk_pwm1),
	CLK(NULL, "pwm2", &clk_pwm2),
	CLK(NULL, "pwm3", &clk_pwm3),
	CLK(NULL, "timer0", &clk_timer0),
	CLK(NULL, "timer1", &clk_timer1),
	CLK("watchdog", NULL, &clk_timer2),
	CLK(NULL, "timer3", &clk_timer3),
	CLK(NULL, "usb", &clk_usb),
	CLK("davinci_emac.1", NULL, &clk_emac),
	CLK("davinci_voicecodec", NULL, &clk_voice_codec),
	CLK("davinci-mcbsp", NULL, &clk_asp0),
	CLK(NULL, "rto", &clk_rto),
	CLK(NULL, "mjcp", &clk_mjcp),
	CLK(NULL, NULL, NULL),
};

/*----------------------------------------------------------------------*/

#define INTMUX		0x18
#define EVTMUX		0x1c


static const struct mux_config dm365_pins[] = {
#ifdef CONFIG_DAVINCI_MUX
MUX_CFG(DM365,	MMCSD0,		0,   24,     1,	  0,	 false)

MUX_CFG(DM365,	SD1_CLK,	0,   16,    3,	  1,	 false)
MUX_CFG(DM365,	SD1_CMD,	4,   30,    3,	  1,	 false)
MUX_CFG(DM365,	SD1_DATA3,	4,   28,    3,	  1,	 false)
MUX_CFG(DM365,	SD1_DATA2,	4,   26,    3,	  1,	 false)
MUX_CFG(DM365,	SD1_DATA1,	4,   24,    3,	  1,	 false)
MUX_CFG(DM365,	SD1_DATA0,	4,   22,    3,	  1,	 false)

MUX_CFG(DM365,	I2C_SDA,	3,   23,    3,	  2,	 false)
MUX_CFG(DM365,	I2C_SCL,	3,   21,    3,	  2,	 false)

MUX_CFG(DM365,	AEMIF_AR_A14,	2,   0,     3,	  1,	 false)
MUX_CFG(DM365,	AEMIF_AR_BA0,	2,   0,     3,	  2,	 false)
MUX_CFG(DM365,	AEMIF_A3,	2,   2,     3,	  1,	 false)
MUX_CFG(DM365,	AEMIF_A7,	2,   4,     3,	  1,	 false)
MUX_CFG(DM365,	AEMIF_D15_8,	2,   6,     1,	  1,	 false)
MUX_CFG(DM365,	AEMIF_CE0,	2,   7,     1,	  0,	 false)
MUX_CFG(DM365,	AEMIF_CE1,	2,   8,     1,    0,     false)
MUX_CFG(DM365,	AEMIF_WE_OE,	2,   9,     1,    0,     false)

MUX_CFG(DM365,	MCBSP0_BDX,	0,   23,    1,	  1,	 false)
MUX_CFG(DM365,	MCBSP0_X,	0,   22,    1,	  1,	 false)
MUX_CFG(DM365,	MCBSP0_BFSX,	0,   21,    1,	  1,	 false)
MUX_CFG(DM365,	MCBSP0_BDR,	0,   20,    1,	  1,	 false)
MUX_CFG(DM365,	MCBSP0_R,	0,   19,    1,	  1,	 false)
MUX_CFG(DM365,	MCBSP0_BFSR,	0,   18,    1,	  1,	 false)

MUX_CFG(DM365,	SPI0_SCLK,	3,   28,    1,    1,	 false)
MUX_CFG(DM365,	SPI0_SDI,	3,   26,    3,    1,	 false)
MUX_CFG(DM365,	SPI0_SDO,	3,   25,    1,    1,	 false)
MUX_CFG(DM365,	SPI0_SDENA0,	3,   29,    3,    1,	 false)
MUX_CFG(DM365,	SPI0_SDENA1,	3,   26,    3,    2,	 false)

MUX_CFG(DM365,	UART0_RXD,	3,   20,    1,    1,	 false)
MUX_CFG(DM365,	UART0_TXD,	3,   19,    1,    1,	 false)
MUX_CFG(DM365,	UART1_RXD,	3,   17,    3,    2,	 false)
MUX_CFG(DM365,	UART1_TXD,	3,   15,    3,    2,	 false)
MUX_CFG(DM365,	UART1_RTS,	3,   23,    3,    1,	 false)
MUX_CFG(DM365,	UART1_CTS,	3,   21,    3,    1,	 false)

MUX_CFG(DM365,  EMAC_TX_EN,	3,   17,    3,    1,     false)
MUX_CFG(DM365,  EMAC_TX_CLK,	3,   15,    3,    1,     false)
MUX_CFG(DM365,  EMAC_COL,	3,   14,    1,    1,     false)
MUX_CFG(DM365,  EMAC_TXD3,	3,   13,    1,    1,     false)
MUX_CFG(DM365,  EMAC_TXD2,	3,   12,    1,    1,     false)
MUX_CFG(DM365,  EMAC_TXD1,	3,   11,    1,    1,     false)
MUX_CFG(DM365,  EMAC_TXD0,	3,   10,    1,    1,     false)
MUX_CFG(DM365,  EMAC_RXD3,	3,   9,     1,    1,     false)
MUX_CFG(DM365,  EMAC_RXD2,	3,   8,     1,    1,     false)
MUX_CFG(DM365,  EMAC_RXD1,	3,   7,     1,    1,     false)
MUX_CFG(DM365,  EMAC_RXD0,	3,   6,     1,    1,     false)
MUX_CFG(DM365,  EMAC_RX_CLK,	3,   5,     1,    1,     false)
MUX_CFG(DM365,  EMAC_RX_DV,	3,   4,     1,    1,     false)
MUX_CFG(DM365,  EMAC_RX_ER,	3,   3,     1,    1,     false)
MUX_CFG(DM365,  EMAC_CRS,	3,   2,     1,    1,     false)
MUX_CFG(DM365,  EMAC_MDIO,	3,   1,     1,    1,     false)
MUX_CFG(DM365,  EMAC_MDCLK,	3,   0,     1,    1,     false)

MUX_CFG(DM365,	KEYSCAN,	2,   0,     0x3f, 0x3f,  false)

MUX_CFG(DM365,	PWM0,		1,   0,     3,    2,     false)
MUX_CFG(DM365,	PWM0_G23,	3,   26,    3,    3,     false)
MUX_CFG(DM365,	PWM1,		1,   2,     3,    2,     false)
MUX_CFG(DM365,	PWM1_G25,	3,   29,    3,    2,     false)
MUX_CFG(DM365,	PWM2_G87,	1,   10,    3,    2,     false)
MUX_CFG(DM365,	PWM2_G88,	1,   8,     3,    2,     false)
MUX_CFG(DM365,	PWM2_G89,	1,   6,     3,    2,     false)
MUX_CFG(DM365,	PWM2_G90,	1,   4,     3,    2,     false)
MUX_CFG(DM365,	PWM3_G80,	1,   20,    3,    3,     false)
MUX_CFG(DM365,	PWM3_G81,	1,   18,    3,    3,     false)
MUX_CFG(DM365,	PWM3_G85,	1,   14,    3,    2,     false)
MUX_CFG(DM365,	PWM3_G86,	1,   12,    3,    2,     false)

MUX_CFG(DM365,	SPI1_SCLK,	4,   2,     3,    1,	 false)
MUX_CFG(DM365,	SPI1_SDI,	3,   31,    1,    1,	 false)
MUX_CFG(DM365,	SPI1_SDO,	4,   0,     3,    1,	 false)
MUX_CFG(DM365,	SPI1_SDENA0,	4,   4,     3,    1,	 false)
MUX_CFG(DM365,	SPI1_SDENA1,	4,   0,     3,    2,	 false)

MUX_CFG(DM365,	SPI2_SCLK,	4,   10,    3,    1,	 false)
MUX_CFG(DM365,	SPI2_SDI,	4,   6,     3,    1,	 false)
MUX_CFG(DM365,	SPI2_SDO,	4,   8,     3,    1,	 false)
MUX_CFG(DM365,	SPI2_SDENA0,	4,   12,    3,    1,	 false)
MUX_CFG(DM365,	SPI2_SDENA1,	4,   8,     3,    2,	 false)

MUX_CFG(DM365,	SPI3_SCLK,	0,   0,	    3,    2,	 false)
MUX_CFG(DM365,	SPI3_SDI,	0,   2,     3,    2,	 false)
MUX_CFG(DM365,	SPI3_SDO,	0,   6,     3,    2,	 false)
MUX_CFG(DM365,	SPI3_SDENA0,	0,   4,     3,    2,	 false)
MUX_CFG(DM365,	SPI3_SDENA1,	0,   6,     3,    3,	 false)

MUX_CFG(DM365,	SPI4_SCLK,	4,   18,    3,    1,	 false)
MUX_CFG(DM365,	SPI4_SDI,	4,   14,    3,    1,	 false)
MUX_CFG(DM365,	SPI4_SDO,	4,   16,    3,    1,	 false)
MUX_CFG(DM365,	SPI4_SDENA0,	4,   20,    3,    1,	 false)
MUX_CFG(DM365,	SPI4_SDENA1,	4,   16,    3,    2,	 false)

MUX_CFG(DM365,	CLKOUT0,	4,   20,    3,    3,     false)
MUX_CFG(DM365,	CLKOUT1,	4,   16,    3,    3,     false)
MUX_CFG(DM365,	CLKOUT2,	4,   8,     3,    3,     false)

MUX_CFG(DM365,	GPIO20,		3,   21,    3,    0,	 false)
MUX_CFG(DM365,	GPIO30,		4,   6,     3,	  0,	 false)
MUX_CFG(DM365,	GPIO31,		4,   8,     3,	  0,	 false)
MUX_CFG(DM365,	GPIO32,		4,   10,    3,	  0,	 false)
MUX_CFG(DM365,	GPIO33,		4,   12,    3,	  0,	 false)
MUX_CFG(DM365,	GPIO40,		4,   26,    3,	  0,	 false)
MUX_CFG(DM365,	GPIO64_57,	2,   6,     1,	  0,	 false)

MUX_CFG(DM365,	VOUT_FIELD,	1,   18,    3,	  1,	 false)
MUX_CFG(DM365,	VOUT_FIELD_G81,	1,   18,    3,	  0,	 false)
MUX_CFG(DM365,	VOUT_HVSYNC,	1,   16,    1,	  0,	 false)
MUX_CFG(DM365,	VOUT_COUTL_EN,	1,   0,     0xff, 0x55,  false)
MUX_CFG(DM365,	VOUT_COUTH_EN,	1,   8,     0xff, 0x55,  false)
MUX_CFG(DM365,	VIN_CAM_WEN,	0,   14,    3,	  0,	 false)
MUX_CFG(DM365,	VIN_CAM_VD,	0,   13,    1,	  0,	 false)
MUX_CFG(DM365,	VIN_CAM_HD,	0,   12,    1,	  0,	 false)
MUX_CFG(DM365,	VIN_YIN4_7_EN,	0,   0,     0xff, 0,	 false)
MUX_CFG(DM365,	VIN_YIN0_3_EN,	0,   8,     0xf,  0,	 false)

INT_CFG(DM365,  INT_EDMA_CC,         2,     1,    1,     false)
INT_CFG(DM365,  INT_EDMA_TC0_ERR,    3,     1,    1,     false)
INT_CFG(DM365,  INT_EDMA_TC1_ERR,    4,     1,    1,     false)
INT_CFG(DM365,  INT_EDMA_TC2_ERR,    22,    1,    1,     false)
INT_CFG(DM365,  INT_EDMA_TC3_ERR,    23,    1,    1,     false)
INT_CFG(DM365,  INT_PRTCSS,          10,    1,    1,     false)
INT_CFG(DM365,  INT_EMAC_RXTHRESH,   14,    1,    1,     false)
INT_CFG(DM365,  INT_EMAC_RXPULSE,    15,    1,    1,     false)
INT_CFG(DM365,  INT_EMAC_TXPULSE,    16,    1,    1,     false)
INT_CFG(DM365,  INT_EMAC_MISCPULSE,  17,    1,    1,     false)
INT_CFG(DM365,  INT_IMX0_ENABLE,     0,     1,    0,     false)
INT_CFG(DM365,  INT_IMX0_DISABLE,    0,     1,    1,     false)
INT_CFG(DM365,  INT_HDVICP_ENABLE,   0,     1,    1,     false)
INT_CFG(DM365,  INT_HDVICP_DISABLE,  0,     1,    0,     false)
INT_CFG(DM365,  INT_IMX1_ENABLE,     24,    1,    1,     false)
INT_CFG(DM365,  INT_IMX1_DISABLE,    24,    1,    0,     false)
INT_CFG(DM365,  INT_NSF_ENABLE,      25,    1,    1,     false)
INT_CFG(DM365,  INT_NSF_DISABLE,     25,    1,    0,     false)

EVT_CFG(DM365,	EVT2_ASP_TX,         0,     1,    0,     false)
EVT_CFG(DM365,	EVT3_ASP_RX,         1,     1,    0,     false)
EVT_CFG(DM365,	EVT2_VC_TX,          0,     1,    1,     false)
EVT_CFG(DM365,	EVT3_VC_RX,          1,     1,    1,     false)
#endif
};

static u64 dm365_spi0_dma_mask = DMA_BIT_MASK(32);

static struct davinci_spi_platform_data dm365_spi0_pdata = {
	.version 	= SPI_VERSION_1,
	.num_chipselect = 2,
	.dma_event_q	= EVENTQ_3,
};

static struct resource dm365_spi0_resources[] = {
	{
		.start = 0x01c66000,
		.end   = 0x01c667ff,
		.flags = IORESOURCE_MEM,
	},
	{
		.start = IRQ_DM365_SPIINT0_0,
		.flags = IORESOURCE_IRQ,
	},
	{
		.start = 17,
		.flags = IORESOURCE_DMA,
	},
	{
		.start = 16,
		.flags = IORESOURCE_DMA,
	},
};

static struct platform_device dm365_spi0_device = {
	.name = "spi_davinci",
	.id = 0,
	.dev = {
		.dma_mask = &dm365_spi0_dma_mask,
		.coherent_dma_mask = DMA_BIT_MASK(32),
		.platform_data = &dm365_spi0_pdata,
	},
	.num_resources = ARRAY_SIZE(dm365_spi0_resources),
	.resource = dm365_spi0_resources,
};

void __init dm365_init_spi0(unsigned chipselect_mask,
		const struct spi_board_info *info, unsigned len)
{
	davinci_cfg_reg(DM365_SPI0_SCLK);
	davinci_cfg_reg(DM365_SPI0_SDI);
	davinci_cfg_reg(DM365_SPI0_SDO);

	/* not all slaves will be wired up */
	if (chipselect_mask & BIT(0))
		davinci_cfg_reg(DM365_SPI0_SDENA0);
	if (chipselect_mask & BIT(1))
		davinci_cfg_reg(DM365_SPI0_SDENA1);

	spi_register_board_info(info, len);

	platform_device_register(&dm365_spi0_device);
}

static struct emac_platform_data dm365_emac_pdata = {
	.ctrl_reg_offset	= DM365_EMAC_CNTRL_OFFSET,
	.ctrl_mod_reg_offset	= DM365_EMAC_CNTRL_MOD_OFFSET,
	.ctrl_ram_offset	= DM365_EMAC_CNTRL_RAM_OFFSET,
	.ctrl_ram_size		= DM365_EMAC_CNTRL_RAM_SIZE,
	.version		= EMAC_VERSION_2,
};

static struct resource dm365_emac_resources[] = {
	{
		.start	= DM365_EMAC_BASE,
		.end	= DM365_EMAC_BASE + SZ_16K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.start	= IRQ_DM365_EMAC_RXTHRESH,
		.end	= IRQ_DM365_EMAC_RXTHRESH,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= IRQ_DM365_EMAC_RXPULSE,
		.end	= IRQ_DM365_EMAC_RXPULSE,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= IRQ_DM365_EMAC_TXPULSE,
		.end	= IRQ_DM365_EMAC_TXPULSE,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.start	= IRQ_DM365_EMAC_MISCPULSE,
		.end	= IRQ_DM365_EMAC_MISCPULSE,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device dm365_emac_device = {
	.name		= "davinci_emac",
	.id		= 1,
	.dev = {
		.platform_data	= &dm365_emac_pdata,
	},
	.num_resources	= ARRAY_SIZE(dm365_emac_resources),
	.resource	= dm365_emac_resources,
};

static struct resource dm365_mdio_resources[] = {
	{
		.start	= DM365_EMAC_MDIO_BASE,
		.end	= DM365_EMAC_MDIO_BASE + SZ_4K - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device dm365_mdio_device = {
	.name		= "davinci_mdio",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(dm365_mdio_resources),
	.resource	= dm365_mdio_resources,
};

static u8 dm365_default_priorities[DAVINCI_N_AINTC_IRQ] = {
	[IRQ_VDINT0]			= 2,
	[IRQ_VDINT1]			= 6,
	[IRQ_VDINT2]			= 6,
	[IRQ_HISTINT]			= 6,
	[IRQ_H3AINT]			= 6,
	[IRQ_PRVUINT]			= 6,
	[IRQ_RSZINT]			= 6,
	[IRQ_DM365_INSFINT]		= 7,
	[IRQ_VENCINT]			= 6,
	[IRQ_ASQINT]			= 6,
	[IRQ_IMXINT]			= 6,
	[IRQ_DM365_IMCOPINT]		= 4,
	[IRQ_USBINT]			= 4,
	[IRQ_DM365_RTOINT]		= 7,
	[IRQ_DM365_TINT5]		= 7,
	[IRQ_DM365_TINT6]		= 5,
	[IRQ_CCINT0]			= 5,
	[IRQ_CCERRINT]			= 5,
	[IRQ_TCERRINT0]			= 5,
	[IRQ_TCERRINT]			= 7,
	[IRQ_PSCIN]			= 4,
	[IRQ_DM365_SPINT2_1]		= 7,
	[IRQ_DM365_TINT7]		= 7,
	[IRQ_DM365_SDIOINT0]		= 7,
	[IRQ_MBXINT]			= 7,
	[IRQ_MBRINT]			= 7,
	[IRQ_MMCINT]			= 7,
	[IRQ_DM365_MMCINT1]		= 7,
	[IRQ_DM365_PWMINT3]		= 7,
	[IRQ_AEMIFINT]			= 2,
	[IRQ_DM365_SDIOINT1]		= 2,
	[IRQ_TINT0_TINT12]		= 7,
	[IRQ_TINT0_TINT34]		= 7,
	[IRQ_TINT1_TINT12]		= 7,
	[IRQ_TINT1_TINT34]		= 7,
	[IRQ_PWMINT0]			= 7,
	[IRQ_PWMINT1]			= 3,
	[IRQ_PWMINT2]			= 3,
	[IRQ_I2C]			= 3,
	[IRQ_UARTINT0]			= 3,
	[IRQ_UARTINT1]			= 3,
	[IRQ_DM365_RTCINT]		= 3,
	[IRQ_DM365_SPIINT0_0]		= 3,
	[IRQ_DM365_SPIINT3_0]		= 3,
	[IRQ_DM365_GPIO0]		= 3,
	[IRQ_DM365_GPIO1]		= 7,
	[IRQ_DM365_GPIO2]		= 4,
	[IRQ_DM365_GPIO3]		= 4,
	[IRQ_DM365_GPIO4]		= 7,
	[IRQ_DM365_GPIO5]		= 7,
	[IRQ_DM365_GPIO6]		= 7,
	[IRQ_DM365_GPIO7]		= 7,
	[IRQ_DM365_EMAC_RXTHRESH]	= 7,
	[IRQ_DM365_EMAC_RXPULSE]	= 7,
	[IRQ_DM365_EMAC_TXPULSE]	= 7,
	[IRQ_DM365_EMAC_MISCPULSE]	= 7,
	[IRQ_DM365_GPIO12]		= 7,
	[IRQ_DM365_GPIO13]		= 7,
	[IRQ_DM365_GPIO14]		= 7,
	[IRQ_DM365_GPIO15]		= 7,
	[IRQ_DM365_KEYINT]		= 7,
	[IRQ_DM365_TCERRINT2]		= 7,
	[IRQ_DM365_TCERRINT3]		= 7,
	[IRQ_DM365_EMUINT]		= 7,
};

/* Four Transfer Controllers on DM365 */
static const s8
dm365_queue_tc_mapping[][2] = {
	/* {event queue no, TC no} */
	{0, 0},
	{1, 1},
	{2, 2},
	{3, 3},
	{-1, -1},
};

static const s8
dm365_queue_priority_mapping[][2] = {
	/* {event queue no, Priority} */
	{0, 7},
	{1, 7},
	{2, 7},
	{3, 0},
	{-1, -1},
};

static struct edma_soc_info edma_cc0_info = {
	.n_channel		= 64,
	.n_region		= 4,
	.n_slot			= 256,
	.n_tc			= 4,
	.n_cc			= 1,
	.queue_tc_mapping	= dm365_queue_tc_mapping,
	.queue_priority_mapping	= dm365_queue_priority_mapping,
	.default_queue		= EVENTQ_3,
};

static struct edma_soc_info *dm365_edma_info[EDMA_MAX_CC] = {
	&edma_cc0_info,
};

static struct resource edma_resources[] = {
	{
		.name	= "edma_cc0",
		.start	= 0x01c00000,
		.end	= 0x01c00000 + SZ_64K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "edma_tc0",
		.start	= 0x01c10000,
		.end	= 0x01c10000 + SZ_1K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "edma_tc1",
		.start	= 0x01c10400,
		.end	= 0x01c10400 + SZ_1K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "edma_tc2",
		.start	= 0x01c10800,
		.end	= 0x01c10800 + SZ_1K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "edma_tc3",
		.start	= 0x01c10c00,
		.end	= 0x01c10c00 + SZ_1K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "edma0",
		.start	= IRQ_CCINT0,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.name	= "edma0_err",
		.start	= IRQ_CCERRINT,
		.flags	= IORESOURCE_IRQ,
	},
	/* not using TC*_ERR */
};

static struct platform_device dm365_edma_device = {
	.name			= "edma",
	.id			= 0,
	.dev.platform_data	= dm365_edma_info,
	.num_resources		= ARRAY_SIZE(edma_resources),
	.resource		= edma_resources,
};

static struct resource dm365_asp_resources[] = {
	{
		.start	= DAVINCI_DM365_ASP0_BASE,
		.end	= DAVINCI_DM365_ASP0_BASE + SZ_8K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.start	= DAVINCI_DMA_ASP0_TX,
		.end	= DAVINCI_DMA_ASP0_TX,
		.flags	= IORESOURCE_DMA,
	},
	{
		.start	= DAVINCI_DMA_ASP0_RX,
		.end	= DAVINCI_DMA_ASP0_RX,
		.flags	= IORESOURCE_DMA,
	},
};

static struct platform_device dm365_asp_device = {
	.name		= "davinci-mcbsp",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(dm365_asp_resources),
	.resource	= dm365_asp_resources,
};

static struct resource dm365_vc_resources[] = {
	{
		.start	= DAVINCI_DM365_VC_BASE,
		.end	= DAVINCI_DM365_VC_BASE + SZ_1K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.start	= DAVINCI_DMA_VC_TX,
		.end	= DAVINCI_DMA_VC_TX,
		.flags	= IORESOURCE_DMA,
	},
	{
		.start	= DAVINCI_DMA_VC_RX,
		.end	= DAVINCI_DMA_VC_RX,
		.flags	= IORESOURCE_DMA,
	},
};

static struct platform_device dm365_vc_device = {
	.name		= "davinci_voicecodec",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(dm365_vc_resources),
	.resource	= dm365_vc_resources,
};

static struct resource dm365_rtc_resources[] = {
	{
		.start = DM365_RTC_BASE,
		.end = DM365_RTC_BASE + SZ_1K - 1,
		.flags = IORESOURCE_MEM,
	},
	{
		.start = IRQ_DM365_RTCINT,
		.flags = IORESOURCE_IRQ,
	},
};

static struct platform_device dm365_rtc_device = {
	.name = "rtc_davinci",
	.id = 0,
	.num_resources = ARRAY_SIZE(dm365_rtc_resources),
	.resource = dm365_rtc_resources,
};

static struct map_desc dm365_io_desc[] = {
	{
		.virtual	= IO_VIRT,
		.pfn		= __phys_to_pfn(IO_PHYS),
		.length		= IO_SIZE,
		.type		= MT_DEVICE
	},
};

static struct resource dm365_ks_resources[] = {
	{
		/* registers */
		.start = DM365_KEYSCAN_BASE,
		.end = DM365_KEYSCAN_BASE + SZ_1K - 1,
		.flags = IORESOURCE_MEM,
	},
	{
		/* interrupt */
		.start = IRQ_DM365_KEYINT,
		.end = IRQ_DM365_KEYINT,
		.flags = IORESOURCE_IRQ,
	},
};

static struct platform_device dm365_ks_device = {
	.name		= "davinci_keyscan",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(dm365_ks_resources),
	.resource	= dm365_ks_resources,
};

/* Contents of JTAG ID register used to identify exact cpu type */
static struct davinci_id dm365_ids[] = {
	{
		.variant	= 0x0,
		.part_no	= 0xb83e,
		.manufacturer	= 0x017,
		.cpu_id		= DAVINCI_CPU_ID_DM365,
		.name		= "dm365_rev1.1",
	},
	{
		.variant	= 0x8,
		.part_no	= 0xb83e,
		.manufacturer	= 0x017,
		.cpu_id		= DAVINCI_CPU_ID_DM365,
		.name		= "dm365_rev1.2",
	},
};

static u32 dm365_psc_bases[] = { DAVINCI_PWR_SLEEP_CNTRL_BASE };

static struct davinci_timer_info dm365_timer_info = {
	.timers		= davinci_timer_instance,
	.clockevent_id	= T0_BOT,
	.clocksource_id	= T0_TOP,
};

#define DM365_UART1_BASE	(IO_PHYS + 0x106000)

static struct plat_serial8250_port dm365_serial_platform_data[] = {
	{
		.mapbase	= DAVINCI_UART0_BASE,
		.irq		= IRQ_UARTINT0,
		.flags		= UPF_BOOT_AUTOCONF | UPF_SKIP_TEST |
				  UPF_IOREMAP,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
	},
	{
		.mapbase	= DM365_UART1_BASE,
		.irq		= IRQ_UARTINT1,
		.flags		= UPF_BOOT_AUTOCONF | UPF_SKIP_TEST |
				  UPF_IOREMAP,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
	},
	{
		.flags		= 0
	},
};

static struct platform_device dm365_serial_device = {
	.name			= "serial8250",
	.id			= PLAT8250_DEV_PLATFORM,
	.dev			= {
		.platform_data	= dm365_serial_platform_data,
	},
};

static struct davinci_soc_info davinci_soc_info_dm365 = {
	.io_desc		= dm365_io_desc,
	.io_desc_num		= ARRAY_SIZE(dm365_io_desc),
	.jtag_id_reg		= 0x01c40028,
	.ids			= dm365_ids,
	.ids_num		= ARRAY_SIZE(dm365_ids),
	.cpu_clks		= dm365_clks,
	.psc_bases		= dm365_psc_bases,
	.psc_bases_num		= ARRAY_SIZE(dm365_psc_bases),
	.pinmux_base		= DAVINCI_SYSTEM_MODULE_BASE,
	.pinmux_pins		= dm365_pins,
	.pinmux_pins_num	= ARRAY_SIZE(dm365_pins),
	.intc_base		= DAVINCI_ARM_INTC_BASE,
	.intc_type		= DAVINCI_INTC_TYPE_AINTC,
	.intc_irq_prios		= dm365_default_priorities,
	.intc_irq_num		= DAVINCI_N_AINTC_IRQ,
	.timer_info		= &dm365_timer_info,
	.gpio_type		= GPIO_TYPE_DAVINCI,
	.gpio_base		= DAVINCI_GPIO_BASE,
	.gpio_num		= 104,
	.gpio_irq		= IRQ_DM365_GPIO0,
	.gpio_unbanked		= 8,	/* really 16 ... skip muxed GPIOs */
	.serial_dev		= &dm365_serial_device,
	.emac_pdata		= &dm365_emac_pdata,
	.sram_dma		= 0x00010000,
	.sram_len		= SZ_32K,
};

void __init dm365_init_asp(struct snd_platform_data *pdata)
{
	davinci_cfg_reg(DM365_MCBSP0_BDX);
	davinci_cfg_reg(DM365_MCBSP0_X);
	davinci_cfg_reg(DM365_MCBSP0_BFSX);
	davinci_cfg_reg(DM365_MCBSP0_BDR);
	davinci_cfg_reg(DM365_MCBSP0_R);
	davinci_cfg_reg(DM365_MCBSP0_BFSR);
	davinci_cfg_reg(DM365_EVT2_ASP_TX);
	davinci_cfg_reg(DM365_EVT3_ASP_RX);
	dm365_asp_device.dev.platform_data = pdata;
	platform_device_register(&dm365_asp_device);
}

void __init dm365_init_vc(struct snd_platform_data *pdata)
{
	davinci_cfg_reg(DM365_EVT2_VC_TX);
	davinci_cfg_reg(DM365_EVT3_VC_RX);
	dm365_vc_device.dev.platform_data = pdata;
	platform_device_register(&dm365_vc_device);
}

void __init dm365_init_ks(struct davinci_ks_platform_data *pdata)
{
	dm365_ks_device.dev.platform_data = pdata;
	platform_device_register(&dm365_ks_device);
}

void __init dm365_init_rtc(void)
{
	davinci_cfg_reg(DM365_INT_PRTCSS);
	platform_device_register(&dm365_rtc_device);
}

void __init dm365_init(void)
{
	davinci_common_init(&davinci_soc_info_dm365);
	davinci_map_sysmod();
}

static struct resource dm365_vpss_resources[] = {
	{
		/* VPSS ISP5 Base address */
		.name           = "isp5",
		.start          = 0x01c70000,
		.end            = 0x01c70000 + 0xff,
		.flags          = IORESOURCE_MEM,
	},
	{
		/* VPSS CLK Base address */
		.name           = "vpss",
		.start          = 0x01c70200,
		.end            = 0x01c70200 + 0xff,
		.flags          = IORESOURCE_MEM,
	},
};

static struct platform_device dm365_vpss_device = {
       .name                   = "vpss",
       .id                     = -1,
       .dev.platform_data      = "dm365_vpss",
       .num_resources          = ARRAY_SIZE(dm365_vpss_resources),
       .resource               = dm365_vpss_resources,
};

static struct resource vpfe_resources[] = {
	{
		.start          = IRQ_VDINT0,
		.end            = IRQ_VDINT0,
		.flags          = IORESOURCE_IRQ,
	},
	{
		.start          = IRQ_VDINT1,
		.end            = IRQ_VDINT1,
		.flags          = IORESOURCE_IRQ,
	},
};

static u64 vpfe_capture_dma_mask = DMA_BIT_MASK(32);
static struct platform_device vpfe_capture_dev = {
	.name           = CAPTURE_DRV_NAME,
	.id             = -1,
	.num_resources  = ARRAY_SIZE(vpfe_resources),
	.resource       = vpfe_resources,
	.dev = {
		.dma_mask               = &vpfe_capture_dma_mask,
		.coherent_dma_mask      = DMA_BIT_MASK(32),
	},
};

static void dm365_isif_setup_pinmux(void)
{
	davinci_cfg_reg(DM365_VIN_CAM_WEN);
	davinci_cfg_reg(DM365_VIN_CAM_VD);
	davinci_cfg_reg(DM365_VIN_CAM_HD);
	davinci_cfg_reg(DM365_VIN_YIN4_7_EN);
	davinci_cfg_reg(DM365_VIN_YIN0_3_EN);
}

static struct resource isif_resource[] = {
	/* ISIF Base address */
	{
		.start          = 0x01c71000,
		.end            = 0x01c71000 + 0x1ff,
		.flags          = IORESOURCE_MEM,
	},
	/* ISIF Linearization table 0 */
	{
		.start          = 0x1C7C000,
		.end            = 0x1C7C000 + 0x2ff,
		.flags          = IORESOURCE_MEM,
	},
	/* ISIF Linearization table 1 */
	{
		.start          = 0x1C7C400,
		.end            = 0x1C7C400 + 0x2ff,
		.flags          = IORESOURCE_MEM,
	},
};
static struct platform_device dm365_isif_dev = {
	.name           = "isif",
	.id             = -1,
	.num_resources  = ARRAY_SIZE(isif_resource),
	.resource       = isif_resource,
	.dev = {
		.dma_mask               = &vpfe_capture_dma_mask,
		.coherent_dma_mask      = DMA_BIT_MASK(32),
		.platform_data		= dm365_isif_setup_pinmux,
	},
};

static struct resource dm365_osd_resources[] = {
	{
		.start = DM365_OSD_BASE,
		.end   = DM365_OSD_BASE + 0xff,
		.flags = IORESOURCE_MEM,
	},
};

static u64 dm365_video_dma_mask = DMA_BIT_MASK(32);

static struct platform_device dm365_osd_dev = {
	.name		= DM365_VPBE_OSD_SUBDEV_NAME,
	.id		= -1,
	.num_resources	= ARRAY_SIZE(dm365_osd_resources),
	.resource	= dm365_osd_resources,
	.dev		= {
		.dma_mask		= &dm365_video_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
	},
};

static struct resource dm365_venc_resources[] = {
	{
		.start = IRQ_VENCINT,
		.end   = IRQ_VENCINT,
		.flags = IORESOURCE_IRQ,
	},
	/* venc registers io space */
	{
		.start = DM365_VENC_BASE,
		.end   = DM365_VENC_BASE + 0x177,
		.flags = IORESOURCE_MEM,
	},
	/* vdaccfg registers io space */
	{
		.start = DAVINCI_SYSTEM_MODULE_BASE + SYSMOD_VDAC_CONFIG,
		.end   = DAVINCI_SYSTEM_MODULE_BASE + SYSMOD_VDAC_CONFIG + 3,
		.flags = IORESOURCE_MEM,
	},
};

static struct resource dm365_v4l2_disp_resources[] = {
	{
		.start = IRQ_VENCINT,
		.end   = IRQ_VENCINT,
		.flags = IORESOURCE_IRQ,
	},
	/* venc registers io space */
	{
		.start = DM365_VENC_BASE,
		.end   = DM365_VENC_BASE + 0x177,
		.flags = IORESOURCE_MEM,
	},
};

static int dm365_vpbe_setup_pinmux(enum v4l2_mbus_pixelcode if_type,
			    int field)
{
	switch (if_type) {
	case V4L2_MBUS_FMT_SGRBG8_1X8:
		davinci_cfg_reg(DM365_VOUT_FIELD_G81);
		davinci_cfg_reg(DM365_VOUT_COUTL_EN);
		davinci_cfg_reg(DM365_VOUT_COUTH_EN);
		break;
	case V4L2_MBUS_FMT_YUYV10_1X20:
		if (field)
			davinci_cfg_reg(DM365_VOUT_FIELD);
		else
			davinci_cfg_reg(DM365_VOUT_FIELD_G81);
		davinci_cfg_reg(DM365_VOUT_COUTL_EN);
		davinci_cfg_reg(DM365_VOUT_COUTH_EN);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int dm365_venc_setup_clock(enum vpbe_enc_timings_type type,
				  unsigned int pclock)
{
	void __iomem *vpss_clkctl_reg;
	u32 val;

	vpss_clkctl_reg = DAVINCI_SYSMOD_VIRT(SYSMOD_VPSS_CLKCTL);

	switch (type) {
	case VPBE_ENC_STD:
		val = VPSS_VENCCLKEN_ENABLE | VPSS_DACCLKEN_ENABLE;
		break;
	case VPBE_ENC_DV_TIMINGS:
		if (pclock <= 27000000) {
			val = VPSS_VENCCLKEN_ENABLE | VPSS_DACCLKEN_ENABLE;
		} else {
			/* set sysclk4 to output 74.25 MHz from pll1 */
			val = VPSS_PLLC2SYSCLK5_ENABLE | VPSS_DACCLKEN_ENABLE |
			      VPSS_VENCCLKEN_ENABLE;
		}
		break;
	default:
		return -EINVAL;
	}
	writel(val, vpss_clkctl_reg);

	return 0;
}

static struct platform_device dm365_vpbe_display = {
	.name		= "vpbe-v4l2",
	.id		= -1,
	.num_resources  = ARRAY_SIZE(dm365_v4l2_disp_resources),
	.resource	= dm365_v4l2_disp_resources,
	.dev		= {
		.dma_mask		= &dm365_video_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
	},
};

struct venc_platform_data dm365_venc_pdata = {
	.setup_pinmux	= dm365_vpbe_setup_pinmux,
	.setup_clock	= dm365_venc_setup_clock,
};

static struct platform_device dm365_venc_dev = {
	.name		= DM365_VPBE_VENC_SUBDEV_NAME,
	.id		= -1,
	.num_resources	= ARRAY_SIZE(dm365_venc_resources),
	.resource	= dm365_venc_resources,
	.dev		= {
		.dma_mask		= &dm365_video_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
		.platform_data		= (void *)&dm365_venc_pdata,
	},
};

static struct platform_device dm365_vpbe_dev = {
	.name		= "vpbe_controller",
	.id		= -1,
	.dev		= {
		.dma_mask		= &dm365_video_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
	},
};

int __init dm365_init_video(struct vpfe_config *vpfe_cfg,
				struct vpbe_config *vpbe_cfg)
{
	if (vpfe_cfg || vpbe_cfg)
		platform_device_register(&dm365_vpss_device);

	if (vpfe_cfg) {
		vpfe_capture_dev.dev.platform_data = vpfe_cfg;
		platform_device_register(&dm365_isif_dev);
		platform_device_register(&vpfe_capture_dev);
	}
	if (vpbe_cfg) {
		dm365_vpbe_dev.dev.platform_data = vpbe_cfg;
		platform_device_register(&dm365_osd_dev);
		platform_device_register(&dm365_venc_dev);
		platform_device_register(&dm365_vpbe_dev);
		platform_device_register(&dm365_vpbe_display);
	}

	return 0;
}

static int __init dm365_init_devices(void)
{
	if (!cpu_is_davinci_dm365())
		return 0;

	davinci_cfg_reg(DM365_INT_EDMA_CC);
	platform_device_register(&dm365_edma_device);

	platform_device_register(&dm365_mdio_device);
	platform_device_register(&dm365_emac_device);
	clk_add_alias(NULL, dev_name(&dm365_mdio_device.dev),
		      NULL, &dm365_emac_device.dev);

	return 0;
}
postcore_initcall(dm365_init_devices);
