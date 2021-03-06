/*
 * Copyright (C) 2012 - 2014 Texas Instruments Incorporated
 * Authors: Sandeep Paulraj <s-paulraj@ti.com>
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

#include <linux/io.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/phy.h>
#include <linux/timer.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/if_vlan.h>
#include <linux/of_mdio.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/net_tstamp.h>
#include <linux/netdevice.h>
#include <linux/interrupt.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/ptp_classify.h>
#include <linux/atomic.h>

#include "cpsw_ale.h"
#include "keystone_net.h"
#include "cpts.h"
#include "keystone_serdes.h"

#define NETCP_DRIVER_NAME	"TI KeyStone Ethernet Driver"
#define NETCP_DRIVER_VERSION	"v1.2.2"

#define CPSW_MODULE_NAME	"keystone-cpsw"

#define CPSW_SGMII_IDENT(reg)		((reg >> 16) & 0xffff)
#define CPSW_MAJOR_VERSION(reg)		(reg >> 8 & 0x7)
#define CPSW_MINOR_VERSION(reg)		(reg & 0xff)
#define CPSW_RTL_VERSION(reg)		((reg >> 11) & 0x1f)

#define TCI6614_SS_BASE				0x02090000
#define DEVICE_N_GMACSL_PORTS			2
#define DEVICE_EMACSL_RESET_POLL_COUNT		100

#define	CPSW_TIMER_INTERVAL			(HZ / 10)

/* Soft reset register values */
#define SOFT_RESET_MASK				BIT(0)
#define SOFT_RESET				BIT(0)

#define MACSL_RX_ENABLE_CSF			BIT(23)
#define MACSL_RX_ENABLE_EXT_CTL			BIT(18)
#define MACSL_ENABLE				BIT(5)
#define MACSL_DEFAULT_CONFIG			(MACSL_ENABLE |\
						 MACSL_RX_ENABLE_EXT_CTL |\
						 MACSL_RX_ENABLE_CSF)
#define GMACSL_RET_WARN_RESET_INCOMPLETE	-2

#define CPSW_NUM_PORTS		                5
#define CPSW_CTL_P0_ENABLE			BIT(2)
#define CPSW_CTL_VLAN_AWARE			BIT(1)
#define CPSW_REG_VAL_STAT_ENABLE_ALL		0xff
#define CPSW_STATS_CD_SEL			BIT(28)

#define CPSW_MASK_ALL_PORTS			0x1f
#define CPSW_MASK_PHYS_PORTS			0x1e
#define CPSW_MASK_NO_PORTS			0

#define CPSW_STATSA_MODULE			0
#define CPSW_STATSB_MODULE			1
#define CPSW_STATSC_MODULE			2
#define CPSW_STATSD_MODULE			3

#define MAX_SIZE_STREAM_BUFFER		        9504

/* Px_TS_CTL register */
#define CPSW_TS_RX_ANX_F_EN			BIT(0)
#define CPSW_TS_RX_VLAN_LT1_EN			BIT(1)
#define CPSW_TS_RX_VLAN_LT2_EN			BIT(2)
#define CPSW_TS_RX_ANX_D_EN			BIT(3)
#define CPSW_TS_TX_ANX_F_EN			BIT(4)
#define CPSW_TS_TX_VLAN_LT1_EN			BIT(5)
#define CPSW_TS_TX_VLAN_LT2_EN			BIT(6)
#define CPSW_TS_TX_ANX_D_EN			BIT(7)
#define CPSW_TS_LT2_EN				BIT(8)
#define CPSW_TS_RX_ANX_E_EN			BIT(9)
#define CPSW_TS_TX_ANX_E_EN			BIT(10)
#define CPSW_TS_MSG_TYPE_EN_SHIFT		16
#define CPSW_TS_MSG_TYPE_EN_MASK		0xffff

/* Px_TS_SEQ_LTYPE */
#define CPSW_TS_LTYPE1_SHIFT			0
#define CPSW_TS_LTYPE1_MASK			0xffff
#define CPSW_TS_SEQ_ID_OFS_SHIFT		16
#define CPSW_TS_SEQ_ID_OFS_MASK			0x3f

/* Px_TS_VLAN_LTYPE */
#define CPSW_TS_VLAN_LTYPE1_SHIFT		0
#define CPSW_TS_VLAN_LTYPE1_MASK		0xffff
#define CPSW_TS_VLAN_LTYPE2_SHIFT		16
#define CPSW_TS_VLAN_LTYPE2_MASK		0xffff

/* Px_TS_CTL_LTYPE2 */
#define CPSW_TS_LTYPE2_SHIFT			0
#define CPSW_TS_LTYPE2_MASK			0xffff
#define CPSW_TS_107				BIT(16)
#define CPSW_TS_129				BIT(17)
#define CPSW_TS_130				BIT(18)
#define CPSW_TS_131				BIT(19)
#define CPSW_TS_132				BIT(20)
#define CPSW_TS_319				BIT(21)
#define CPSW_TS_320				BIT(22)
#define CPSW_TS_TTL_NONZERO			BIT(23)
#define CPSW_TS_UNI_EN				BIT(24)
#define CPSW_TS_UNI_EN_SHIFT			24

/* Px_TS_CTL2 */
#define CPSW_TS_MCAST_TYPE_EN_SHIFT		0
#define CPSW_TS_MCAST_TYPE_EN_MASK		0xff
#define CPSW_TS_DOMAIN_OFFSET_SHIFT		16
#define CPSW_TS_DOMAIN_OFFSET_MASK		0x3f

#define CPSW_TS_TX_ANX_ALL_EN		 \
		(CPSW_TS_TX_ANX_D_EN	|\
		 CPSW_TS_TX_ANX_E_EN	|\
		 CPSW_TS_TX_ANX_F_EN)


#define CPSW_TS_RX_ANX_ALL_EN		 \
		(CPSW_TS_RX_ANX_D_EN	|\
		 CPSW_TS_RX_ANX_E_EN	|\
		 CPSW_TS_RX_ANX_F_EN)

#define CPSW_TS_CTL_DST_PORT		(CPSW_TS_319)
#define CPSW_TS_CTL_DST_PORT_SHIFT	21

#define CPSW_TS_CTL_MADDR_ALL	\
		(CPSW_TS_107 | CPSW_TS_129 | CPSW_TS_130 | \
		 CPSW_TS_131 | CPSW_TS_132)

#define CPSW_TS_CTL_MADDR_SHIFT		16

/* The PTP event messages - Sync, Delay_Req, Pdelay_Req, and Pdelay_Resp. */
#define EVENT_MSG_BITS ((1<<0) | (1<<1) | (1<<2) | (1<<3))

#define MAX_SLAVES			4

/* s: 0-based slave_num */
#define SGMII_BASE(s) \
	(((s) < 2) ? cpsw_dev->sgmii_port_regs : cpsw_dev->sgmii_port34_regs)

#define IS_SGMII_MAC_PHY(i) \
	(((i) == SGMII_LINK_MAC_PHY) || ((i) == SGMII_LINK_MAC_PHY_MASTER))

/* CPSW SERDES */
#define CPSW_SERDES_MAX_NUM		1
#define CPSW_LANE_NUM_PER_SERDES	4

struct cpts_port_ts_ctl {
	int	uni;
	u8	dst_port_map;
	u8	maddr_map;
	u8	ts_mcast_type;
};

/* slave_num: 0-based
 *  port_num: 1-based
 */
struct cpsw_slave {
	struct cpsw_slave_regs __iomem	*regs;
	struct cpsw_sliver_regs __iomem	*sliver;
	int				 slave_num;
	int				 port_num;
	u32				 mac_control;
	struct phy_device		*phy;
	const char			*phy_id;
	struct cpsw_ale			*ale;
	u32				 link_interface;
	u8				 phy_port_t;
	struct cpts_port_ts_ctl		 ts_ctl;
};

struct cpsw_ss_regs {
	u32	id_ver;
	u32	soft_reset;
	u32	control;
	u32	int_control;
	u32	rx_thresh_en;
	u32	rx_en;
	u32	tx_en;
	u32	misc_en;
	u32	mem_allign1[8];
	u32	rx_thresh_stat;
	u32	rx_stat;
	u32	tx_stat;
	u32	misc_stat;
	u32	mem_allign2[8];
	u32	rx_imax;
	u32	tx_imax;
};

struct cpsw_regs {
	u32	id_ver;
	u32	control;
	u32	soft_reset;
	u32	stat_port_en;
	u32	ptype;
	u32	soft_idle;
	u32	thru_rate;
	u32	gap_thresh;
	u32	tx_start_wds;
	u32	flow_control;
};

struct cpsw_slave_regs {
	u32	max_blks;
	u32	blk_cnt;
	u32	port_vlan;
	u32	tx_pri_map;
	u32	sa_lo;
	u32	sa_hi;
	u32	ts_ctl;
	u32	ts_seq_ltype;
	u32	ts_vlan;
	u32	ts_ctl_ltype2;
	u32	ts_ctl2;
};

struct cpsw_host_regs {
	u32	src_id;
	u32	port_vlan;
	u32	rx_pri_map;
	u32	rx_maxlen;
};

struct cpsw_sliver_regs {
	u32	id_ver;
	u32	mac_control;
	u32	mac_status;
	u32	soft_reset;
	u32	rx_maxlen;
	u32	__reserved_0;
	u32	rx_pause;
	u32	tx_pause;
	u32	__reserved_1;
	u32	rx_pri_map;
	u32	rsvd[6];
};

struct cpsw_hw_stats {
	u32	rx_good_frames;
	u32	rx_broadcast_frames;
	u32	rx_multicast_frames;
	u32	rx_pause_frames;
	u32	rx_crc_errors;
	u32	rx_align_code_errors;
	u32	rx_oversized_frames;
	u32	rx_jabber_frames;
	u32	rx_undersized_frames;
	u32	rx_fragments;
	u32	__pad_0[2];
	u32	rx_bytes;
	u32	tx_good_frames;
	u32	tx_broadcast_frames;
	u32	tx_multicast_frames;
	u32	tx_pause_frames;
	u32	tx_deferred_frames;
	u32	tx_collision_frames;
	u32	tx_single_coll_frames;
	u32	tx_mult_coll_frames;
	u32	tx_excessive_collisions;
	u32	tx_late_collisions;
	u32	tx_underrun;
	u32	tx_carrier_sense_errors;
	u32	tx_bytes;
	u32	tx_64byte_frames;
	u32	tx_65_to_127byte_frames;
	u32	tx_128_to_255byte_frames;
	u32	tx_256_to_511byte_frames;
	u32	tx_512_to_1023byte_frames;
	u32	tx_1024byte_frames;
	u32	net_bytes;
	u32	rx_sof_overruns;
	u32	rx_mof_overruns;
	u32	rx_dma_overruns;
};

struct cpsw_ale_regs {
	u32	ale_idver;
	u32	rsvd0;
	u32	ale_control;
	u32	rsvd1;
	u32	ale_prescale;
	u32	rsvd2;
	u32	ale_unknown_vlan;
	u32	rsvd3;
	u32	ale_tblctl;
	u32	rsvd4[4];
	u32	ale_tblw2;
	u32	ale_tblw1;
	u32	ale_tblw0;
	u32	ale_portctl[6];
};

/*
 * Statistic management
 */
struct netcp_ethtool_stat {
	char desc[ETH_GSTRING_LEN];
	int type;
	u32 size;
	int offset;
};

#define for_each_slave(priv, func, arg...)				\
	do {								\
		int idx, port;						\
		port = (priv)->slave_port;				\
		if ((priv)->multi_if)					\
			(func)((priv)->slaves, ##arg);			\
		else							\
			for (idx = 0; idx < (priv)->num_slaves; idx++)	\
				(func)((priv)->slaves + idx, ##arg);	\
	} while (0)

#define FIELDINFO(_struct, field)       FIELD_SIZEOF(_struct, field),	\
		                                offsetof(_struct, field)
#define CPSW_STATSA_INFO(field) 	"CPSW_A:"#field, CPSW_STATSA_MODULE,\
					FIELDINFO(struct cpsw_hw_stats,\
						field)
#define CPSW_STATSB_INFO(field) 	"CPSW_B:"#field, CPSW_STATSB_MODULE,\
					FIELDINFO(struct cpsw_hw_stats,\
						field)
#define CPSW_STATSC_INFO(field)		"CPSW_C:"#field, CPSW_STATSC_MODULE,\
					FIELDINFO(struct cpsw_hw_stats,\
						field)
#define CPSW_STATSD_INFO(field)		"CPSW_D:"#field, CPSW_STATSD_MODULE,\
					FIELDINFO(struct cpsw_hw_stats,\
						field)

static const struct netcp_ethtool_stat et_stats[] = {
	/* CPSW module A */
	{CPSW_STATSA_INFO(rx_good_frames)},
	{CPSW_STATSA_INFO(rx_broadcast_frames)},
	{CPSW_STATSA_INFO(rx_multicast_frames)},
	{CPSW_STATSA_INFO(rx_pause_frames)},
	{CPSW_STATSA_INFO(rx_crc_errors)},
	{CPSW_STATSA_INFO(rx_align_code_errors)},
	{CPSW_STATSA_INFO(rx_oversized_frames)},
	{CPSW_STATSA_INFO(rx_jabber_frames)},
	{CPSW_STATSA_INFO(rx_undersized_frames)},
	{CPSW_STATSA_INFO(rx_fragments)},
	{CPSW_STATSA_INFO(rx_bytes)},
	{CPSW_STATSA_INFO(tx_good_frames)},
	{CPSW_STATSA_INFO(tx_broadcast_frames)},
	{CPSW_STATSA_INFO(tx_multicast_frames)},
	{CPSW_STATSA_INFO(tx_pause_frames)},
	{CPSW_STATSA_INFO(tx_deferred_frames)},
	{CPSW_STATSA_INFO(tx_collision_frames)},
	{CPSW_STATSA_INFO(tx_single_coll_frames)},
	{CPSW_STATSA_INFO(tx_mult_coll_frames)},
	{CPSW_STATSA_INFO(tx_excessive_collisions)},
	{CPSW_STATSA_INFO(tx_late_collisions)},
	{CPSW_STATSA_INFO(tx_underrun)},
	{CPSW_STATSA_INFO(tx_carrier_sense_errors)},
	{CPSW_STATSA_INFO(tx_bytes)},
	{CPSW_STATSA_INFO(tx_64byte_frames)},
	{CPSW_STATSA_INFO(tx_65_to_127byte_frames)},
	{CPSW_STATSA_INFO(tx_128_to_255byte_frames)},
	{CPSW_STATSA_INFO(tx_256_to_511byte_frames)},
	{CPSW_STATSA_INFO(tx_512_to_1023byte_frames)},
	{CPSW_STATSA_INFO(tx_1024byte_frames)},
	{CPSW_STATSA_INFO(net_bytes)},
	{CPSW_STATSA_INFO(rx_sof_overruns)},
	{CPSW_STATSA_INFO(rx_mof_overruns)},
	{CPSW_STATSA_INFO(rx_dma_overruns)},
	/* CPSW module B */
	{CPSW_STATSB_INFO(rx_good_frames)},
	{CPSW_STATSB_INFO(rx_broadcast_frames)},
	{CPSW_STATSB_INFO(rx_multicast_frames)},
	{CPSW_STATSB_INFO(rx_pause_frames)},
	{CPSW_STATSB_INFO(rx_crc_errors)},
	{CPSW_STATSB_INFO(rx_align_code_errors)},
	{CPSW_STATSB_INFO(rx_oversized_frames)},
	{CPSW_STATSB_INFO(rx_jabber_frames)},
	{CPSW_STATSB_INFO(rx_undersized_frames)},
	{CPSW_STATSB_INFO(rx_fragments)},
	{CPSW_STATSB_INFO(rx_bytes)},
	{CPSW_STATSB_INFO(tx_good_frames)},
	{CPSW_STATSB_INFO(tx_broadcast_frames)},
	{CPSW_STATSB_INFO(tx_multicast_frames)},
	{CPSW_STATSB_INFO(tx_pause_frames)},
	{CPSW_STATSB_INFO(tx_deferred_frames)},
	{CPSW_STATSB_INFO(tx_collision_frames)},
	{CPSW_STATSB_INFO(tx_single_coll_frames)},
	{CPSW_STATSB_INFO(tx_mult_coll_frames)},
	{CPSW_STATSB_INFO(tx_excessive_collisions)},
	{CPSW_STATSB_INFO(tx_late_collisions)},
	{CPSW_STATSB_INFO(tx_underrun)},
	{CPSW_STATSB_INFO(tx_carrier_sense_errors)},
	{CPSW_STATSB_INFO(tx_bytes)},
	{CPSW_STATSB_INFO(tx_64byte_frames)},
	{CPSW_STATSB_INFO(tx_65_to_127byte_frames)},
	{CPSW_STATSB_INFO(tx_128_to_255byte_frames)},
	{CPSW_STATSB_INFO(tx_256_to_511byte_frames)},
	{CPSW_STATSB_INFO(tx_512_to_1023byte_frames)},
	{CPSW_STATSB_INFO(tx_1024byte_frames)},
	{CPSW_STATSB_INFO(net_bytes)},
	{CPSW_STATSB_INFO(rx_sof_overruns)},
	{CPSW_STATSB_INFO(rx_mof_overruns)},
	{CPSW_STATSB_INFO(rx_dma_overruns)},
	/* CPSW module C */
	{CPSW_STATSC_INFO(rx_good_frames)},
	{CPSW_STATSC_INFO(rx_broadcast_frames)},
	{CPSW_STATSC_INFO(rx_multicast_frames)},
	{CPSW_STATSC_INFO(rx_pause_frames)},
	{CPSW_STATSC_INFO(rx_crc_errors)},
	{CPSW_STATSC_INFO(rx_align_code_errors)},
	{CPSW_STATSC_INFO(rx_oversized_frames)},
	{CPSW_STATSC_INFO(rx_jabber_frames)},
	{CPSW_STATSC_INFO(rx_undersized_frames)},
	{CPSW_STATSC_INFO(rx_fragments)},
	{CPSW_STATSC_INFO(rx_bytes)},
	{CPSW_STATSC_INFO(tx_good_frames)},
	{CPSW_STATSC_INFO(tx_broadcast_frames)},
	{CPSW_STATSC_INFO(tx_multicast_frames)},
	{CPSW_STATSC_INFO(tx_pause_frames)},
	{CPSW_STATSC_INFO(tx_deferred_frames)},
	{CPSW_STATSC_INFO(tx_collision_frames)},
	{CPSW_STATSC_INFO(tx_single_coll_frames)},
	{CPSW_STATSC_INFO(tx_mult_coll_frames)},
	{CPSW_STATSC_INFO(tx_excessive_collisions)},
	{CPSW_STATSC_INFO(tx_late_collisions)},
	{CPSW_STATSC_INFO(tx_underrun)},
	{CPSW_STATSC_INFO(tx_carrier_sense_errors)},
	{CPSW_STATSC_INFO(tx_bytes)},
	{CPSW_STATSC_INFO(tx_64byte_frames)},
	{CPSW_STATSC_INFO(tx_65_to_127byte_frames)},
	{CPSW_STATSC_INFO(tx_128_to_255byte_frames)},
	{CPSW_STATSC_INFO(tx_256_to_511byte_frames)},
	{CPSW_STATSC_INFO(tx_512_to_1023byte_frames)},
	{CPSW_STATSC_INFO(tx_1024byte_frames)},
	{CPSW_STATSC_INFO(net_bytes)},
	{CPSW_STATSC_INFO(rx_sof_overruns)},
	{CPSW_STATSC_INFO(rx_mof_overruns)},
	{CPSW_STATSC_INFO(rx_dma_overruns)},
	/* CPSW module D */
	{CPSW_STATSD_INFO(rx_good_frames)},
	{CPSW_STATSD_INFO(rx_broadcast_frames)},
	{CPSW_STATSD_INFO(rx_multicast_frames)},
	{CPSW_STATSD_INFO(rx_pause_frames)},
	{CPSW_STATSD_INFO(rx_crc_errors)},
	{CPSW_STATSD_INFO(rx_align_code_errors)},
	{CPSW_STATSD_INFO(rx_oversized_frames)},
	{CPSW_STATSD_INFO(rx_jabber_frames)},
	{CPSW_STATSD_INFO(rx_undersized_frames)},
	{CPSW_STATSD_INFO(rx_fragments)},
	{CPSW_STATSD_INFO(rx_bytes)},
	{CPSW_STATSD_INFO(tx_good_frames)},
	{CPSW_STATSD_INFO(tx_broadcast_frames)},
	{CPSW_STATSD_INFO(tx_multicast_frames)},
	{CPSW_STATSD_INFO(tx_pause_frames)},
	{CPSW_STATSD_INFO(tx_deferred_frames)},
	{CPSW_STATSD_INFO(tx_collision_frames)},
	{CPSW_STATSD_INFO(tx_single_coll_frames)},
	{CPSW_STATSD_INFO(tx_mult_coll_frames)},
	{CPSW_STATSD_INFO(tx_excessive_collisions)},
	{CPSW_STATSD_INFO(tx_late_collisions)},
	{CPSW_STATSD_INFO(tx_underrun)},
	{CPSW_STATSD_INFO(tx_carrier_sense_errors)},
	{CPSW_STATSD_INFO(tx_bytes)},
	{CPSW_STATSD_INFO(tx_64byte_frames)},
	{CPSW_STATSD_INFO(tx_65_to_127byte_frames)},
	{CPSW_STATSD_INFO(tx_128_to_255byte_frames)},
	{CPSW_STATSD_INFO(tx_256_to_511byte_frames)},
	{CPSW_STATSD_INFO(tx_512_to_1023byte_frames)},
	{CPSW_STATSD_INFO(tx_1024byte_frames)},
	{CPSW_STATSD_INFO(net_bytes)},
	{CPSW_STATSD_INFO(rx_sof_overruns)},
	{CPSW_STATSD_INFO(rx_mof_overruns)},
	{CPSW_STATSD_INFO(rx_dma_overruns)},
};

#define ETHTOOL_STATS_NUM ARRAY_SIZE(et_stats)

struct cpsw_priv {
	struct device			*dev;
	struct clk			*cpgmac;
	struct netcp_device		*netcp_device;
	u32				 num_slaves;
	u32				 ale_ageout;
	u32				 ale_entries;
	u32				 ale_ports;
	u32				 sgmii_module_ofs;
	u32				 sgmii_module34_ofs;
	u32				 switch_module_ofs;
	u32				 host_port_reg_ofs;
	u32				 slave_reg_ofs;
	u32				 sliver_reg_ofs;
	u32				 slave23_reg_ofs;
	u32				 hw_stats_reg_ofs;
	u32				 ale_reg_ofs;
	u32				 cpts_reg_ofs;

	int				 host_port;
	u32				 rx_packet_max;

	struct cpsw_regs __iomem	*regs;
	struct cpsw_ss_regs __iomem	*ss_regs;
	struct cpsw_hw_stats __iomem	*hw_stats_regs[2];
	struct cpsw_host_regs __iomem	*host_port_regs;
	struct cpsw_ale_regs __iomem	*ale_reg;

	void __iomem			*sgmii_port_regs;
	void __iomem			*sgmii_port34_regs;

	struct cpsw_ale			*ale;
	atomic_t			 ale_refcnt;

	u32				 link[MAX_SLAVES + 1];
	struct device_node		*phy_node[MAX_SLAVES];

	u32				 intf_tx_queues;

	u32				 multi_if;
	u32				 slaves_per_interface;
	u32				 num_interfaces;
	struct device_node		*interfaces;
	struct list_head		 cpsw_intf_head;

	u64				 hw_stats[ETHTOOL_STATS_NUM];
	u32				 hw_stats_prev[ETHTOOL_STATS_NUM];
	int				 init_serdes_at_probe;
	struct kobject			kobj;
	struct kobject			tx_pri_kobj;
	struct kobject			pvlan_kobj;
	struct kobject			port_ts_kobj[MAX_SLAVES];
	struct kobject			stats_kobj;
	spinlock_t			hw_stats_lock;
	struct cpts			cpts;
	int				cpts_registered;
	int				force_no_hwtstamp;
	void __iomem			*serdes_regs[CPSW_SERDES_MAX_NUM];
	u32				num_serdes;
	u32				serdes_lanes;
	struct serdes			serdes;
	u32				opened;
};

/* slave_port: 0-based (currently relevant only in multi_if mode)
*/
struct cpsw_intf {
	struct net_device	*ndev;
	struct device		*dev;
	struct cpsw_priv	*cpsw_priv;
	struct device_node	*phy_node;
	u32			 num_slaves;
	u32			 slave_port;
	struct cpsw_slave	*slaves;
	u32			 intf_tx_queues;
	const char		*tx_chan_name;
	u32			 tx_queue_depth;
	struct netcp_tx_pipe	 tx_pipe;
	u32			 multi_if;
	struct list_head	 cpsw_intf_list;
	struct timer_list	 timer;
	u32			 link_state;
};

static struct cpsw_priv *global_priv;		/* FIXME: REMOVE THIS!! */

struct cpsw_attribute {
	struct attribute attr;
	ssize_t (*show)(struct cpsw_priv *cpsw_dev,
		struct cpsw_attribute *attr, char *buf);
	ssize_t	(*store)(struct cpsw_priv *cpsw_dev,
		struct cpsw_attribute *attr, const char *, size_t);
	const struct cpsw_mod_info *info;
	ssize_t info_size;
	void *context;
};
#define to_cpsw_attr(_attr) container_of(_attr, struct cpsw_attribute, attr)

#define to_cpsw_dev(obj) container_of(obj, struct cpsw_priv, kobj)
#define tx_pri_to_cpsw_dev(obj) container_of(obj, struct cpsw_priv, tx_pri_kobj)
#define pvlan_to_cpsw_dev(obj) container_of(obj, struct cpsw_priv, pvlan_kobj)
#define stats_to_cpsw_dev(obj) container_of(obj, struct cpsw_priv, stats_kobj)

#define BITS(x)			(BIT(x) - 1)
#define BITMASK(n, s)		(BITS(n) << (s))
#define cpsw_mod_info_field_val(r, i) \
	((r & BITMASK(i->bits, i->shift)) >> i->shift)

#define for_each_intf(i, priv) \
	list_for_each_entry((i), &(priv)->cpsw_intf_head, cpsw_intf_list)

#define __CPSW_ATTR_FULL(_name, _mode, _show, _store, _info,	\
				_info_size, _ctxt)		\
	{ \
		.attr = {.name = __stringify(_name), .mode = _mode },	\
		.show	= _show,		\
		.store	= _store,		\
		.info	= _info,		\
		.info_size = _info_size,	\
		.context = (_ctxt),		\
	}

#define __CPSW_ATTR(_name, _mode, _show, _store, _info) \
		__CPSW_ATTR_FULL(_name, _mode, _show, _store, _info, \
					(ARRAY_SIZE(_info)), NULL)

#define __CPSW_CTXT_ATTR(_name, _mode, _show, _store, _info, _ctxt) \
		__CPSW_ATTR_FULL(_name, _mode, _show, _store, _info, \
					(ARRAY_SIZE(_info)), _ctxt)

struct cpsw_mod_info {
	const char	*name;
	int		shift;
	int		bits;
};

struct cpsw_parse_result {
	int control;
	int port;
	u32 value;
};

static ssize_t cpsw_attr_info_show(const struct cpsw_mod_info *info,
				int info_size, u32 reg, char *buf)
{
	int i, len = 0;

	for (i = 0; i < info_size; i++, info++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
			"%s=%d\n", info->name,
			(int)cpsw_mod_info_field_val(reg, info));
	}

	return len;
}

static ssize_t cpsw_attr_parse_set_command(struct cpsw_priv *cpsw_dev,
			      struct cpsw_attribute *attr,
			      const char *buf, size_t count,
				struct cpsw_parse_result *res)
{
	char ctrl_str[33], tmp_str[9];
	int port = -1, value, len, control;
	unsigned long end;
	const struct cpsw_mod_info *info = attr->info;

	len = strcspn(buf, ".=");
	if (len >= 32)
		return -ENOMEM;

	strncpy(ctrl_str, buf, len);
	ctrl_str[len] = '\0';
	buf += len;

	if (*buf == '.') {
		++buf;
		len = strcspn(buf, "=");
		if (len >= 8)
			return -ENOMEM;
		strncpy(tmp_str, buf, len);
		tmp_str[len] = '\0';
		if (kstrtoul(tmp_str, 0, &end))
			return -EINVAL;
		port = (int)end;
		buf += len;
	}

	if (*buf != '=')
		return -EINVAL;

	if (kstrtoul(buf + 1, 0, &end))
		return -EINVAL;

	value = (int)end;

	for (control = 0; control < attr->info_size; control++)
		if (strcmp(ctrl_str, info[control].name) == 0)
			break;

	if (control >= attr->info_size)
		return -ENOENT;

	res->control = control;
	res->port = port;
	res->value = value;

	dev_info(cpsw_dev->dev, "parsed command %s.%d=%d\n",
		attr->info[control].name, port, value);

	return 0;
}

static inline void cpsw_info_set_reg_field(void __iomem *r,
		const struct cpsw_mod_info *info, int val)
{
	u32 rv;

	rv = __raw_readl(r);
	rv = ((rv & ~BITMASK(info->bits, info->shift)) | (val << info->shift));
	__raw_writel(rv, r);
}

static ssize_t cpsw_version_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_attribute *attr,
		     char *buf)
{
	u32 reg;

	reg = __raw_readl(&cpsw_dev->regs->id_ver);

	return snprintf(buf, PAGE_SIZE,
		"cpsw version %d.%d (%d) SGMII identification value 0x%x\n",
		 CPSW_MAJOR_VERSION(reg), CPSW_MINOR_VERSION(reg),
		 CPSW_RTL_VERSION(reg), CPSW_SGMII_IDENT(reg));
}

static struct cpsw_attribute cpsw_version_attribute =
	__ATTR(version, S_IRUGO, cpsw_version_show, NULL);

static const struct cpsw_mod_info cpsw_controls[] = {
	{
		.name		= "fifo_loopback",
		.shift		= 0,
		.bits		= 1,
	},
	{
		.name		= "vlan_aware",
		.shift		= 1,
		.bits		= 1,
	},
	{
		.name		= "p0_enable",
		.shift		= 2,
		.bits		= 1,
	},
	{
		.name		= "p0_pass_pri_tagged",
		.shift		= 3,
		.bits		= 1,
	},
	{
		.name		= "p1_pass_pri_tagged",
		.shift		= 4,
		.bits		= 1,
	},
	{
		.name		= "p2_pass_pri_tagged",
		.shift		= 5,
		.bits		= 1,
	},
	{
		.name		= "p3_pass_pri_tagged",
		.shift		= 7,
		.bits		= 1,
	},
	{
		.name		= "p4_pass_pri_tagged",
		.shift		= 8,
		.bits		= 1,
	},
};

static ssize_t cpsw_control_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_attribute *attr,
		     char *buf)
{
	u32 reg;

	reg = __raw_readl(&cpsw_dev->regs->control);
	return cpsw_attr_info_show(attr->info, attr->info_size, reg, buf);
}

static ssize_t cpsw_control_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_attribute *attr,
			      const char *buf, size_t count)
{
	const struct cpsw_mod_info *i;
	struct cpsw_parse_result res;
	void __iomem *r = NULL;
	int ret;


	ret = cpsw_attr_parse_set_command(cpsw_dev, attr, buf, count, &res);
	if (ret)
		return ret;

	i = &(attr->info[res.control]);
	r = &cpsw_dev->regs->control;

	cpsw_info_set_reg_field(r, i, res.value);
	return count;
}

static struct cpsw_attribute cpsw_control_attribute =
	__CPSW_ATTR(control, S_IRUGO | S_IWUSR,
		cpsw_control_show, cpsw_control_store, cpsw_controls);

static const struct cpsw_mod_info cpsw_ptypes[] = {
	{
		.name		= "escalate_pri_load_val",
		.shift		= 0,
		.bits		= 5,
	},
	{
		.name		= "port0_pri_type_escalate",
		.shift		= 8,
		.bits		= 1,
	},
	{
		.name		= "port1_pri_type_escalate",
		.shift		= 9,
		.bits		= 1,
	},
	{
		.name		= "port2_pri_type_escalate",
		.shift		= 10,
		.bits		= 1,
	},
	{
		.name		= "port3_pri_type_escalate",
		.shift		= 11,
		.bits		= 1,
	},
	{
		.name		= "port4_pri_type_escalate",
		.shift		= 12,
		.bits		= 1,
	},
};

static ssize_t cpsw_pri_type_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_attribute *attr,
		     char *buf)
{
	u32 reg;

	reg = __raw_readl(&cpsw_dev->regs->ptype);

	return cpsw_attr_info_show(attr->info, attr->info_size, reg, buf);
}

static ssize_t cpsw_pri_type_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_attribute *attr,
			      const char *buf, size_t count)
{
	const struct cpsw_mod_info *i;
	struct cpsw_parse_result res;
	void __iomem *r = NULL;
	int ret;


	ret = cpsw_attr_parse_set_command(cpsw_dev, attr, buf, count, &res);
	if (ret)
		return ret;

	i = &(attr->info[res.control]);
	r = &cpsw_dev->regs->ptype;

	cpsw_info_set_reg_field(r, i, res.value);
	return count;
}

static struct cpsw_attribute cpsw_pri_type_attribute =
	__CPSW_ATTR(priority_type, S_IRUGO | S_IWUSR,
			cpsw_pri_type_show,
			cpsw_pri_type_store,
			cpsw_ptypes);

static const struct cpsw_mod_info cpsw_flow_controls[] = {
	{
		.name		= "port0_flow_control_en",
		.shift		= 0,
		.bits		= 1,
	},
	{
		.name		= "port1_flow_control_en",
		.shift		= 1,
		.bits		= 1,
	},
	{
		.name		= "port2_flow_control_en",
		.shift		= 2,
		.bits		= 1,
	},
	{
		.name		= "port3_flow_control_en",
		.shift		= 3,
		.bits		= 1,
	},
	{
		.name		= "port4_flow_control_en",
		.shift		= 4,
		.bits		= 1,
	},
};

static ssize_t cpsw_flow_control_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_attribute *attr, char *buf)
{
	u32 reg;

	reg = __raw_readl(&cpsw_dev->regs->flow_control);

	return cpsw_attr_info_show(attr->info, attr->info_size, reg, buf);
}

static ssize_t cpsw_flow_control_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_attribute *attr,
			      const char *buf, size_t count)
{
	const struct cpsw_mod_info *i;
	struct cpsw_parse_result res;
	void __iomem *r = NULL;
	int ret;


	ret = cpsw_attr_parse_set_command(cpsw_dev, attr, buf, count, &res);
	if (ret)
		return ret;

	i = &(attr->info[res.control]);
	r = &cpsw_dev->regs->flow_control;

	cpsw_info_set_reg_field(r, i, res.value);
	return count;
}

static struct cpsw_attribute cpsw_flow_control_attribute =
	__CPSW_ATTR(flow_control, S_IRUGO | S_IWUSR,
		cpsw_flow_control_show,
		cpsw_flow_control_store,
		cpsw_flow_controls);

static const struct cpsw_mod_info cpsw_port_tx_pri_maps[] = {
	{
		.name		= "port_tx_pri_0",
		.shift		= 0,
		.bits		= 3,
	},
	{
		.name		= "port_tx_pri_1",
		.shift		= 4,
		.bits		= 3,
	},
	{
		.name		= "port_tx_pri_2",
		.shift		= 8,
		.bits		= 3,
	},
	{
		.name		= "port_tx_pri_3",
		.shift		= 12,
		.bits		= 3,
	},
	{
		.name		= "port_tx_pri_4",
		.shift		= 16,
		.bits		= 3,
	},
	{
		.name		= "port_tx_pri_5",
		.shift		= 20,
		.bits		= 3,
	},
	{
		.name		= "port_tx_pri_6",
		.shift		= 24,
		.bits		= 3,
	},
	{
		.name		= "port_tx_pri_7",
		.shift		= 28,
		.bits		= 3,
	},
};

static ssize_t cpsw_port_tx_pri_map_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_attribute *attr,
		     char *buf)
{
	int idx, len = 0, total_len = 0, port;
	struct cpsw_intf *cpsw_intf;
	struct cpsw_slave *slave;
	u32 reg;

	port = (int)(attr->context);

	for_each_intf(cpsw_intf, cpsw_dev) {
		if (cpsw_intf->multi_if) {
			slave = cpsw_intf->slaves;
			if (slave->port_num != port)
				continue;
			reg = __raw_readl(&slave->regs->tx_pri_map);
			len = cpsw_attr_info_show(attr->info, attr->info_size,
						reg, buf+total_len);
			total_len += len;
		} else {
			for (idx = 0; idx < cpsw_intf->num_slaves; idx++) {
				slave = cpsw_intf->slaves + idx;
				if (slave->port_num != port)
					continue;
				reg = __raw_readl(&slave->regs->tx_pri_map);
				len = cpsw_attr_info_show(attr->info,
					attr->info_size, reg, buf+total_len);
				total_len += len;
			}
		}
	}
	return total_len;
}

static ssize_t cpsw_port_tx_pri_map_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_attribute *attr,
			      const char *buf, size_t count)
{
	const struct cpsw_mod_info *i;
	struct cpsw_parse_result res;
	struct cpsw_intf *cpsw_intf;
	struct cpsw_slave *slave;
	void __iomem *r = NULL;
	int ret, idx, port;

	port = (int)(attr->context);

	ret = cpsw_attr_parse_set_command(cpsw_dev, attr, buf, count, &res);
	if (ret)
		return ret;

	i = &(attr->info[res.control]);

	/* Slave port */
	for_each_intf(cpsw_intf, cpsw_dev) {
		if (cpsw_intf->multi_if) {
			slave = cpsw_intf->slaves;
			if (slave->port_num == port) {
				r = &slave->regs->tx_pri_map;
				goto set;
			}
		} else
			for (idx = 0; idx < cpsw_intf->num_slaves; idx++) {
				slave = cpsw_intf->slaves + idx;
				if (slave->port_num == port) {
					r = &slave->regs->tx_pri_map;
					goto set;
				}
			}
	}

	if (!r)
		return  -ENOENT;

set:
	cpsw_info_set_reg_field(r, i, res.value);
	return count;
}

static struct cpsw_attribute cpsw_tx_pri_1_attribute =
	__CPSW_CTXT_ATTR(1, S_IRUGO | S_IWUSR,
			cpsw_port_tx_pri_map_show,
			cpsw_port_tx_pri_map_store,
			cpsw_port_tx_pri_maps, (void *)1);

static struct cpsw_attribute cpsw_tx_pri_2_attribute =
	__CPSW_CTXT_ATTR(2, S_IRUGO | S_IWUSR,
			cpsw_port_tx_pri_map_show,
			cpsw_port_tx_pri_map_store,
			cpsw_port_tx_pri_maps, (void *)2);

static struct cpsw_attribute cpsw_tx_pri_3_attribute =
	__CPSW_CTXT_ATTR(3, S_IRUGO | S_IWUSR,
			cpsw_port_tx_pri_map_show,
			cpsw_port_tx_pri_map_store,
			cpsw_port_tx_pri_maps, (void *)3);

static struct cpsw_attribute cpsw_tx_pri_4_attribute =
	__CPSW_CTXT_ATTR(4, S_IRUGO | S_IWUSR,
			cpsw_port_tx_pri_map_show,
			cpsw_port_tx_pri_map_store,
			cpsw_port_tx_pri_maps, (void *)4);

static struct attribute *cpsw_tx_pri_default_attrs[] = {
	&cpsw_tx_pri_1_attribute.attr,
	&cpsw_tx_pri_2_attribute.attr,
	&cpsw_tx_pri_3_attribute.attr,
	&cpsw_tx_pri_4_attribute.attr,
	NULL
};

static ssize_t cpsw_tx_pri_attr_show(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	struct cpsw_attribute *attribute = to_cpsw_attr(attr);
	struct cpsw_priv *cpsw_dev = tx_pri_to_cpsw_dev(kobj);

	if (!attribute->show)
		return -EIO;

	return attribute->show(cpsw_dev, attribute, buf);
}

static ssize_t cpsw_tx_pri_attr_store(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	struct cpsw_attribute *attribute = to_cpsw_attr(attr);
	struct cpsw_priv *cpsw_dev = tx_pri_to_cpsw_dev(kobj);

	if (!attribute->store)
		return -EIO;

	return attribute->store(cpsw_dev, attribute, buf, count);
}

static const struct sysfs_ops cpsw_tx_pri_sysfs_ops = {
	.show = cpsw_tx_pri_attr_show,
	.store = cpsw_tx_pri_attr_store,
};

static struct kobj_type cpsw_tx_pri_ktype = {
	.sysfs_ops = &cpsw_tx_pri_sysfs_ops,
	.default_attrs = cpsw_tx_pri_default_attrs,
};

static const struct cpsw_mod_info cpsw_port_vlans[] = {
	{
		.name		= "port_vlan_id",
		.shift		= 0,
		.bits		= 12,
	},
	{
		.name		= "port_cfi",
		.shift		= 12,
		.bits		= 1,
	},
	{
		.name		= "port_vlan_pri",
		.shift		= 13,
		.bits		= 3,
	},
};

static ssize_t cpsw_port_vlan_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_attribute *attr,
		     char *buf)
{
	int idx, len = 0, total_len = 0, port;
	struct cpsw_intf *cpsw_intf;
	struct cpsw_slave *slave;
	u32 reg;

	port = (int)(attr->context);

	if (port == cpsw_dev->host_port) {
		/* Host port */
		reg = __raw_readl(&cpsw_dev->host_port_regs->port_vlan);
		len = cpsw_attr_info_show(attr->info, attr->info_size,
					reg, buf);
		return len;
	}

	/* Slave ports */
	for_each_intf(cpsw_intf, cpsw_dev) {
		if (cpsw_intf->multi_if) {
			slave = cpsw_intf->slaves;
			if (slave->port_num != port)
				continue;
			reg = __raw_readl(&slave->regs->port_vlan);
			len = cpsw_attr_info_show(attr->info, attr->info_size,
					reg, buf+total_len);
			total_len += len;
		} else {
			for (idx = 0; idx < cpsw_intf->num_slaves; idx++) {
				slave = cpsw_intf->slaves + idx;
				if (slave->port_num != port)
					continue;
				reg = __raw_readl(&slave->regs->port_vlan);
				len = cpsw_attr_info_show(attr->info,
					attr->info_size, reg, buf+total_len);
				total_len += len;
			}
		}
	}
	return total_len;
}

static ssize_t cpsw_port_vlan_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_attribute *attr,
			      const char *buf, size_t count)
{
	const struct cpsw_mod_info *i;
	struct cpsw_parse_result res;
	struct cpsw_intf *cpsw_intf;
	struct cpsw_slave *slave;
	void __iomem *r = NULL;
	int ret, idx, port;

	port = (int)(attr->context);

	ret = cpsw_attr_parse_set_command(cpsw_dev, attr, buf, count, &res);
	if (ret)
		return ret;

	i = &(attr->info[res.control]);

	/* Host port */
	if (port == cpsw_dev->host_port) {
		r = &cpsw_dev->host_port_regs->port_vlan;
		goto set;
	}

	/* Slave port */
	for_each_intf(cpsw_intf, cpsw_dev) {
		if (cpsw_intf->multi_if) {
			slave = cpsw_intf->slaves;
			if (slave->port_num == port) {
				r = &slave->regs->port_vlan;
				goto set;
			}
		} else
			for (idx = 0; idx < cpsw_intf->num_slaves; idx++) {
				slave = cpsw_intf->slaves + idx;
				if (slave->port_num == port) {
					r = &slave->regs->port_vlan;
					goto set;
				}
			}
	}

	if (!r)
		return  -ENOENT;

set:
	cpsw_info_set_reg_field(r, i, res.value);
	return count;
}

static struct cpsw_attribute cpsw_pvlan_0_attribute =
	__CPSW_CTXT_ATTR(0, S_IRUGO | S_IWUSR,
			cpsw_port_vlan_show,
			cpsw_port_vlan_store,
			cpsw_port_vlans, (void *)0);

static struct cpsw_attribute cpsw_pvlan_1_attribute =
	__CPSW_CTXT_ATTR(1, S_IRUGO | S_IWUSR,
			cpsw_port_vlan_show,
			cpsw_port_vlan_store,
			cpsw_port_vlans, (void *)1);

static struct cpsw_attribute cpsw_pvlan_2_attribute =
	__CPSW_CTXT_ATTR(2, S_IRUGO | S_IWUSR,
			cpsw_port_vlan_show,
			cpsw_port_vlan_store,
			cpsw_port_vlans, (void *)2);

static struct cpsw_attribute cpsw_pvlan_3_attribute =
	__CPSW_CTXT_ATTR(3, S_IRUGO | S_IWUSR,
			cpsw_port_vlan_show,
			cpsw_port_vlan_store,
			cpsw_port_vlans, (void *)3);

static struct cpsw_attribute cpsw_pvlan_4_attribute =
	__CPSW_CTXT_ATTR(4, S_IRUGO | S_IWUSR,
			cpsw_port_vlan_show,
			cpsw_port_vlan_store,
			cpsw_port_vlans, (void *)4);

static struct attribute *cpsw_pvlan_default_attrs[] = {
	&cpsw_pvlan_0_attribute.attr,
	&cpsw_pvlan_1_attribute.attr,
	&cpsw_pvlan_2_attribute.attr,
	&cpsw_pvlan_3_attribute.attr,
	&cpsw_pvlan_4_attribute.attr,
	NULL
};

static ssize_t cpsw_pvlan_attr_show(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	struct cpsw_attribute *attribute = to_cpsw_attr(attr);
	struct cpsw_priv *cpsw_dev = pvlan_to_cpsw_dev(kobj);

	if (!attribute->show)
		return -EIO;

	return attribute->show(cpsw_dev, attribute, buf);
}

static ssize_t cpsw_pvlan_attr_store(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	struct cpsw_attribute *attribute = to_cpsw_attr(attr);
	struct cpsw_priv *cpsw_dev = pvlan_to_cpsw_dev(kobj);

	if (!attribute->store)
		return -EIO;

	return attribute->store(cpsw_dev, attribute, buf, count);
}

static const struct sysfs_ops cpsw_pvlan_sysfs_ops = {
	.show = cpsw_pvlan_attr_show,
	.store = cpsw_pvlan_attr_store,
};

static struct kobj_type cpsw_pvlan_ktype = {
	.sysfs_ops = &cpsw_pvlan_sysfs_ops,
	.default_attrs = cpsw_pvlan_default_attrs,
};

struct cpsw_ts_attribute {
	struct attribute attr;
	ssize_t (*show)(struct cpsw_priv *cpsw_dev,
		struct cpsw_ts_attribute *attr, char *buf, void *);
	ssize_t	(*store)(struct cpsw_priv *cpsw_dev,
		struct cpsw_ts_attribute *attr, const char *, size_t, void *);
};
#define to_cpsw_ts_attr(_attr) \
	container_of(_attr, struct cpsw_ts_attribute, attr)

#define __CPSW_TS_ATTR(_name, _mode, _show, _store)		\
	{ \
		.attr = {.name = __stringify(_name), .mode = _mode },	\
		.show	= _show,		\
		.store	= _store,		\
	}

#define pts_to_cpsw_dev(obj) container_of(obj, struct cpsw_priv, pts_kobj)

#define pts_n_to_cpsw_dev(obj, n) \
	container_of(obj, struct cpsw_priv, port_ts_kobj[n])

struct cpsw_slave *cpsw_port_num_get_slave(struct cpsw_priv *cpsw_dev, int port)
{
	struct cpsw_intf *cpsw_intf;
	struct cpsw_slave *slave = NULL;
	int idx;

	for_each_intf(cpsw_intf, cpsw_dev) {
		if (cpsw_intf->multi_if) {
			slave = cpsw_intf->slaves;
			if (slave->port_num == port)
				return slave;
		} else {
			for (idx = 0; idx < cpsw_intf->num_slaves; idx++) {
				slave = cpsw_intf->slaves + idx;
				if (slave->port_num == port)
					return slave;
			}
		}
	}
	return NULL;
}

static ssize_t cpsw_port_ts_uni_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_ts_attribute *attr,
		     char *buf, void *context)
{
	struct cpsw_slave *slave;
	int len, port;
	u32 reg;

	port = (int)context;

	slave = cpsw_port_num_get_slave(cpsw_dev, port);
	if (!slave)
		return 0;

	reg = readl(&slave->regs->ts_ctl_ltype2);
	len = snprintf(buf, PAGE_SIZE, "%lu\n",
		((reg & CPSW_TS_UNI_EN) >> CPSW_TS_UNI_EN_SHIFT));

	return len;
}

static ssize_t cpsw_port_ts_uni_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_ts_attribute *attr,
			      const char *buf, size_t count, void *context)
{
	struct cpsw_slave *slave;
	int port, val;
	u32 reg, mode;

	port = (int)context;

	slave = cpsw_port_num_get_slave(cpsw_dev, port);
	if (!slave)
		return 0;

	if (kstrtoint(buf, 0, &val) < 0)
		return -EINVAL;


	if (val)
		mode = CPSW_TS_UNI_EN;
	else
		mode = (slave->ts_ctl.maddr_map << CPSW_TS_CTL_MADDR_SHIFT);

	reg = readl(&slave->regs->ts_ctl_ltype2);
	reg &= ~(CPSW_TS_UNI_EN | CPSW_TS_CTL_MADDR_ALL);
	reg |= mode;
	writel(reg, &slave->regs->ts_ctl_ltype2);

	slave->ts_ctl.uni = (val ? 1 : 0);
	return count;
}

static struct cpsw_ts_attribute cpsw_pts_uni_attribute =
	__CPSW_TS_ATTR(uni_en, S_IRUGO | S_IWUSR,
			cpsw_port_ts_uni_show,
			cpsw_port_ts_uni_store);

static ssize_t cpsw_port_ts_maddr_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_ts_attribute *attr, char *buf, void *context)
{
	struct cpsw_slave *slave;
	int len, port;
	u32 reg;

	port = (int)context;

	slave = cpsw_port_num_get_slave(cpsw_dev, port);
	if (!slave)
		return 0;

	reg = readl(&slave->regs->ts_ctl_ltype2);
	len = snprintf(buf, PAGE_SIZE, "%02x\n",
		(reg >> CPSW_TS_CTL_MADDR_SHIFT) & 0x1f);
	return len;
}

static ssize_t cpsw_port_ts_maddr_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_ts_attribute *attr,
			      const char *buf, size_t count, void *context)
{
	struct cpsw_slave *slave;
	int port;
	u32 reg;
	u8 val;

	port = (int)context;

	slave = cpsw_port_num_get_slave(cpsw_dev, port);
	if (!slave)
		return 0;

	if (kstrtou8(buf, 0, &val) < 0)
		return -EINVAL;

	reg = readl(&slave->regs->ts_ctl_ltype2);
	reg &= ~CPSW_TS_CTL_MADDR_ALL;
	reg |= ((val & 0x1f) << CPSW_TS_CTL_MADDR_SHIFT);
	writel(reg, &slave->regs->ts_ctl_ltype2);

	slave->ts_ctl.maddr_map = val & 0x1f;
	return count;
}

static struct cpsw_ts_attribute cpsw_pts_maddr_attribute =
	__CPSW_TS_ATTR(mcast_addr, S_IRUGO | S_IWUSR,
			cpsw_port_ts_maddr_show,
			cpsw_port_ts_maddr_store);

static ssize_t cpsw_port_ts_dst_port_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_ts_attribute *attr, char *buf, void *context)
{
	struct cpsw_slave *slave;
	int len, port;
	u32 reg;

	port = (int)context;

	slave = cpsw_port_num_get_slave(cpsw_dev, port);
	if (!slave)
		return 0;

	reg = readl(&slave->regs->ts_ctl_ltype2);
	len = snprintf(buf, PAGE_SIZE, "%01x\n",
		(reg >> CPSW_TS_CTL_DST_PORT_SHIFT) & 0x3);
	return len;
}

static ssize_t cpsw_port_ts_dst_port_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_ts_attribute *attr,
			      const char *buf, size_t count, void *context)
{
	struct cpsw_slave *slave;
	int port;
	u32 reg;
	u8 val;

	port = (int)context;

	slave = cpsw_port_num_get_slave(cpsw_dev, port);
	if (!slave)
		return 0;

	if (kstrtou8(buf, 0, &val) < 0)
		return -EINVAL;

	reg = readl(&slave->regs->ts_ctl_ltype2);
	reg &= ~CPSW_TS_CTL_DST_PORT;
	reg |= ((val & 0x3) << CPSW_TS_CTL_DST_PORT_SHIFT);
	writel(reg, &slave->regs->ts_ctl_ltype2);

	slave->ts_ctl.dst_port_map = val & 0x3;
	return count;
}

static struct cpsw_ts_attribute cpsw_pts_dst_port_attribute =
	__CPSW_TS_ATTR(dst_port, S_IRUGO | S_IWUSR,
			cpsw_port_ts_dst_port_show,
			cpsw_port_ts_dst_port_store);

static ssize_t cpsw_port_ts_config_show(struct cpsw_priv *cpsw_dev,
		     struct cpsw_ts_attribute *attr, char *buf, void *context)
{
	struct cpsw_slave *slave;
	int len, port, total_len = 0;
	u32 reg;
	char *p = buf;

	port = (int)context;

	slave = cpsw_port_num_get_slave(cpsw_dev, port);
	if (!slave)
		return 0;

	reg = readl(&slave->regs->ts_ctl);
	len = snprintf(p, PAGE_SIZE, "%08x ", reg);
	p += len;
	total_len += len;

	reg = readl(&slave->regs->ts_seq_ltype);
	len = snprintf(p, PAGE_SIZE, "%08x ", reg);
	p += len;
	total_len += len;

	reg = readl(&slave->regs->ts_vlan);
	len = snprintf(p, PAGE_SIZE, "%08x ", reg);
	p += len;
	total_len += len;

	reg = readl(&slave->regs->ts_ctl_ltype2);
	len = snprintf(p, PAGE_SIZE, "%08x ", reg);
	p += len;
	total_len += len;

	reg = readl(&slave->regs->ts_ctl2);
	len = snprintf(p, PAGE_SIZE, "%08x\n", reg);
	p += len;
	total_len += len;

	return total_len;
}

static ssize_t cpsw_port_ts_config_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_ts_attribute *attr,
			      const char *buf, size_t count, void *context)
{
	struct cpsw_slave *slave;
	unsigned long reg, val;
	int len, port;
	char tmp_str[4];
	u8 reg_num = 0;
	u32 __iomem *p;

	port = (int)context;

	slave = cpsw_port_num_get_slave(cpsw_dev, port);
	if (!slave)
		return 0;

	len = strcspn(buf, " ");
	if (len > 1)
		return -ENOMEM;

	strncpy(tmp_str, buf, len);
	tmp_str[len] = '\0';
	if (kstrtou8(tmp_str, 0, &reg_num))
		return -EINVAL;

	buf += (len + 1);
	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	switch(reg_num) {
		case 1:
			p = &slave->regs->ts_ctl;
		break;
		case 2:
			p = &slave->regs->ts_seq_ltype;
		break;
		case 3:
			p = &slave->regs->ts_vlan;
		break;
		case 4:
			p = &slave->regs->ts_ctl_ltype2;
		break;
		case 5:
			p = &slave->regs->ts_ctl2;
		break;
		default:
			return -EINVAL;
	}

	reg = readl(p);
	if (reg != val)
		writel(val, p);

	return count;
}

static struct cpsw_ts_attribute cpsw_pts_config_attribute =
	__CPSW_TS_ATTR(config, S_IRUGO | S_IWUSR,
			cpsw_port_ts_config_show,
			cpsw_port_ts_config_store);

static struct attribute *cpsw_pts_n_default_attrs[] = {
	&cpsw_pts_uni_attribute.attr,
	&cpsw_pts_maddr_attribute.attr,
	&cpsw_pts_dst_port_attribute.attr,
	&cpsw_pts_config_attribute.attr,
	NULL
};

struct cpsw_priv *cpsw_port_ts_kobj_to_priv(struct kobject *kobj, int *port)
{
	char *port_name[] = {"1", "2", "3", "4", NULL};
	struct cpsw_priv *cpsw_dev;
	struct kobject *kobj_0;
	int i = 0;

	*port = -1;

	while (i < MAX_SLAVES && port_name[i]) {
		if (strncmp(port_name[i], kobject_name(kobj), 1) == 0)
			*port = i+1;
		i++;
	}

	if (*port < 0)
		return NULL;

	kobj_0 = kobj - (*port - 1);
	cpsw_dev = pts_n_to_cpsw_dev(kobj_0, 0);
	return cpsw_dev;
}

static ssize_t cpsw_pts_n_attr_show(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	struct cpsw_ts_attribute *attribute = to_cpsw_ts_attr(attr);
	struct cpsw_priv *cpsw_dev;
	int port = -1;

	if (!attribute->show)
		return -EIO;

	cpsw_dev = cpsw_port_ts_kobj_to_priv(kobj, &port);
	if (!cpsw_dev)
		return -EIO;

	return attribute->show(cpsw_dev, attribute, buf, (void *)port);
}

static ssize_t cpsw_pts_n_attr_store(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	struct cpsw_ts_attribute *attribute = to_cpsw_ts_attr(attr);
	struct cpsw_priv *cpsw_dev;
	int port = -1;

	if (!attribute->store)
		return -EIO;

	cpsw_dev = cpsw_port_ts_kobj_to_priv(kobj, &port);
	if (!cpsw_dev)
		return -EIO;

	return attribute->store(cpsw_dev, attribute, buf, count, (void *)port);
}

static const struct sysfs_ops cpsw_pts_n_sysfs_ops = {
	.show = cpsw_pts_n_attr_show,
	.store = cpsw_pts_n_attr_store,
};

static struct kobj_type cpsw_pts_n_ktype = {
	.sysfs_ops = &cpsw_pts_n_sysfs_ops,
	.default_attrs = cpsw_pts_n_default_attrs,
};

static void cpsw_reset_mod_stats(struct cpsw_priv *cpsw_dev, int stat_mod)
{
	struct cpsw_hw_stats __iomem *cpsw_statsa = cpsw_dev->hw_stats_regs[0];
	struct cpsw_hw_stats __iomem *cpsw_statsb = cpsw_dev->hw_stats_regs[1];
	void __iomem *base;
	u32  __iomem *p;
	u32 val;
	int i;

	val = readl(&cpsw_dev->regs->stat_port_en);

	switch (stat_mod) {
	case CPSW_STATSA_MODULE:
		base = cpsw_statsa;
		val &= ~CPSW_STATS_CD_SEL;
		break;
	case CPSW_STATSB_MODULE:
		base = cpsw_statsb;
		val &= ~CPSW_STATS_CD_SEL;
		break;
	case CPSW_STATSC_MODULE:
		base = cpsw_statsa;
		val |= CPSW_STATS_CD_SEL;
		break;
	case CPSW_STATSD_MODULE:
		base = cpsw_statsb;
		val |= CPSW_STATS_CD_SEL;
		break;
	default:
		return;
	}

	/* make the stat modules visible */
	writel(val, &cpsw_dev->regs->stat_port_en);

	for (i = 0; i < ETHTOOL_STATS_NUM; i++) {
		if (et_stats[i].type == stat_mod) {
			cpsw_dev->hw_stats[i] = 0;
			p = base + et_stats[i].offset;
			cpsw_dev->hw_stats_prev[i] = readl_relaxed(p);
		}
	}
	return;
}

static ssize_t cpsw_stats_mod_store(struct cpsw_priv *cpsw_dev,
			      struct cpsw_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned long end;
	int stat_mod;

	if (kstrtoul(buf, 0, &end) != 0 || (end != 0))
		return -EINVAL;

	stat_mod = (int)(attr->context);
	spin_lock_bh(&cpsw_dev->hw_stats_lock);
	cpsw_reset_mod_stats(cpsw_dev, stat_mod);
	spin_unlock_bh(&cpsw_dev->hw_stats_lock);
	return count;
}

static struct cpsw_attribute cpsw_stats_a_attribute =
	__CPSW_ATTR_FULL(A, S_IWUSR, NULL, cpsw_stats_mod_store,
			NULL, 0, (void *)CPSW_STATSA_MODULE);

static struct cpsw_attribute cpsw_stats_b_attribute =
	__CPSW_ATTR_FULL(B, S_IWUSR, NULL, cpsw_stats_mod_store,
			NULL, 0, (void *)CPSW_STATSB_MODULE);

static struct cpsw_attribute cpsw_stats_c_attribute =
	__CPSW_ATTR_FULL(C, S_IWUSR, NULL, cpsw_stats_mod_store,
			NULL, 0, (void *)CPSW_STATSC_MODULE);

static struct cpsw_attribute cpsw_stats_d_attribute =
	__CPSW_ATTR_FULL(D, S_IWUSR, NULL, cpsw_stats_mod_store,
			NULL, 0, (void *)CPSW_STATSD_MODULE);

static struct attribute *cpsw_stats_default_attrs[] = {
	&cpsw_stats_a_attribute.attr,
	&cpsw_stats_b_attribute.attr,
	&cpsw_stats_c_attribute.attr,
	&cpsw_stats_d_attribute.attr,
	NULL
};

static ssize_t cpsw_stats_attr_store(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	struct cpsw_attribute *attribute = to_cpsw_attr(attr);
	struct cpsw_priv *cpsw_dev = stats_to_cpsw_dev(kobj);

	if (!attribute->store)
		return -EIO;

	return attribute->store(cpsw_dev, attribute, buf, count);
}

static const struct sysfs_ops cpsw_stats_sysfs_ops = {
	.store = cpsw_stats_attr_store,
};

static struct kobj_type cpsw_stats_ktype = {
	.sysfs_ops = &cpsw_stats_sysfs_ops,
	.default_attrs = cpsw_stats_default_attrs,
};

static struct attribute *cpsw_default_attrs[] = {
	&cpsw_version_attribute.attr,
	&cpsw_control_attribute.attr,
	&cpsw_pri_type_attribute.attr,
	&cpsw_flow_control_attribute.attr,
	NULL
};

static ssize_t cpsw_attr_show(struct kobject *kobj, struct attribute *attr,
				  char *buf)
{
	struct cpsw_attribute *attribute = to_cpsw_attr(attr);
	struct cpsw_priv *cpsw_dev = to_cpsw_dev(kobj);

	if (!attribute->show)
		return -EIO;

	return attribute->show(cpsw_dev, attribute, buf);
}

static ssize_t cpsw_attr_store(struct kobject *kobj, struct attribute *attr,
				   const char *buf, size_t count)
{
	struct cpsw_attribute *attribute = to_cpsw_attr(attr);
	struct cpsw_priv *cpsw_dev = to_cpsw_dev(kobj);

	if (!attribute->store)
		return -EIO;

	return attribute->store(cpsw_dev, attribute, buf, count);
}

static const struct sysfs_ops cpsw_sysfs_ops = {
	.show = cpsw_attr_show,
	.store = cpsw_attr_store,
};

static struct kobj_type cpsw_ktype = {
	.sysfs_ops = &cpsw_sysfs_ops,
	.default_attrs = cpsw_default_attrs,
};

static void keystone_get_drvinfo(struct net_device *ndev,
			     struct ethtool_drvinfo *info)
{
	strncpy(info->driver, NETCP_DRIVER_NAME, sizeof(info->driver));
	strncpy(info->version, NETCP_DRIVER_VERSION, sizeof(info->version));
}

static u32 keystone_get_msglevel(struct net_device *ndev)
{
	struct netcp_priv *netcp = netdev_priv(ndev);
	return netcp->msg_enable;
}

static void keystone_set_msglevel(struct net_device *ndev, u32 value)
{
	struct netcp_priv *netcp = netdev_priv(ndev);
	netcp->msg_enable = value;
}

static void keystone_get_stat_strings(struct net_device *ndev,
				   uint32_t stringset, uint8_t *data)
{
	int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ETHTOOL_STATS_NUM; i++) {
			memcpy(data, et_stats[i].desc, ETH_GSTRING_LEN);
			data += ETH_GSTRING_LEN;
		}
		break;
	case ETH_SS_TEST:
		break;
	}
}

static int keystone_get_sset_count(struct net_device *ndev, int stringset)
{
	switch (stringset) {
	case ETH_SS_TEST:
		return 0;
	case ETH_SS_STATS:
		return ETHTOOL_STATS_NUM;
	default:
		return -EINVAL;
	}
}

static void cpsw_update_stats(struct cpsw_priv *cpsw_dev, uint64_t *data)
{
	struct cpsw_hw_stats __iomem *cpsw_statsa = cpsw_dev->hw_stats_regs[0];
	struct cpsw_hw_stats __iomem *cpsw_statsb = cpsw_dev->hw_stats_regs[1];
	u64 *hw_stats = &cpsw_dev->hw_stats[0];
	u32 *hw_stats_prev = &cpsw_dev->hw_stats_prev[0];
	void __iomem *base = NULL;
	u32  __iomem *p;
	u32 curr, delta;
	u32 val, pair_size = (ETHTOOL_STATS_NUM / 2);
	int i, j, pair;

	for (pair = 0; pair < 2; pair++) {
		val = readl(&cpsw_dev->regs->stat_port_en);

		if (pair == 0)
			val &= ~CPSW_STATS_CD_SEL;
		else
			val |= CPSW_STATS_CD_SEL;

		/* make the stat modules visible */
		writel(val, &cpsw_dev->regs->stat_port_en);

		for (i = 0; i < pair_size; i++) {
			j = pair * pair_size + i;
			switch (et_stats[j].type) {
			case CPSW_STATSA_MODULE:
			case CPSW_STATSC_MODULE:
				base = cpsw_statsa;
				break;
			case CPSW_STATSB_MODULE:
			case CPSW_STATSD_MODULE:
				base  = cpsw_statsb;
				break;
			}

			p = base + et_stats[j].offset;
			curr = readl_relaxed(p);
			delta = curr - hw_stats_prev[j];
			hw_stats_prev[j] = curr;
			hw_stats[j] += delta;

			if (data)
				data[j] = hw_stats[j];
		}
	}

	return;
}

static void keystone_get_ethtool_stats(struct net_device *ndev,
				       struct ethtool_stats *stats,
				       uint64_t *data)
{
	spin_lock_bh(&global_priv->hw_stats_lock);
	cpsw_update_stats(global_priv, data);
	spin_unlock_bh(&global_priv->hw_stats_lock);

	return;
}

static int keystone_get_settings(struct net_device *ndev,
			      struct ethtool_cmd *cmd)
{
	struct phy_device *phy = ndev->phydev;
	struct cpsw_slave *slave;
	int ret;

	if (!phy)
		return -EINVAL;

	slave = (struct cpsw_slave *)phy->context;
	if (!slave)
		return -EINVAL;

	ret = phy_ethtool_gset(phy, cmd);
	if (!ret)
		cmd->port = slave->phy_port_t;

	return ret;
}

static int keystone_set_settings(struct net_device *ndev,
				struct ethtool_cmd *cmd)
{
	struct phy_device *phy = ndev->phydev;
	struct cpsw_slave *slave;
	u32 features = cmd->advertising & cmd->supported;

	if (!phy)
		return -EINVAL;

	slave = (struct cpsw_slave *)phy->context;
	if (!slave)
		return -EINVAL;

	if (cmd->port != slave->phy_port_t) {
		if ((cmd->port == PORT_TP) && !(features & ADVERTISED_TP))
			return -EINVAL;

		if ((cmd->port == PORT_AUI) && !(features & ADVERTISED_AUI))
			return -EINVAL;

		if ((cmd->port == PORT_BNC) && !(features & ADVERTISED_BNC))
			return -EINVAL;

		if ((cmd->port == PORT_MII) && !(features & ADVERTISED_MII))
			return -EINVAL;

		if ((cmd->port == PORT_FIBRE) && !(features & ADVERTISED_FIBRE))
			return -EINVAL;
	}

	slave->phy_port_t = cmd->port;

	return phy_ethtool_sset(phy, cmd);
}

#ifdef CONFIG_TI_CPTS
static int keystone_get_ts_info(struct net_device *ndev,
			    struct ethtool_ts_info *info)
{
	struct netcp_priv *netcp = netdev_priv(ndev);
	struct netcp_device *netcp_device = netcp->netcp_device;
	struct cpsw_priv *priv;

	/* find the instance of the module registered to the netcp_device */
	priv = netcp_device_find_module(netcp_device, CPSW_MODULE_NAME);
	if (!priv)
		return -EINVAL;

	info->so_timestamping =
		SOF_TIMESTAMPING_TX_HARDWARE |
		SOF_TIMESTAMPING_TX_SOFTWARE |
		SOF_TIMESTAMPING_RX_HARDWARE |
		SOF_TIMESTAMPING_RX_SOFTWARE |
		SOF_TIMESTAMPING_SOFTWARE |
		SOF_TIMESTAMPING_RAW_HARDWARE;
	info->phc_index = priv->cpts.phc_index;
	info->tx_types =
		(1 << HWTSTAMP_TX_OFF) |
		(1 << HWTSTAMP_TX_ON);
	info->rx_filters =
		(1 << HWTSTAMP_FILTER_NONE) |
		(1 << HWTSTAMP_FILTER_PTP_V1_L4_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_EVENT);
	return 0;
}
#else
static int keystone_get_ts_info(struct net_device *ndev,
			    struct ethtool_ts_info *info)
{
	info->so_timestamping =
		SOF_TIMESTAMPING_TX_SOFTWARE |
		SOF_TIMESTAMPING_RX_SOFTWARE |
		SOF_TIMESTAMPING_SOFTWARE;
	info->phc_index = -1;
	info->tx_types = 0;
	info->rx_filters = 0;
	return 0;
}
#endif

static const struct ethtool_ops keystone_ethtool_ops = {
	.get_drvinfo		= keystone_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_msglevel		= keystone_get_msglevel,
	.set_msglevel		= keystone_set_msglevel,
	.get_strings		= keystone_get_stat_strings,
	.get_sset_count		= keystone_get_sset_count,
	.get_ethtool_stats	= keystone_get_ethtool_stats,
	.get_settings		= keystone_get_settings,
	.set_settings		= keystone_set_settings,
	.get_ts_info		= keystone_get_ts_info,
};

#define mac_hi(mac)	(((mac)[0] << 0) | ((mac)[1] << 8) |	\
			 ((mac)[2] << 16) | ((mac)[3] << 24))
#define mac_lo(mac)	(((mac)[4] << 0) | ((mac)[5] << 8))

static void cpsw_set_slave_mac(struct cpsw_slave *slave,
			       struct cpsw_intf *cpsw_intf)
{
	struct net_device *ndev = cpsw_intf->ndev;

	__raw_writel(mac_hi(ndev->dev_addr), &slave->regs->sa_hi);
	__raw_writel(mac_lo(ndev->dev_addr), &slave->regs->sa_lo);
}

static inline int cpsw_get_slave_port(struct cpsw_priv *priv, u32 slave_num)
{
	if (priv->host_port == 0)
		return slave_num + 1;
	else
		return slave_num;
}

static void _cpsw_adjust_link(struct cpsw_slave *slave, bool *link)
{
	struct phy_device *phy = slave->phy;
	u32 mac_control = 0;
	u32 slave_port;

	if (!phy)
		return;

	slave_port = slave->port_num;

	if (phy->link) {
		mac_control = slave->mac_control;
		mac_control |= MACSL_DEFAULT_CONFIG;
		/* enable forwarding */
		cpsw_ale_control_set(slave->ale, slave_port,
				     ALE_PORT_STATE, ALE_PORT_STATE_FORWARD);

		if (phy->duplex)
			mac_control |= BIT(0);	/* FULLDUPLEXEN	*/
		else
			mac_control &= ~0x1;

		*link = true;
	} else {
		mac_control = 0;
		/* disable forwarding */
		cpsw_ale_control_set(slave->ale, slave_port,
				     ALE_PORT_STATE, ALE_PORT_STATE_DISABLE);
	}

	if (mac_control != slave->mac_control) {
		phy_print_status(phy);
		__raw_writel(mac_control, &slave->sliver->mac_control);
	}

	slave->mac_control = mac_control;
}

static void cpsw_adjust_link(struct net_device *n_dev, void *context)
{
	struct cpsw_slave *slave = (struct cpsw_slave *)context;
	struct netcp_priv *netcp = netdev_priv(n_dev);
	bool link = false;

	_cpsw_adjust_link(slave, &link);

	if (link)
		netcp->phy_link_state_mask |= BIT(slave->slave_num);
	else
		netcp->phy_link_state_mask &= ~BIT(slave->slave_num);
}

/*
 * Reset the the mac sliver
 * Soft reset is set and polled until clear, or until a timeout occurs
 */
static int cpsw_port_reset(struct cpsw_slave *slave)
{
	u32 i, v;

	/* Set the soft reset bit */
	__iowmb();
	__raw_writel(SOFT_RESET, &slave->sliver->soft_reset);
	__iowmb();

	/* Wait for the bit to clear */
	for (i = 0; i < DEVICE_EMACSL_RESET_POLL_COUNT; i++) {
		v = __raw_readl(&slave->sliver->soft_reset);
		if ((v & SOFT_RESET_MASK) != SOFT_RESET)
			return 0;
	}

	/* Timeout on the reset */
	return GMACSL_RET_WARN_RESET_INCOMPLETE;
}

/*
 * Configure the mac sliver
 */
static void cpsw_port_config(struct cpsw_slave *slave, int max_rx_len)
{
	if (max_rx_len > MAX_SIZE_STREAM_BUFFER)
		max_rx_len = MAX_SIZE_STREAM_BUFFER;

	slave->mac_control = MACSL_DEFAULT_CONFIG;

	__raw_writel(max_rx_len, &slave->sliver->rx_maxlen);

	__iowmb();
	__raw_writel(slave->mac_control, &slave->sliver->mac_control);
}

static void cpsw_slave_stop(struct cpsw_slave *slave,
			    struct cpsw_intf *cpsw_intf)
{
	struct cpsw_priv *priv = cpsw_intf->cpsw_priv;
	void __iomem *sgmii_port_regs;

	if (slave->slave_num < 2)
		sgmii_port_regs = priv->sgmii_port_regs;
	else
		sgmii_port_regs = priv->sgmii_port34_regs;

	/* disable forwarding */
	if (slave->ale)
		cpsw_ale_control_set(slave->ale, slave->port_num,
				ALE_PORT_STATE, ALE_PORT_STATE_DISABLE);

	keystone_sgmii_rtreset(sgmii_port_regs, slave->slave_num, true);

	cpsw_port_reset(slave);

	if (!slave->phy)
		return;

	phy_stop(slave->phy);
	phy_disconnect(slave->phy);
	slave->phy = NULL;
}

static void cpsw_slave_link(struct cpsw_slave *slave,
			    struct cpsw_intf *cpsw_intf)
{
	struct netcp_priv *netcp = netdev_priv(cpsw_intf->ndev);
	int sn = slave->slave_num;

	if (IS_SGMII_MAC_PHY(slave->link_interface)) {
		/* check only the bit in phy_link_state_mask
		 * that corresponds to the slave
		 */
		if (!(netcp->phy_link_state_mask & BIT(sn)))
			cpsw_intf->link_state &= ~BIT(sn);
	}
}

static void cpsw_slave_open(struct cpsw_slave *slave,
			    struct cpsw_intf *cpsw_intf)
{
	struct cpsw_priv *priv = cpsw_intf->cpsw_priv;
	void __iomem *sgmii_port_regs;
	u32 slave_port;

	if (slave->slave_num < 2)
		sgmii_port_regs = priv->sgmii_port_regs;
	else
		sgmii_port_regs = priv->sgmii_port34_regs;

	keystone_sgmii_reset(sgmii_port_regs, slave->slave_num);

	keystone_sgmii_config(sgmii_port_regs, slave->slave_num,
				slave->link_interface);

	cpsw_port_reset(slave);

	keystone_sgmii_rtreset(sgmii_port_regs, slave->slave_num, false);

	cpsw_port_config(slave, priv->rx_packet_max);

	cpsw_set_slave_mac(slave, cpsw_intf);

	/* this slave port here is 1 based */
	slave_port = cpsw_get_slave_port(priv, slave->slave_num);

	/* hence port num here is also 1 based */
	slave->port_num = slave_port;
	slave->ale = priv->ale;

	/* enable forwarding */
	cpsw_ale_control_set(priv->ale, slave_port,
			     ALE_PORT_STATE, ALE_PORT_STATE_FORWARD);

	cpsw_ale_add_mcast(priv->ale, cpsw_intf->ndev->broadcast,
			   1 << slave_port, 0, 0, ALE_MCAST_FWD_2);

	if (IS_SGMII_MAC_PHY(slave->link_interface)) {
		slave->phy = of_phy_connect(cpsw_intf->ndev,
					    cpsw_intf->phy_node,
					    &cpsw_adjust_link, 0,
					    PHY_INTERFACE_MODE_SGMII,
					    slave);
		if (IS_ERR_OR_NULL(slave->phy)) {
			dev_err(priv->dev, "phy not found on slave %d\n",
				slave->slave_num);
			slave->phy = NULL;
		} else {
			dev_info(priv->dev, "phy found: id is: 0x%s\n",
				 dev_name(&slave->phy->dev));
			cpsw_intf->ndev->phydev = slave->phy;
			slave->phy_port_t = PORT_MII;
			phy_start(slave->phy);
		}
	}
}

static int cpsw_init_ale(struct cpsw_priv *cpsw_dev)
{
	struct cpsw_ale_params ale_params;

	memset(&ale_params, 0, sizeof(ale_params));

	ale_params.dev			= cpsw_dev->dev;
	ale_params.ale_regs		= (void *)((u32)cpsw_dev->ale_reg);
	ale_params.ale_ageout		= cpsw_dev->ale_ageout;
	ale_params.ale_entries		= cpsw_dev->ale_entries;
	ale_params.ale_ports		= cpsw_dev->ale_ports;

	cpsw_dev->ale = cpsw_ale_create(&ale_params);
	if (!cpsw_dev->ale) {
		dev_err(cpsw_dev->dev, "error initializing ale engine\n");
		return -ENODEV;
	}

	dev_info(cpsw_dev->dev, "Created a cpsw ale engine\n");

	cpsw_ale_start(cpsw_dev->ale);

	cpsw_ale_control_set(cpsw_dev->ale, 0, ALE_BYPASS,
			cpsw_dev->multi_if ? 1 : 0);

	cpsw_ale_control_set(cpsw_dev->ale, 0, ALE_NO_PORT_VLAN, 1);

	cpsw_ale_control_set(cpsw_dev->ale, cpsw_dev->host_port,
			     ALE_PORT_STATE, ALE_PORT_STATE_FORWARD);

	cpsw_ale_control_set(cpsw_dev->ale, 0,
			     ALE_PORT_UNKNOWN_VLAN_MEMBER,
			     CPSW_MASK_ALL_PORTS);

	cpsw_ale_control_set(cpsw_dev->ale, 0,
			     ALE_PORT_UNKNOWN_MCAST_FLOOD,
			     CPSW_MASK_PHYS_PORTS);

	cpsw_ale_control_set(cpsw_dev->ale, 0,
			     ALE_PORT_UNKNOWN_REG_MCAST_FLOOD,
			     CPSW_MASK_ALL_PORTS);

	cpsw_ale_control_set(cpsw_dev->ale, 0,
			     ALE_PORT_UNTAGGED_EGRESS,
			     CPSW_MASK_ALL_PORTS);

	return 0;
}

static void cpsw_init_host_port(struct cpsw_priv *priv)
{
	/* Max length register */
	__raw_writel(MAX_SIZE_STREAM_BUFFER,
		     &priv->host_port_regs->rx_maxlen);

}

/* Sliver regs memmap are contiguous but slave port regs are not */
static void cpsw_slave_init(struct cpsw_slave *slave, struct cpsw_priv *priv)
{
	void __iomem		*regs = priv->ss_regs;
	int			slave_num = slave->slave_num;
	int			slave_reg_num = slave_num;
	u32			slave_reg_ofs;

	if (slave_num > 1) {
		slave_reg_ofs = priv->slave23_reg_ofs;
		slave_reg_num -= 2;
	} else
		slave_reg_ofs = priv->slave_reg_ofs;

	slave->regs	= regs + slave_reg_ofs + (0x30 * slave_reg_num);
	slave->sliver	= regs + priv->sliver_reg_ofs + (0x40 * slave_num);
}

static void cpsw_add_mcast_addr(struct cpsw_intf *cpsw_intf, u8 *addr)
{
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;

	cpsw_ale_add_mcast(cpsw_dev->ale, addr, CPSW_MASK_ALL_PORTS, 0, 0,
			   ALE_MCAST_FWD_2);
}

static void cpsw_add_ucast_addr(struct cpsw_intf *cpsw_intf, u8 *addr)
{
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;

	cpsw_ale_add_ucast(cpsw_dev->ale, addr, cpsw_dev->host_port, 0, 0);
}

static void cpsw_del_mcast_addr(struct cpsw_intf *cpsw_intf, u8 *addr)
{
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;

	cpsw_ale_del_mcast(cpsw_dev->ale, addr, 0, 0, 0);
}

static void cpsw_del_ucast_addr(struct cpsw_intf *cpsw_intf, u8 *addr)
{
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;

	cpsw_ale_del_ucast(cpsw_dev->ale, addr, cpsw_dev->host_port, 0, 0);
}

int cpsw_add_addr(void *intf_priv, struct netcp_addr *naddr)
{
	struct cpsw_intf *cpsw_intf = intf_priv;
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;

	if (!cpsw_dev->opened)
		return -ENXIO;

	dev_dbg(cpsw_dev->dev, "ethss adding address %pM, type %d\n",
		naddr->addr, naddr->type);

	switch (naddr->type) {
	case ADDR_MCAST:
	case ADDR_BCAST:
		cpsw_add_mcast_addr(cpsw_intf, naddr->addr);
		break;
	case ADDR_UCAST:
	case ADDR_DEV:
		cpsw_add_ucast_addr(cpsw_intf, naddr->addr);
		break;
	case ADDR_ANY:
		/* nothing to do for promiscuous */
	default:
		break;
	}

	return 0;
}

int cpsw_del_addr(void *intf_priv, struct netcp_addr *naddr)
{
	struct cpsw_intf *cpsw_intf = intf_priv;
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;

	if (!cpsw_dev->opened)
		return -ENXIO;

	dev_dbg(cpsw_dev->dev, "ethss deleting address %pM, type %d\n",
		naddr->addr, naddr->type);

	switch (naddr->type) {
	case ADDR_MCAST:
	case ADDR_BCAST:
		cpsw_del_mcast_addr(cpsw_intf, naddr->addr);
		break;
	case ADDR_UCAST:
	case ADDR_DEV:
		cpsw_del_ucast_addr(cpsw_intf, naddr->addr);
		break;
	case ADDR_ANY:
		/* nothing to do for promiscuous */
	default:
		break;
	}

	return 0;
}

#ifdef CONFIG_TI_CPTS
#define KEYSTONE_PTP_FILTER \
{ \
	{OP_LDH,	0,   0, OFF_ETYPE		}, /*              */ \
/*ip4*/	{OP_JEQ,	0,  12, ETH_P_IP		}, /* f goto ip6   */ \
	{OP_LDB,	0,   0, OFF_PROTO4		}, /*              */ \
	{OP_JEQ,	0,   9, IPPROTO_UDP		}, /*              */ \
	{OP_LDH,	0,   0, OFF_FRAG		}, /*              */ \
	{OP_JSET,	7,   0, 0x1fff			}, /*              */ \
	{OP_LDX,	0,   0, OFF_IHL			}, /*              */ \
	{OP_LDHI,	0,   0, RELOFF_DST4		}, /*              */ \
	{OP_JEQ,	0,   4, PTP_EV_PORT		}, /*              */ \
	{OP_LDHI,	0,   0, ETH_HLEN + UDP_HLEN	}, /*              */ \
	{OP_AND,	0,   0, PTP_CLASS_VMASK		}, /*              */ \
	{OP_OR,		0,   0, PTP_CLASS_IPV4		}, /*              */ \
	{OP_RETA,	0,   0, 0			}, /*              */ \
	{OP_RETK,	0,   0, PTP_CLASS_NONE		}, /*              */ \
/*ip6*/	{OP_JEQ,	0,   9, ETH_P_IPV6		}, /* f goto v     */ \
	{OP_LDB,	0,   0, ETH_HLEN + OFF_NEXT	}, /*              */ \
	{OP_JEQ,	0,   6, IPPROTO_UDP		}, /*              */ \
	{OP_LDH,	0,   0, OFF_DST6		}, /*              */ \
	{OP_JEQ,	0,   4, PTP_EV_PORT		}, /*              */ \
	{OP_LDH,	0,   0, OFF_PTP6		}, /*              */ \
	{OP_AND,	0,   0, PTP_CLASS_VMASK		}, /*              */ \
	{OP_OR,		0,   0, PTP_CLASS_IPV6		}, /*              */ \
	{OP_RETA,	0,   0, 0			}, /*              */ \
	{OP_RETK,	0,   0, PTP_CLASS_NONE		}, /*              */ \
/* v */ {OP_JEQ,	0,  32, ETH_P_8021Q		}, /* f goto ptp   */ \
	{OP_LDH,	0,   0, OFF_ETYPE + 4		}, /*              */ \
/*vip4*/{OP_JEQ,	0,  12, ETH_P_IP		}, /* f goto vip6  */ \
	{OP_LDB,	0,   0, OFF_PROTO4 + 4		}, /*              */ \
	{OP_JEQ,	0,   9, IPPROTO_UDP		}, /*              */ \
	{OP_LDH,	0,   0, OFF_FRAG + 4		}, /*              */ \
	{OP_JSET,	7,   0, 0x1fff			}, /*              */ \
	{OP_LDX,	0,   0, OFF_IHL + 4		}, /*              */ \
	{OP_LDHI,	0,   0, RELOFF_DST4 + 4		}, /*              */ \
	{OP_JEQ,	0,   4, PTP_EV_PORT		}, /*              */ \
	{OP_LDHI,	0,   0, ETH_HLEN + UDP_HLEN + 4	}, /*              */ \
	{OP_AND,	0,   0, PTP_CLASS_VMASK		}, /*              */ \
	{OP_OR,		0,   0, PTP_CLASS_VLAN_IPV4	}, /*              */ \
	{OP_RETA,	0,   0, 0			}, /*              */ \
	{OP_RETK,	0,   0, PTP_CLASS_NONE		}, /*              */ \
/*vip6*/{OP_JEQ,	0,   9, ETH_P_IPV6		}, /* f goto vptp  */ \
	{OP_LDB,	0,   0, ETH_HLEN + OFF_NEXT + 4	}, /*              */ \
	{OP_JEQ,	0,   6, IPPROTO_UDP		}, /*              */ \
	{OP_LDH,	0,   0, OFF_DST6 + 4		}, /*              */ \
	{OP_JEQ,	0,   4, PTP_EV_PORT		}, /*              */ \
	{OP_LDH,	0,   0, OFF_PTP6 + 4		}, /*              */ \
	{OP_AND,	0,   0, PTP_CLASS_VMASK		}, /*              */ \
	{OP_OR,		0,   0, PTP_CLASS_VLAN_IPV6	}, /*              */ \
	{OP_RETA,	0,   0, 0			}, /*              */ \
	{OP_RETK,	0,   0, PTP_CLASS_NONE		}, /*              */ \
/*vptp*/{OP_JEQ,	0,  15, ETH_P_1588		}, /*              */ \
	{OP_LDB,	0,   0, ETH_HLEN + VLAN_HLEN	}, /*              */ \
	{OP_AND,	0,   0, PTP_GEN_BIT		}, /*              */ \
	{OP_JEQ,	0,  12, 0			}, /*              */ \
	{OP_LDH,	0,   0, ETH_HLEN + VLAN_HLEN	}, /*              */ \
	{OP_AND,	0,   0, PTP_CLASS_VMASK		}, /*              */ \
	{OP_OR,		0,   0, PTP_CLASS_VLAN		}, /*              */ \
	{OP_RETA,	0,   0, 0			}, /*              */ \
/*ptp*/	{OP_JEQ,	0,   7, ETH_P_1588		}, /*              */ \
	{OP_LDB,	0,   0, ETH_HLEN		}, /*              */ \
	{OP_AND,	0,   0, PTP_GEN_BIT		}, /*              */ \
	{OP_JEQ,	0,   4, 0			}, /*              */ \
	{OP_LDH,	0,   0, ETH_HLEN		}, /*              */ \
	{OP_AND,	0,   0, PTP_CLASS_VMASK		}, /*              */ \
	{OP_OR,		0,   0, PTP_CLASS_L2		}, /*              */ \
	{OP_RETA,	0,   0, 0			}, /*              */ \
	{OP_RETK,	0,   0, PTP_CLASS_NONE		},                    \
}

static struct sock_filter phy_ptp_filter[] = {
	PTP_FILTER
};

static struct sock_filter cpsw_ptp_filter[] = KEYSTONE_PTP_FILTER;

static void cpsw_hwtstamp(struct cpsw_intf *cpsw_intf)
{
	struct cpsw_priv *priv = cpsw_intf->cpsw_priv;
	struct cpsw_slave *slave = cpsw_intf->slaves;
	u32 ts_en, seq_id, ctl, i;

	if (!priv->cpts.tx_enable && !priv->cpts.rx_enable) {
		__raw_writel(0, &slave->regs->ts_ctl);
		return;
	}

	seq_id = (30 << CPSW_TS_SEQ_ID_OFS_SHIFT) | ETH_P_1588;
	ts_en = EVENT_MSG_BITS << CPSW_TS_MSG_TYPE_EN_SHIFT;
	ctl = ETH_P_1588 | CPSW_TS_TTL_NONZERO |
		(slave->ts_ctl.dst_port_map << CPSW_TS_CTL_DST_PORT_SHIFT) |
		(slave->ts_ctl.uni ?  CPSW_TS_UNI_EN :
			slave->ts_ctl.maddr_map << CPSW_TS_CTL_MADDR_SHIFT);

	if (priv->cpts.tx_enable)
		ts_en |= (CPSW_TS_TX_ANX_ALL_EN | CPSW_TS_TX_VLAN_LT1_EN);

	if (priv->cpts.rx_enable)
		ts_en |= (CPSW_TS_RX_ANX_ALL_EN | CPSW_TS_RX_VLAN_LT1_EN);

	for (i = 0; i < cpsw_intf->num_slaves; i++, slave++) {
		writel(ts_en, &slave->regs->ts_ctl);
		writel(seq_id, &slave->regs->ts_seq_ltype);
		writel(ctl, &slave->regs->ts_ctl_ltype2);
	}
}

static int cpsw_hwtstamp_ioctl(struct cpsw_intf *cpsw_intf, struct ifreq *ifr)
{
	struct cpsw_priv *priv = cpsw_intf->cpsw_priv;
	struct cpts *cpts = &priv->cpts;
	struct hwtstamp_config cfg;

	if (!cpts->reg)
		return -EOPNOTSUPP;

	if (copy_from_user(&cfg, ifr->ifr_data, sizeof(cfg)))
		return -EFAULT;

	/* reserved for future extensions */
	if (cfg.flags)
		return -EINVAL;

	switch (cfg.tx_type) {
	case HWTSTAMP_TX_OFF:
		cpts->tx_enable = 0;
		break;
	case HWTSTAMP_TX_ON:
		cpts->tx_enable = 1;
		break;
	default:
		return -ERANGE;
	}

	switch (cfg.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		cpts->rx_enable = 0;
		break;
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		cpts->rx_enable = 1;
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_EVENT;
		break;
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		cpts->rx_enable = 1;
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		break;
	default:
		return -ERANGE;
	}

	cpsw_hwtstamp(cpsw_intf);

	return copy_to_user(ifr->ifr_data, &cfg, sizeof(cfg)) ? -EFAULT : 0;
}
#else
static inline int cpsw_hwtstamp_ioctl(struct cpsw_intf *cpsw_intf,
					struct ifreq *ifr)
{
	return -EOPNOTSUPP;
}
#endif /*CONFIG_TI_CPTS*/

int cpsw_ioctl(void *intf_priv, struct ifreq *req, int cmd)
{
	struct cpsw_intf *cpsw_intf = intf_priv;
	struct cpsw_slave *slave = cpsw_intf->slaves;
	struct phy_device *phy = slave->phy;
	int ret = -EOPNOTSUPP;

	if (cpsw_intf->cpsw_priv->force_no_hwtstamp)
		return -EOPNOTSUPP;

	if (phy)
		ret = phy_mii_ioctl(phy, req, cmd);

	if ((cmd == SIOCSHWTSTAMP) && (ret == -EOPNOTSUPP))
		ret = cpsw_hwtstamp_ioctl(cpsw_intf, req);

	return ret;
}

static void cpsw_timer(unsigned long arg)
{
	struct cpsw_intf *cpsw_intf = (struct cpsw_intf *)arg;
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;
	u32 sp = cpsw_intf->slave_port;
	u32 ns = cpsw_intf->num_slaves;
	u32 link_state;

	if (cpsw_dev->multi_if)
		link_state = keystone_sgmii_get_port_link(SGMII_BASE(sp), sp);
	else {
		/* Single interface mode. Link is up if any one slave
		 * port is up.  It assumes slave port always starts from
		 * 0 and is consecutive.
		 */

		/* slave port 2, 3 status */
		link_state = keystone_sgmii_link_status(SGMII_BASE(2),
						   max_t(u32, ns, 2) - 2);

		link_state <<= 2;

		/* slave port 0, 1 status */
		link_state |= keystone_sgmii_link_status(SGMII_BASE(0),
						   min_t(u32, ns, 2));
	}

	cpsw_intf->link_state = link_state;

	/* if MAC-to-PHY, check phy link status also
	 * to conclude the intf link's status
	 */
	for_each_slave(cpsw_intf, cpsw_slave_link, cpsw_intf);

	/* Is this the right logic?
	 *  multi_if & MAC_PHY: phy state machine already reported carrier
	 *  multi_if & !MAC_PHY: report carrier
	 * !multi_if: any one slave up means intf is up, reporting carrier
	 *            here corrects what phy state machine (if it exists)
	 *            might have reported.
	 */
	if (!cpsw_dev->multi_if ||
	    (cpsw_dev->multi_if &&
	     !IS_SGMII_MAC_PHY(cpsw_intf->slaves->link_interface))) {
		if (cpsw_intf->link_state)
			netif_carrier_on(cpsw_intf->ndev);
		else
			netif_carrier_off(cpsw_intf->ndev);
	}

	/* A timer runs as a BH, no need to block them */
	spin_lock(&cpsw_dev->hw_stats_lock);
	cpsw_update_stats(cpsw_dev, NULL);
	spin_unlock(&cpsw_dev->hw_stats_lock);

	cpsw_intf->timer.expires = jiffies + (HZ/10);
	add_timer(&cpsw_intf->timer);

	return;
}

#ifdef CONFIG_TI_CPTS
#define PHY_TXTSTAMP(p)				\
		(p->skb->dev &&			\
		 p->skb->dev->phydev &&		\
		 p->skb->dev->phydev->drv &&	\
		 p->skb->dev->phydev->drv->txtstamp)

#define PHY_RXTSTAMP(p)				\
		(p->skb->dev &&			\
		 p->skb->dev->phydev &&		\
		 p->skb->dev->phydev->drv &&	\
		 p->skb->dev->phydev->drv->rxtstamp)

static bool phy_ptp_tstamp(const struct netcp_packet *p_info, bool is_tx)
{
	struct sk_buff *skb = p_info->skb;
	unsigned type = PTP_CLASS_NONE;

	if (is_tx && likely(PHY_TXTSTAMP(p_info)))
		type = sk_run_filter(skb, phy_ptp_filter);
	else if (!is_tx && likely(PHY_RXTSTAMP(p_info)))
		type = sk_run_filter(skb, phy_ptp_filter);
	else
		return false;

	switch (type) {
	case PTP_CLASS_V1_IPV4:
	case PTP_CLASS_V1_IPV6:
	case PTP_CLASS_V2_IPV4:
	case PTP_CLASS_V2_IPV6:
	case PTP_CLASS_V2_L2:
	case PTP_CLASS_V2_VLAN:
		return true;
	}

	return false;
}

static int cpsw_txtstamp_complete(void *context, struct sk_buff *skb)
{
	struct cpsw_intf *cpsw_intf = context;
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;

	return cpts_tx_timestamp(&cpsw_dev->cpts, skb);
}

static bool cpsw_cpts_txtstamp(struct cpsw_intf *cpsw_intf,
				const struct netcp_packet *p_info)
{
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;
	struct sk_buff *skb = p_info->skb;
	unsigned type = PTP_CLASS_NONE;

	if (!cpsw_dev->cpts.tx_enable)
		return false;

	type = sk_run_filter(skb, cpsw_dev->cpts.filter);
	switch (type) {
	case PTP_CLASS_V1_IPV4:
	case PTP_CLASS_V1_IPV6:
	case PTP_CLASS_V2_IPV4:
	case PTP_CLASS_V2_IPV6:
	case PTP_CLASS_V2_L2:
	case PTP_CLASS_V2_VLAN:
	case PTP_CLASS_V1_VLAN_IPV4:
	case PTP_CLASS_V2_VLAN_IPV4:
	case PTP_CLASS_V1_VLAN_IPV6:
	case PTP_CLASS_V2_VLAN_IPV6:
		return true;
	}

	return false;
}

int cpsw_mark_pkt_txtstamp(struct cpsw_intf *cpsw_intf,
			struct netcp_packet *p_info)
{
	if (!(skb_shinfo(p_info->skb)->tx_flags & SKBTX_HW_TSTAMP))
		return 0;

	if (phy_ptp_tstamp(p_info, true)) {
		skb_shinfo(p_info->skb)->tx_flags |= SKBTX_IN_PROGRESS;
		return 0;
	}

	if (cpsw_cpts_txtstamp(cpsw_intf, p_info)) {
		p_info->txtstamp_complete = cpsw_txtstamp_complete;
		p_info->ts_context = (void *)cpsw_intf;
		skb_shinfo(p_info->skb)->tx_flags |= SKBTX_IN_PROGRESS;
	}

	return 0;
}

static int cpsw_rxtstamp_complete(struct cpsw_intf *cpsw_intf,
				struct netcp_packet *p_info)
{
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;

	if (p_info->rxtstamp_complete)
		return 0;

	if (phy_ptp_tstamp(p_info, false)) {
		p_info->rxtstamp_complete = true;
		return 0;
	}

	if (!cpts_rx_timestamp(&cpsw_dev->cpts, p_info->skb))
		p_info->rxtstamp_complete = true;

	return 0;
}

static inline void cpsw_register_cpts(struct cpsw_priv *cpsw_dev)
{
	if (!cpsw_dev->cpts.reg)
		return;

	if (cpsw_dev->cpts_registered < 0)
		/* Should not happen */
		return;

	if (cpsw_dev->cpts_registered > 0)
		goto done;

	if (ptp_filter_init(phy_ptp_filter, ARRAY_SIZE(phy_ptp_filter))) {
		dev_err(cpsw_dev->dev, "bad ptp filter\n");
		return;
	}

	cpsw_dev->cpts.filter = cpsw_ptp_filter;
	cpsw_dev->cpts.filter_size = ARRAY_SIZE(cpsw_ptp_filter);

	/* Let cpts calculate the mult and shift */
	if (cpts_register(cpsw_dev->dev, &cpsw_dev->cpts,
			  cpsw_dev->cpts.cc.mult, cpsw_dev->cpts.cc.shift)) {
		dev_err(cpsw_dev->dev, "error registering cpts device\n");
		return;
	}

done:
	++cpsw_dev->cpts_registered;
}

static inline void cpsw_unregister_cpts(struct cpsw_priv *cpsw_dev)
{
	if (!cpsw_dev->cpts.reg)
		return;

	if (cpsw_dev->cpts_registered <= 0)
		return;

	--cpsw_dev->cpts_registered;

	if (cpsw_dev->cpts_registered)
		return;

	cpsw_dev->cpts.filter = 0;
	cpsw_dev->cpts.filter_size = 0;
	cpts_unregister(&cpsw_dev->cpts);
}

static void cpsw_update_cpts_dt_params(struct cpsw_priv *cpsw_dev,
				  struct device_node *node)
{
	int ret;

	ret = of_property_read_u32(node, "cpts_reg_ofs",
				   &cpsw_dev->cpts_reg_ofs);
	if (ret < 0)
		dev_err(cpsw_dev->dev,
			"missing cpts reg offset, err %d\n", ret);

	ret = of_property_read_u32(node, "cpts_rftclk_sel",
				   &cpsw_dev->cpts.rftclk_sel);
	if (ret < 0) {
		dev_err(cpsw_dev->dev,
			"missing cpts rftclk_sel, err %d\n", ret);
		cpsw_dev->cpts.rftclk_sel = 0;
	}

	ret = of_property_read_u32(node, "cpts_rftclk_freq",
				   &cpsw_dev->cpts.rftclk_freq);
	if (ret < 0) {
		dev_vdbg(cpsw_dev->dev, "cpts rftclk freq not defined\n");
		cpsw_dev->cpts.rftclk_freq = 0;
	}

	ret = of_property_read_u32(node, "cpts_ts_comp_length",
				&cpsw_dev->cpts.ts_comp_length);
	if (ret < 0) {
		dev_err(cpsw_dev->dev,
			"missing cpts ts_comp length, err %d\n", ret);
		cpsw_dev->cpts.ts_comp_length = 1;
	}

	if (of_property_read_u32(node, "cpts_ts_comp_polarity",
				&cpsw_dev->cpts.ts_comp_polarity))
		cpsw_dev->cpts.ts_comp_polarity = 1;

	if (of_property_read_u32(node, "cpts_clock_mult",
				&cpsw_dev->cpts.cc.mult)) {
		dev_err(cpsw_dev->dev,
			"Missing cpts_clock_mult property in the DT.\n");
		cpsw_dev->cpts.cc.mult = 0;
	}

	if (of_property_read_u32(node, "cpts_clock_shift",
				&cpsw_dev->cpts.cc.shift)) {
		dev_err(cpsw_dev->dev,
			"Missing cpts_clock_shift property in the DT.\n");
		cpsw_dev->cpts.cc.shift = 0;
	}

	if (of_property_read_u32(node, "cpts_clock_div",
				&cpsw_dev->cpts.cc_div)) {
		dev_err(cpsw_dev->dev,
			"Missing cpts_clock_div property in the DT.\n");
		cpsw_dev->cpts.cc_div = 1;
	}

	cpsw_dev->cpts.ignore_adjfreq =
		of_property_read_bool(node, "cpts-ignore-adjfreq");

	ret = of_property_read_u32(node, "num_slaves", &cpsw_dev->num_slaves);
	if (ret < 0) {
		dev_err(cpsw_dev->dev,
			"missing num_slaves parameter, err %d\n", ret);
		cpsw_dev->num_slaves = 2;
	}
}
#else
static inline int cpsw_mark_pkt_txtstamp(struct cpsw_intf *cpsw_intf,
					struct netcp_packet *p_info)
{
	return 0;
}

static inline int cpsw_rxtstamp_complete(struct cpsw_intf *cpsw_intf,
					struct netcp_packet *p_info)
{
	return 0;
}

static inline void cpsw_register_cpts(struct cpsw_priv *cpsw_dev)
{
}

static inline void cpsw_unregister_cpts(struct cpsw_priv *cpsw_dev)
{
}

static void cpsw_update_cpts_dt_params(struct cpsw_priv *cpsw_dev,
				  struct device_node *node)
{
}
#endif /* CONFIG_TI_CPTS */

static int cpsw_serdes_init(struct cpsw_priv *cpsw_dev)
{
	int i, total_slaves, slaves;
	int ret = 0;

	for (i = 0, total_slaves = cpsw_dev->num_slaves;
	     i < cpsw_dev->num_serdes;
	     i++, total_slaves -= cpsw_dev->serdes_lanes) {
		if (total_slaves <= 0)
			break;

		if (total_slaves > cpsw_dev->serdes_lanes)
			slaves = cpsw_dev->serdes_lanes;
		else
			slaves = total_slaves;

		serdes_reset(cpsw_dev->serdes_regs[i], slaves);
		ret = serdes_init(cpsw_dev->serdes_regs[i], &cpsw_dev->serdes,
				  slaves);
		if (ret < 0) {
			dev_err(cpsw_dev->dev,
				"cpsw serdes initialization failed\n");
			break;
		}
	}
	return ret;
}

static int cpsw_tx_hook(int order, void *data, struct netcp_packet *p_info)
{
	struct cpsw_intf *cpsw_intf = data;

	p_info->tx_pipe = &cpsw_intf->tx_pipe;

	return cpsw_mark_pkt_txtstamp(cpsw_intf, p_info);
}

static int cpsw_rx_hook(int order, void *data, struct netcp_packet *p_info)
{
	struct cpsw_intf *cpsw_intf = data;

	return cpsw_rxtstamp_complete(cpsw_intf, p_info);
}

#define	CPSW_TXHOOK_ORDER	0
#define	CPSW_RXHOOK_ORDER	0

static int cpsw_open(void *intf_priv, struct net_device *ndev)
{
	struct cpsw_intf *cpsw_intf = intf_priv;
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;
	struct netcp_priv *netcp = netdev_priv(ndev);
	int ret = 0;
	u32 reg;

	cpsw_dev->cpgmac = clk_get(cpsw_dev->dev, "clk_cpgmac");
	if (IS_ERR(cpsw_dev->cpgmac)) {
		ret = PTR_ERR(cpsw_dev->cpgmac);
		cpsw_dev->cpgmac = NULL;
		dev_err(cpsw_dev->dev, "unable to get Keystone CPGMAC"
			" clock: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(cpsw_dev->cpgmac);
	if (ret)
		goto clk_fail;

	reg = __raw_readl(&cpsw_dev->regs->id_ver);

	dev_info(cpsw_dev->dev, "initializing cpsw version %d.%d (%d) "
		 "SGMII identification value 0x%x\n",
		 CPSW_MAJOR_VERSION(reg), CPSW_MINOR_VERSION(reg),
		 CPSW_RTL_VERSION(reg), CPSW_SGMII_IDENT(reg));

	ret = netcp_txpipe_open(&cpsw_intf->tx_pipe);
	if (ret)
		goto txpipe_fail;

	dev_dbg(cpsw_dev->dev, "opened TX channel %s: %p with psflags %d\n",
		cpsw_intf->tx_pipe.dma_chan_name,
		cpsw_intf->tx_pipe.dma_channel,
		cpsw_intf->tx_pipe.dma_psflags);

	if (atomic_inc_return(&cpsw_dev->ale_refcnt) == 1) {
		ret = cpsw_init_ale(cpsw_dev);
		if (ret < 0) {
			atomic_dec(&cpsw_dev->ale_refcnt);
			goto ale_fail;
		}
		cpsw_init_host_port(cpsw_dev);
	}

	for_each_slave(cpsw_intf, cpsw_slave_init, cpsw_dev);

	for_each_slave(cpsw_intf, cpsw_slave_stop, cpsw_intf);

	/* Serdes init */
	if (cpsw_dev->init_serdes_at_probe == 0)
		cpsw_serdes_init(cpsw_dev);

	/* disable priority elevation and enable statistics on all ports */
	__raw_writel(0, &cpsw_dev->regs->ptype);

	/* Control register */
	__raw_writel(CPSW_CTL_P0_ENABLE, &cpsw_dev->regs->control);

	/* All statistics enabled and STAT AB visible by default */
	__raw_writel(CPSW_REG_VAL_STAT_ENABLE_ALL,
		     &cpsw_dev->regs->stat_port_en);

	/* initialize slave ports */
	for_each_slave(cpsw_intf, cpsw_slave_open, cpsw_intf);

	init_timer(&cpsw_intf->timer);
	cpsw_intf->timer.data		= (unsigned long)cpsw_intf;
	cpsw_intf->timer.function	= cpsw_timer;
	cpsw_intf->timer.expires	= jiffies + CPSW_TIMER_INTERVAL;
	add_timer(&cpsw_intf->timer);
	dev_dbg(cpsw_dev->dev, "%s(): cpsw_timer = %p\n", __func__, cpsw_timer);

	netcp_register_txhook(netcp, CPSW_TXHOOK_ORDER,
			      cpsw_tx_hook, cpsw_intf);

	if(!cpsw_dev->force_no_hwtstamp)
		netcp_register_rxhook(netcp, CPSW_RXHOOK_ORDER,
				      cpsw_rx_hook, cpsw_intf);

	/* Configure the streaming switch */
#define	PSTREAM_ROUTE_DMA	6
	netcp_set_streaming_switch(cpsw_dev->netcp_device, netcp->cpsw_port,
				   PSTREAM_ROUTE_DMA);

	cpsw_register_cpts(cpsw_dev);
	cpsw_dev->opened = 1;
	return 0;

ale_fail:
	netcp_txpipe_close(&cpsw_intf->tx_pipe);
txpipe_fail:
	clk_disable_unprepare(cpsw_dev->cpgmac);
clk_fail:
	clk_put(cpsw_dev->cpgmac);
	cpsw_dev->cpgmac = NULL;
	return ret;
}

static int cpsw_close(void *intf_priv, struct net_device *ndev)
{
	struct cpsw_intf *cpsw_intf = intf_priv;
	struct cpsw_priv *cpsw_dev = cpsw_intf->cpsw_priv;
	struct netcp_priv *netcp = netdev_priv(ndev);

	del_timer_sync(&cpsw_intf->timer);

	for_each_slave(cpsw_intf, cpsw_slave_stop, cpsw_intf);

	if (atomic_dec_return(&cpsw_dev->ale_refcnt) == 0) {
		cpsw_ale_destroy(cpsw_dev->ale);
		cpsw_dev->ale = NULL;
	}

	if(!cpsw_dev->force_no_hwtstamp)
		netcp_unregister_rxhook(netcp, CPSW_RXHOOK_ORDER,
				      cpsw_rx_hook, cpsw_intf);

	netcp_unregister_txhook(netcp, CPSW_TXHOOK_ORDER, cpsw_tx_hook,
				cpsw_intf);
	netcp_txpipe_close(&cpsw_intf->tx_pipe);

	clk_disable_unprepare(cpsw_dev->cpgmac);
	clk_put(cpsw_dev->cpgmac);

	cpsw_unregister_cpts(cpsw_dev);
	cpsw_dev->opened = 0;
	return 0;
}

static int cpsw_remove(struct netcp_device *netcp_device, void *inst_priv)
{
	struct cpsw_priv *cpsw_dev = inst_priv;
	struct cpsw_intf *cpsw_intf, *tmp;
	int i;

	of_node_put(cpsw_dev->interfaces);

	list_for_each_entry_safe(cpsw_intf, tmp, &cpsw_dev->cpsw_intf_head,
				 cpsw_intf_list) {
		netcp_delete_interface(netcp_device, cpsw_intf->ndev);
	}
	BUG_ON(!list_empty(&cpsw_dev->cpsw_intf_head));

	iounmap(cpsw_dev->ss_regs);
	for (i = 0; i < cpsw_dev->num_serdes; i++)
		if (cpsw_dev->serdes_regs[i])
			iounmap(cpsw_dev->serdes_regs[i]);
	memset(cpsw_dev, 0x00, sizeof(*cpsw_dev));	/* FIXME: Poison */
	kfree(cpsw_dev);
	return 0;
}

static int init_slave(struct cpsw_priv *cpsw_dev,
		      struct device_node *node, int slave_num)
{
	int ret = 0;

	ret = of_property_read_u32(node, "link-interface",
				   &cpsw_dev->link[slave_num]);
	if (ret < 0) {
		dev_err(cpsw_dev->dev,
			"missing link-interface value"
			"defaulting to mac-phy link\n");
		cpsw_dev->link[slave_num] = 1;
	}

	cpsw_dev->phy_node[slave_num] = of_parse_phandle(node, "phy-handle", 0);

	return 0;
}

static int cpsw_create_cpts_sysfs(struct cpsw_priv *cpsw_dev)
{
	struct kobject *pts_kobj;
	char *port_name[] = {"1", "2", "3", "4", NULL};
	int i, ret;

	pts_kobj = kobject_create_and_add("port_ts",
			kobject_get(&cpsw_dev->kobj));
	if (!pts_kobj) {
		dev_err(cpsw_dev->dev,
			"failed to create sysfs port_ts entry\n");
		kobject_put(&cpsw_dev->kobj);
		return -ENOMEM;
	}

	for (i = 0; (i < cpsw_dev->num_slaves) && port_name[i]; i++) {
		ret = kobject_init_and_add(&cpsw_dev->port_ts_kobj[i],
			&cpsw_pts_n_ktype, kobject_get(pts_kobj), port_name[i]);

		if (ret) {
			dev_err(cpsw_dev->dev,
				"failed to create sysfs port_ts/%s entry\n",
				port_name[i]);
			kobject_put(&cpsw_dev->port_ts_kobj[i]);
			kobject_put(pts_kobj);
			return ret;
		}
	}

	return 0;
}

static int cpsw_create_sysfs_entries(struct cpsw_priv *cpsw_dev)
{
	struct device *dev = cpsw_dev->dev;
	int ret;

	ret = kobject_init_and_add(&cpsw_dev->kobj, &cpsw_ktype,
		kobject_get(&dev->kobj), "cpsw");

	if (ret) {
		dev_err(dev, "failed to create cpsw sysfs entry\n");
		kobject_put(&cpsw_dev->kobj);
		kobject_put(&dev->kobj);
		return ret;
	}

	ret = kobject_init_and_add(&cpsw_dev->tx_pri_kobj,
		&cpsw_tx_pri_ktype,
		kobject_get(&cpsw_dev->kobj), "port_tx_pri_map");

	if (ret) {
		dev_err(dev, "failed to create sysfs port_tx_pri_map entry\n");
		kobject_put(&cpsw_dev->tx_pri_kobj);
		kobject_put(&cpsw_dev->kobj);
		return ret;
	}

	ret = kobject_init_and_add(&cpsw_dev->pvlan_kobj,
		&cpsw_pvlan_ktype,
		kobject_get(&cpsw_dev->kobj), "port_vlan");

	if (ret) {
		dev_err(dev, "failed to create sysfs port_vlan entry\n");
		kobject_put(&cpsw_dev->pvlan_kobj);
		kobject_put(&cpsw_dev->kobj);
		return ret;
	}

	ret = cpsw_create_cpts_sysfs(cpsw_dev);
	if (ret)
		return ret;

	ret = kobject_init_and_add(&cpsw_dev->stats_kobj,
		&cpsw_stats_ktype,
		kobject_get(&cpsw_dev->kobj), "stats");

	if (ret) {
		dev_err(dev, "failed to create sysfs stats entry\n");
		kobject_put(&cpsw_dev->stats_kobj);
		kobject_put(&cpsw_dev->kobj);
		return ret;
	}

	return 0;
}

static int cpsw_probe(struct netcp_device *netcp_device,
			struct device *dev,
			struct device_node *node,
			void **inst_priv)
{
	struct cpsw_priv *cpsw_dev;
	struct device_node *slaves, *slave, *interfaces;
	void __iomem *regs;
	struct net_device *ndev;
	int slave_num = 0;
	int i, ret = 0;
	u32 temp[8];

	if (!node) {
		dev_err(dev, "device tree info unavailable\n");
		return -ENODEV;
	}

	cpsw_dev = devm_kzalloc(dev, sizeof(struct cpsw_priv), GFP_KERNEL);
	if (!cpsw_dev) {
		dev_err(dev, "cpsw_dev memory allocation failed\n");
		return -ENOMEM;
	}
	*inst_priv = cpsw_dev;
	dev_dbg(dev, "%s(): cpsw_priv = %p\n", __func__, cpsw_dev);

	cpsw_dev->dev = dev;
	cpsw_dev->netcp_device = netcp_device;

	global_priv = cpsw_dev;	/* FIXME: Remove this!! */

	ret = of_property_read_u32(node, "num_serdes",
				   &cpsw_dev->num_serdes);
	if (ret < 0) {
		dev_err(dev, "missing num_serdes parameter\n");
		cpsw_dev->num_serdes = CPSW_SERDES_MAX_NUM;
	}
	dev_dbg(dev, "serdes_ref_clk %u\n", cpsw_dev->num_serdes);

	ret = of_property_read_u32(node, "serdes_lanes",
				   &cpsw_dev->serdes_lanes);
	if (ret < 0) {
		dev_err(dev, "missing serdes_lanes parameter\n");
		cpsw_dev->serdes_lanes = CPSW_LANE_NUM_PER_SERDES;
	}
	dev_dbg(dev, "serdes_lanes %u\n", cpsw_dev->serdes_lanes);

	if (of_property_read_u32_array(node, "serdes_reg", (u32 *)&(temp[0]),
					cpsw_dev->num_serdes * 2)) {
		dev_err(dev, "No serdes regs defined\n");
		ret = -ENODEV;
		goto exit;
	}

	for (i = 0; i < cpsw_dev->num_serdes; i++) {
		cpsw_dev->serdes_regs[i] = ioremap(temp[i*2], temp[i*2+1]);
		if (!cpsw_dev->serdes_regs[i]) {
			dev_err(dev, "can't map serdes regs\n");
			ret = -ENOMEM;
			goto exit;
		}
	}

	ret = of_property_read_u32(node, "serdes_ref_clk",
				   &cpsw_dev->serdes.clk);
	if (ret < 0) {
		dev_err(dev, "missing serdes_ref_clk parameter\n");
		cpsw_dev->serdes.clk = SERDES_CLOCK_156P25M;
	}
	dev_dbg(dev, "serdes_ref_clk %u\n", cpsw_dev->serdes.clk);

	ret = of_property_read_u32(node, "serdes_baud_rate",
				   &cpsw_dev->serdes.rate);
	if (ret < 0) {
		dev_err(dev, "missing serdes_baud_rate parameter\n");
		cpsw_dev->serdes.rate = SERDES_RATE_5G;
	}
	dev_dbg(dev, "serdes_baud_rate %u\n", cpsw_dev->serdes.rate);

	ret = of_property_read_u32(node, "serdes_rate_mode",
				   &cpsw_dev->serdes.rate_mode);
	if (ret < 0) {
		dev_err(dev, "missing serdes_rate_mode parameter\n");
		cpsw_dev->serdes.rate_mode = SERDES_QUARTER_RATE;
	}
	dev_dbg(dev, "serdes_rate_mode %u\n", cpsw_dev->serdes.rate_mode);

	ret = of_property_read_u32(node, "serdes_phy_intf",
				   &cpsw_dev->serdes.intf);
	if (ret < 0) {
		dev_err(dev, "missing serdes_phy_intf parameter\n");
		cpsw_dev->serdes.intf = SERDES_PHY_SGMII;
	}
	dev_dbg(dev, "serdes_phy_intf %u\n", cpsw_dev->serdes.intf);

	ret = of_property_read_u32(node, "serdes_loopback",
				   &cpsw_dev->serdes.loopback);
	if (ret < 0) {
		dev_err(dev, "missing serdes_loopback parameter\n");
		cpsw_dev->serdes.loopback = 0;
	}
	dev_dbg(dev, "serdes_loopback %u\n", cpsw_dev->serdes.loopback);

	ret = of_property_read_u32(node, "serdes_at_probe",
				   &cpsw_dev->init_serdes_at_probe);
	if (ret < 0) {
		dev_err(dev, "missing serdes_at_probe parameter\n");
		cpsw_dev->init_serdes_at_probe = 0;
	}
	dev_dbg(dev, "serdes_at_probe %u\n", cpsw_dev->init_serdes_at_probe);
	if (cpsw_dev->init_serdes_at_probe == 1)
		cpsw_serdes_init(cpsw_dev);

	ret = of_property_read_u32(node, "num_slaves", &cpsw_dev->num_slaves);
	if (ret < 0) {
		dev_err(dev, "missing num_slaves parameter, err %d\n", ret);
		cpsw_dev->num_slaves = 2;
	}

	ret = of_property_read_u32(node, "sgmii_module_ofs",
				   &cpsw_dev->sgmii_module_ofs);
	if (ret < 0)
		dev_err(dev, "missing sgmii module offset, err %d\n", ret);

	ret = of_property_read_u32(node, "sgmii_module34_ofs",
				   &cpsw_dev->sgmii_module34_ofs);
	if (ret < 0)
		dev_err(dev, "missing sgmii module34 offset, err %d\n", ret);

	ret = of_property_read_u32(node, "switch_module_ofs",
				   &cpsw_dev->switch_module_ofs);
	if (ret < 0)
		dev_err(dev, "missing switch module offset, err %d\n", ret);

	ret = of_property_read_u32(node, "host_port_reg_ofs",
				   &cpsw_dev->host_port_reg_ofs);
	if (ret < 0)
		dev_err(dev, "missing host port reg offset, err %d\n", ret);

	ret = of_property_read_u32(node, "slave_reg_ofs",
				   &cpsw_dev->slave_reg_ofs);
	if (ret < 0)
		dev_err(dev, "missing slave reg offset, err %d\n", ret);

	ret = of_property_read_u32(node, "sliver_reg_ofs",
				   &cpsw_dev->sliver_reg_ofs);
	if (ret < 0)
		dev_err(dev, "missing sliver reg offset, err %d\n", ret);

	ret = of_property_read_u32(node, "slave23_reg_ofs",
				   &cpsw_dev->slave23_reg_ofs);
	if (ret < 0)
		dev_err(dev, "missing slave23 reg offset, err %d\n", ret);

	ret = of_property_read_u32(node, "hw_stats_reg_ofs",
				   &cpsw_dev->hw_stats_reg_ofs);
	if (ret < 0)
		dev_err(dev, "missing hw stats reg offset, err %d\n", ret);

	ret = of_property_read_u32(node, "ale_reg_ofs",
				   &cpsw_dev->ale_reg_ofs);
	if (ret < 0)
		dev_err(dev, "missing ale reg offset, err %d\n", ret);

	cpsw_update_cpts_dt_params(cpsw_dev, node);

	ret = of_property_read_u32(node, "ale_ageout", &cpsw_dev->ale_ageout);
	if (ret < 0) {
		dev_err(dev, "missing ale_ageout parameter, err %d\n", ret);
		cpsw_dev->ale_ageout = 10;
	}

	ret = of_property_read_u32(node, "ale_entries", &cpsw_dev->ale_entries);
	if (ret < 0) {
		dev_err(dev, "missing ale_entries parameter, err %d\n", ret);
		cpsw_dev->ale_entries = 1024;
	}

	ret = of_property_read_u32(node, "ale_ports", &cpsw_dev->ale_ports);
	if (ret < 0) {
		dev_err(dev, "missing ale_ports parameter, err %d\n", ret);
		cpsw_dev->ale_ports = 2;
	}

	ret = of_property_read_u32(node, "intf_tx_queues", &cpsw_dev->intf_tx_queues);
	if (ret < 0) {
		dev_err(dev, "missing intf_tx_queues parameter, err %d\n", ret);
		cpsw_dev->intf_tx_queues = 1;
	}

	if (of_find_property(node, "force_no_hwtstamp", NULL)) {
		cpsw_dev->force_no_hwtstamp = 1;
		dev_warn(dev, "***** No CPSW or PHY timestamping *****\n");
	}

	if (of_find_property(node, "multi-interface", NULL))
		cpsw_dev->multi_if = 1;

	ret = of_property_read_u32(node, "num-interfaces",
				   &cpsw_dev->num_interfaces);
	if (ret < 0) {
		dev_err(dev, "missing num-interfaces parameter\n");
		cpsw_dev->num_interfaces = 1;
	}

	ret = of_property_read_u32(node, "slaves-per-interface",
				   &cpsw_dev->slaves_per_interface);
	if (ret < 0) {
		dev_err(dev, "missing slaves-per_interface parameter\n");
		cpsw_dev->slaves_per_interface = 2;
	}

	/* FIXME: TCI6614_SS_BASE needs to come from the device tree */
	regs = ioremap(TCI6614_SS_BASE, 0xf00);
	BUG_ON(!regs);

	cpsw_dev->ss_regs = regs;
	cpsw_dev->sgmii_port_regs	= regs + cpsw_dev->sgmii_module_ofs;
	cpsw_dev->sgmii_port34_regs	= regs + cpsw_dev->sgmii_module34_ofs;
	cpsw_dev->regs = regs + cpsw_dev->switch_module_ofs;
	cpsw_dev->host_port_regs = regs + cpsw_dev->host_port_reg_ofs;
	cpsw_dev->hw_stats_regs[0] = regs + cpsw_dev->hw_stats_reg_ofs;
	cpsw_dev->hw_stats_regs[1] = regs + cpsw_dev->hw_stats_reg_ofs + 0x100;
	cpsw_dev->ale_reg	  = regs + cpsw_dev->ale_reg_ofs;
	if (cpsw_dev->cpts_reg_ofs)
		cpsw_dev->cpts.reg = regs + cpsw_dev->cpts_reg_ofs;

	cpsw_dev->host_port = 0;
	cpsw_dev->rx_packet_max = NETCP_MAX_FRAME_SIZE;

	dev_dbg(dev, "num_slaves = %d\n", cpsw_dev->num_slaves);
	dev_dbg(dev, "ale_ageout = %d\n", cpsw_dev->ale_ageout);
	dev_dbg(dev, "ale_entries = %d\n", cpsw_dev->ale_entries);
	dev_dbg(dev, "ale_ports = %d\n", cpsw_dev->ale_ports);

	slaves = of_get_child_by_name(node, "slaves");
	if (!slaves) {
		dev_err(dev, "could not find slaves\n");
		ret = -ENODEV;
		goto exit;
	}

	for_each_child_of_node(slaves, slave) {
			init_slave(cpsw_dev, slave, slave_num);
			slave_num++;
	}

	of_node_put(slaves);

	interfaces = of_get_child_by_name(node, "interfaces");
	if (!interfaces)
		dev_err(dev, "could not find interfaces\n");

	cpsw_dev->interfaces = interfaces;

	/* Create the interface */
	INIT_LIST_HEAD(&cpsw_dev->cpsw_intf_head);
	if (cpsw_dev->multi_if)
		for (i = 0; i < cpsw_dev->num_interfaces; i++)
			netcp_create_interface(netcp_device, &ndev,
					       NULL, cpsw_dev->intf_tx_queues,
					       1, (i + 1));
	else
		netcp_create_interface(netcp_device, &ndev,
					       NULL, cpsw_dev->intf_tx_queues,
					       1, 0);

	/* init the hw stats */
	spin_lock_init(&cpsw_dev->hw_stats_lock);
	spin_lock_bh(&cpsw_dev->hw_stats_lock);
	cpsw_reset_mod_stats(cpsw_dev, CPSW_STATSA_MODULE);
	cpsw_reset_mod_stats(cpsw_dev, CPSW_STATSB_MODULE);
	cpsw_reset_mod_stats(cpsw_dev, CPSW_STATSC_MODULE);
	cpsw_reset_mod_stats(cpsw_dev, CPSW_STATSD_MODULE);
	spin_unlock_bh(&cpsw_dev->hw_stats_lock);

	ret = cpsw_create_sysfs_entries(cpsw_dev);
	if (ret)
		goto exit;

	return 0;

exit:
	if (cpsw_dev->ss_regs)
		iounmap(cpsw_dev->ss_regs);

	for (i = 0; i < cpsw_dev->num_serdes; i++)
		if (cpsw_dev->serdes_regs[i])
			iounmap(cpsw_dev->serdes_regs[i]);

	*inst_priv = NULL;
	kfree(cpsw_dev);
	return ret;
}

static void cpsw_slave_cpts_ctl_init(struct cpsw_slave *slave)
{
	slave->ts_ctl.uni = 1;
	slave->ts_ctl.dst_port_map =
		(CPSW_TS_CTL_DST_PORT >> CPSW_TS_CTL_DST_PORT_SHIFT) & 0x3;
	slave->ts_ctl.maddr_map =
		(CPSW_TS_CTL_MADDR_ALL >> CPSW_TS_CTL_MADDR_SHIFT) & 0x1f;
}

static int cpsw_attach(void *inst_priv, struct net_device *ndev,
		       void **intf_priv)
{
	struct cpsw_priv *cpsw_dev = inst_priv;
	struct cpsw_intf *cpsw_intf;
	struct netcp_priv *netcp = netdev_priv(ndev);
	struct device_node *interface;
	int i = 0, ret = 0;
	char node_name[24];

	cpsw_intf = devm_kzalloc(cpsw_dev->dev,
				 sizeof(struct cpsw_intf), GFP_KERNEL);
	if (!cpsw_intf) {
		dev_err(cpsw_dev->dev, "cpsw interface memory "
			"allocation failed\n");
		return -ENOMEM;
	}
	cpsw_intf->ndev = ndev;
	cpsw_intf->dev = cpsw_dev->dev;
	cpsw_intf->cpsw_priv = cpsw_dev;
	cpsw_intf->multi_if = cpsw_dev->multi_if;

	if (cpsw_dev->multi_if)
		snprintf(node_name, sizeof(node_name), "interface-%d",
			 netcp->cpsw_port - 1);
	else
		snprintf(node_name, sizeof(node_name), "interface-%d",
			 0);

	interface = of_get_child_by_name(cpsw_dev->interfaces, node_name);
	if (!interface) {
		dev_err(cpsw_dev->dev, "interface data not available\n");
		devm_kfree(cpsw_dev->dev, cpsw_intf);
		return -ENODEV;
	}
	ret = of_property_read_u32(interface, "slave_port",
				   &cpsw_intf->slave_port);
	if (ret < 0) {
		dev_err(cpsw_dev->dev, "missing slave_port paramater\n");
		return -EINVAL;
	}

	ret = of_property_read_string(interface, "tx-channel",
				      &cpsw_intf->tx_chan_name);
	if (ret < 0) {
		dev_err(cpsw_dev->dev, "missing tx-channel "
			"parameter, err %d\n", ret);
		cpsw_intf->tx_chan_name = "nettx";
	}
	dev_info(cpsw_dev->dev, "dma_chan_name %s\n", cpsw_intf->tx_chan_name);

	ret = of_property_read_u32(interface, "tx_queue_depth",
				   &cpsw_intf->tx_queue_depth);
	if (ret < 0) {
		dev_err(cpsw_dev->dev, "missing tx_queue_depth "
			"parameter, err %d\n", ret);
		cpsw_intf->tx_queue_depth = 32;
	}
	dev_dbg(cpsw_dev->dev, "tx_queue_depth %u\n",
		cpsw_intf->tx_queue_depth);

	of_node_put(interface);

	cpsw_intf->num_slaves = cpsw_dev->slaves_per_interface;

	cpsw_intf->slaves = devm_kzalloc(cpsw_dev->dev,
					 sizeof(struct cpsw_slave) *
					 cpsw_intf->num_slaves, GFP_KERNEL);

	if (!cpsw_intf->slaves) {
		dev_err(cpsw_dev->dev, "cpsw interface slave memory "
			"allocation failed\n");
		devm_kfree(cpsw_dev->dev, cpsw_intf);
		return -ENOMEM;
	}

	if (cpsw_dev->multi_if) {
		cpsw_intf->slaves[i].slave_num = cpsw_intf->slave_port;
		cpsw_intf->slaves[i].link_interface =
			cpsw_dev->link[cpsw_intf->slave_port];
		cpsw_intf->phy_node = cpsw_dev->phy_node[cpsw_intf->slave_port];
		cpsw_slave_cpts_ctl_init(&(cpsw_intf->slaves[i]));
	} else {
		for (i = 0; i < cpsw_intf->num_slaves; i++) {
			cpsw_intf->slaves[i].slave_num = i;
			cpsw_intf->slaves[i].link_interface = cpsw_dev->link[i];
			cpsw_slave_cpts_ctl_init(&(cpsw_intf->slaves[i]));
		}
	}

	netcp_txpipe_init(&cpsw_intf->tx_pipe, netdev_priv(ndev),
			  cpsw_intf->tx_chan_name, cpsw_intf->tx_queue_depth);

	cpsw_intf->tx_pipe.dma_psflags	= netcp->cpsw_port;

	SET_ETHTOOL_OPS(ndev, &keystone_ethtool_ops);

	list_add(&cpsw_intf->cpsw_intf_list, &cpsw_dev->cpsw_intf_head);

	*intf_priv = cpsw_intf;
	return 0;
}

static int cpsw_release(void *intf_priv)
{
	struct cpsw_intf *cpsw_intf = intf_priv;

	SET_ETHTOOL_OPS(cpsw_intf->ndev, NULL);

	list_del(&cpsw_intf->cpsw_intf_list);

	devm_kfree(cpsw_intf->dev, cpsw_intf->slaves);
	devm_kfree(cpsw_intf->dev, cpsw_intf);

	return 0;
}


static struct netcp_module cpsw_module = {
	.name		= CPSW_MODULE_NAME,
	.owner		= THIS_MODULE,
	.probe		= cpsw_probe,
	.open		= cpsw_open,
	.close		= cpsw_close,
	.remove		= cpsw_remove,
	.attach		= cpsw_attach,
	.release	= cpsw_release,
	.add_addr	= cpsw_add_addr,
	.del_addr	= cpsw_del_addr,
	.ioctl		= cpsw_ioctl,
};

int __init keystone_cpsw_init(void)
{
	return netcp_register_module(&cpsw_module);
}
//module_init(keystone_cpsw_init);

void __exit keystone_cpsw_exit(void)
{
	netcp_unregister_module(&cpsw_module);
}
//module_exit(keystone_cpsw_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sandeep Paulraj <s-paulraj@ti.com>");
MODULE_DESCRIPTION("CPSW driver for Keystone devices");
