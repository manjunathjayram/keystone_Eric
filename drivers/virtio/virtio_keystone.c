/*
 * Copyright (C) 2012 Texas Instruments Incorporated
 * Author: Cyril Chemparathy <cyril@ti.com>
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
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/hwqueue.h>
#include <linux/dmapool.h>
#include <linux/hwqueue.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/of_address.h>
#include <linux/dma-mapping.h>
#include <linux/virtio.h>
#include <linux/pci_ids.h>
#include <linux/virtio_config.h>
#include <linux/platform_device.h>

#define BITS(x)			(BIT(x) - 1)

#define DMA_OPT_HAS_EPIB	BIT(31)
#define DMA_OPT_HAS_PSDATA	BIT(30)
#define DMA_OPT_HAS_FLOWTAG	BIT(29)
#define DMA_OPT_HAS_PSFLAG	BIT(28)
#define DMA_OPT_PSFLAG_SHIFT	16
#define DMA_OPT_FLOWTAG_SHIFT	0

#define DMA_LOOPBACK		BIT(31)
#define DMA_ENABLE		BIT(31)
#define DMA_TEARDOWN		BIT(30)
#define DMA_HAS_EPIB		BIT(30)
#define DMA_HAS_PSINFO		BIT(29)
#define DMA_TIMEOUT		1000	/* msecs */
#define DMA_MAX_QMS		4
#define DMA_DEFAULT_TX_PRIO	2
#define DMA_DEFAULT_RX_PRIO	0
#define DMA_DEFAULT_MAX_DESCS	64
#define DMA_EPIB_LEN		4
#define DMA_PSDATA_LEN		36
#define DMA_MAX_BUFLEN		BIT(22)
#define DMA_MAX_PKTLEN		BIT(17)

#define DESC_HAS_EPIB		BIT(31)
#define DESC_PSLEN_SHIFT	24
#define DESC_PSFLAG_MASK	BITS(4)
#define DESC_PSFLAG_SHIFT	16
#define DESC_FLOWTAG_SHIFT	16
#define DESC_FLOWTAG_MASK	BITS(16)
#define DESC_LEN_MASK		BITS(22)
#define DESC_TYPE_HOST		BIT(26)

#define HWQUEUE_HAS_PACKET_SIZE	BIT(31)

struct reg_global {
	u32	revision;
	u32	perf_control;
	u32	emulation_control;
	u32	priority_control;
	u32	queue_mgr_base[DMA_MAX_QMS];
};

struct reg_chan {
	u32	control;
	u32	mode;
	u32	__rsvd[6];
};

struct reg_tx_sched {
	u32	prio;
};

struct reg_rx_flow {
	u32	control;
	u32	tags;
	u32	tag_sel;
	u32	fdq_sel[2];
	u32	thresh[3];
};

#define REGS_BUILD_CHECK()						\
	do {								\
		BUILD_BUG_ON(sizeof(struct reg_global)   != 32);	\
		BUILD_BUG_ON(sizeof(struct reg_chan)     != 32);	\
		BUILD_BUG_ON(sizeof(struct reg_rx_flow)  != 32);	\
		BUILD_BUG_ON(sizeof(struct reg_tx_sched) !=  4);	\
	} while (0)

struct keystone_vq_desc {
	struct {
		u32			desc_info;
		u32			flowtag;
		u32			packet_info;
		u32			buf_len;
		u32			buf;
		u32			next_desc;
		u32			orig_len;
		u32			orig_buf;
		u32			epib[DMA_EPIB_LEN];
		u32			psdata[DMA_PSDATA_LEN];
	} hw;
	struct {
		u32			*epib;
		u32			*psdata;
		void			*user_data;
		u8			 epiblen;
		u8			 pslen;
		struct scatterlist	 sg;
	} sw;
	struct {
		struct list_head	 list;
		struct keystone_vq	*kvq;
		u8			 desc_len;
	} ctl;
} ____cacheline_aligned;
#define DESC_MIN_HW_SIZE	offsetof(struct keystone_vq_desc, hw.epib)
#define DESC_MAX_HW_SIZE	offsetof(struct keystone_vq_desc, sw)
#define DESC_MIN_SIZE		ALIGN(sizeof(struct keystone_vq_desc),	\
				      SMP_CACHE_BYTES)
#define DESC_BUILD_CHECK()						\
	do {								\
		BUILD_BUG_ON(DESC_MAX_HW_SIZE & (SMP_CACHE_BYTES - 1));	\
	} while (0)

struct keystone_vq_config {
	enum dma_data_direction		 direction;
	const char			*name;
	int				 q_submit;
	int				 q_complete;
	const char			*q_pool;
	u32				 flowtag;
	int				 channel, flow, priority;
	int				 min_descs, max_descs;
	bool				 debug;
};
#define is_tx_cfg(cfg)	((cfg)->direction == DMA_TO_DEVICE)
#define is_rx_cfg(cfg)	((cfg)->direction == DMA_FROM_DEVICE)

struct keystone_vq {
	struct keystone_virtio		*kv;
	struct keystone_vdev		*kvdev;
	struct list_head		 node;
	struct keystone_vq_config	 cfg;
	bool				 dynamic;	/* !(created at init) */
	bool				 in_use;
	spinlock_t			 lock;
	struct virtqueue		 vq;

	/* registers */
	struct reg_chan __iomem		*reg_chan;
	struct reg_tx_sched __iomem	*reg_tx_sched;
	struct reg_rx_flow __iomem	*reg_rx_flow;

	/* allocated resources */
	struct hwqueue			*q_submit;
	struct hwqueue			*q_complete;
	struct hwqueue			*q_pool;
	int				 qnum_submit;
	int				 qnum_complete;
	struct list_head		 free_list, used_list;
	int				 free_count, used_count;
};
#define kvq_dev(kvq)		kvdev_dev((kvq)->kvdev)
#define kvq_to_vq(kvq)		&(kvq)->vq	
#define vq_to_kvq(vq)		container_of(vq, struct keystone_vq, vq)
#define is_tx_vq(kvq)		is_tx_cfg(&((kvq)->cfg))
#define is_rx_vq(kvq)		is_rx_cfg(&((kvq)->cfg))
#define kvq_name(kvq)		((kvq)->cfg.name)

#define kvq_dbg(kvq, format, arg...)				\
	do {							\
		if ((kvq)->cfg.debug)				\
			dev_dbg(kvq_dev(kvq), "%s: " format,	\
				kvq_name(kvq), ##arg);		\
	} while (0)

#define kvq_vdbg(kvq, format, arg...)				\
	do {							\
		if ((kvq)->cfg.debug)				\
			dev_vdbg(kvq_dev(kvq), "%s: " format,	\
				 kvq_name(kvq), ##arg);		\
	} while (0)

struct keystone_vdev {
	struct keystone_virtio		*kv;
	u8				 status;
	const char			*name;
	struct virtio_device		 vdev;
	struct list_head		 node;
	struct list_head		 vqs;
	bool				 debug;
	spinlock_t			 lock;
};
#define kvdev_dev(kvdev)	&((kvdev)->vdev.dev)
#define kvdev_to_vdev(kvdev)	&((kvdev)->vdev)
#define vdev_to_kvdev(vdev)	container_of(vdev, struct keystone_vdev, vdev)

#define status_is_probed(s)	\
	((s) & (VIRTIO_CONFIG_S_FAILED | VIRTIO_CONFIG_S_DRIVER_OK))
#define status_is_probing(s)	\
	(((s) & VIRTIO_CONFIG_S_DRIVER) && !status_is_probed(s))
#define status_is_busy(s)	\
	(((s) & VIRTIO_CONFIG_S_DRIVER_OK) || status_is_probing(s))

struct keystone_virtio {
	struct device			*dev;
	struct clk			*clk;
	bool				 big_endian, loopback, enable_all;
	unsigned			 tx_priority, rx_priority;
	u32				 num_queue_mgrs;
	u32				 queue_mgr_base[DMA_MAX_QMS];
	struct reg_global __iomem	*reg_global;
	struct reg_chan __iomem		*reg_tx_chan;
	struct reg_chan __iomem		*reg_rx_chan;
	struct reg_rx_flow __iomem	*reg_rx_flow;
	struct reg_tx_sched __iomem	*reg_tx_sched;
	unsigned			 max_rx_channel;
	unsigned			 max_tx_channel;
	unsigned			 max_rx_flow;
	bool				 debug;
	atomic_t			 num_users;
	struct list_head		 devices;
};
#define kv_dev(kv)	((kv)->dev)

static inline void __kvq_put_desc(struct keystone_vq *kvq,
				  struct keystone_vq_desc *desc)
{
	list_del(&desc->ctl.list);
	kvq->used_count --;
	list_add_tail(&desc->ctl.list, &kvq->free_list);
	kvq->free_count ++;
}

static inline void kvq_put_desc(struct keystone_vq *kvq,
				struct keystone_vq_desc *desc)
{
	unsigned long flags;
	spin_lock_irqsave(&kvq->lock, flags);
	__kvq_put_desc(kvq, desc);
	spin_unlock_irqrestore(&kvq->lock, flags);
}

static inline void kvq_put_descs(struct keystone_vq *kvq,
				 struct keystone_vq_desc *descs[],
				 int num_desc)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&kvq->lock, flags);
	for (i = 0; i < num_desc; i++) {
		if (descs[i])
			__kvq_put_desc(kvq, descs[i]);
	}
	spin_unlock_irqrestore(&kvq->lock, flags);
}

static inline int kvq_get_descs(struct keystone_vq *kvq,
				struct keystone_vq_desc *descs[],
				int num_desc)
{
	unsigned long flags;
	int i, error = -ENOMEM;

	spin_lock_irqsave(&kvq->lock, flags);
	if (kvq->free_count < num_desc)
		goto unlock_ret;

	for (i = 0; i < num_desc; i++) {
		BUG_ON(list_empty(&kvq->free_list));
		descs[i] = list_first_entry(&kvq->free_list,
					    struct keystone_vq_desc,
					    ctl.list);
		list_del(&descs[i]->ctl.list);
		kvq->free_count --;
		list_add_tail(&descs[i]->ctl.list, &kvq->used_list);
		kvq->used_count ++;
	}
	error = 0;

unlock_ret:
	spin_unlock_irqrestore(&kvq->lock, flags);
	return error;
}

static inline void kvq_dump_desc(struct keystone_vq *kvq,
				 struct keystone_vq_desc *desc,
				 const char *why, int idx)
{
	unsigned long *data = (unsigned long *)desc;

	kvq_vdbg(kvq, "descriptor dump @%s, index %d, desc %p\n",
		 why, idx, desc);

	kvq_vdbg(kvq, "\tdesc_info: %x\n",	desc->hw.desc_info);
	kvq_vdbg(kvq, "\tflowtag: %x\n",	desc->hw.flowtag);
	kvq_vdbg(kvq, "\tpacket_info: %x\n",	desc->hw.packet_info);
	kvq_vdbg(kvq, "\tbuf_len: %x\n",	desc->hw.buf_len);
	kvq_vdbg(kvq, "\tbuf: %x\n",		desc->hw.buf);
	kvq_vdbg(kvq, "\tnext_desc: %x\n",	desc->hw.next_desc);
	kvq_vdbg(kvq, "\torig_len: %x\n",	desc->hw.orig_len);
	kvq_vdbg(kvq, "\torig_buf: %x\n",	desc->hw.orig_buf);

	kvq_vdbg(kvq, "\t%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		  data[0x00], data[0x01], data[0x02], data[0x03],
		  data[0x04], data[0x05], data[0x06], data[0x07]);
	kvq_vdbg(kvq, "\t%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		  data[0x08], data[0x09], data[0x0a], data[0x0b],
		  data[0x0c], data[0x0d], data[0x0e], data[0x0f]);
	kvq_vdbg(kvq, "\t%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		  data[0x10], data[0x11], data[0x12], data[0x13],
		  data[0x14], data[0x15], data[0x16], data[0x17]);
	kvq_vdbg(kvq, "\t%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		  data[0x18], data[0x19], data[0x1a], data[0x1b],
		  data[0x1c], data[0x1d], data[0x1e], data[0x1f]);
	kvq_vdbg(kvq, "\t%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		  data[0x20], data[0x21], data[0x22], data[0x23],
		  data[0x24], data[0x25], data[0x26], data[0x27]);
	kvq_vdbg(kvq, "\t%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		  data[0x28], data[0x29], data[0x2a], data[0x2b],
		  data[0x2c], data[0x2d], data[0x2e], data[0x2f]);
	kvq_vdbg(kvq, "\t%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		  data[0x30], data[0x31], data[0x32], data[0x33],
		  data[0x34], data[0x35], data[0x36], data[0x37]);
	kvq_vdbg(kvq, "\t%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		  data[0x38], data[0x39], data[0x3a], data[0x3b],
		  data[0x3c], data[0x3d], data[0x3e], data[0x3f]);
}

static int kvq_add_buf(struct virtqueue *vq, struct scatterlist _sg[],
		       unsigned int out_num, unsigned int in_num,
		       void *data, unsigned options, gfp_t gfp)
{
	struct keystone_vq *kvq = vq_to_kvq(vq);
	enum dma_data_direction dir = kvq->cfg.direction;
	unsigned num_sg = out_num + in_num;
	struct keystone_vq_desc *descs[num_sg], *desc;
	u32 psflag = 0, flowtag, packet_len, packet_info;
	u32 pslen = 0, *psdata = NULL;
	u32 epiblen = 0, *epib = NULL;
	struct scatterlist *sg = _sg;
	dma_addr_t dma = 0;
	unsigned desc_size;
	int idx, error;

	if ((is_tx_vq(kvq) && in_num) || (is_rx_vq(kvq) && out_num) || !num_sg)
		return -EINVAL;

	if (unlikely(options & DMA_OPT_HAS_EPIB)) {
		if (num_sg <= 0)
			return -EINVAL;
		epiblen = sg->length;
		epib = sg_virt(sg);
		num_sg--;
		sg++;
		if (unlikely(epiblen != DMA_EPIB_LEN * sizeof(u32))) {
			dev_err(kvq_dev(kvq), "bad epib length %d\n", epiblen);
			return -EINVAL;
		}
	}

	if (unlikely(options & DMA_OPT_HAS_PSDATA)) {
		if (num_sg <= 0)
			return -EINVAL;
		pslen = sg->length;
		psdata = sg_virt(sg);
		num_sg--;
		sg++;
		if (unlikely(pslen > DMA_PSDATA_LEN * sizeof(u32) ||
			     pslen & (sizeof(u32) - 1))) {
			dev_err(kvq_dev(kvq), "bad psdata length %d\n", pslen);
			return -EINVAL;
		}
	}

	if (unlikely(options & DMA_OPT_HAS_PSFLAG))
		psflag = ((options >> DMA_OPT_PSFLAG_SHIFT) & DESC_PSFLAG_MASK);

	flowtag = kvq->cfg.flowtag;
	if (unlikely(options & DMA_OPT_HAS_FLOWTAG))
		flowtag = ((options >> DMA_OPT_FLOWTAG_SHIFT) &
			    DESC_FLOWTAG_MASK);

	for (idx = 0, packet_len = 0; idx < num_sg; idx++) {
		if (sg_dma_len(sg + idx) >= DMA_MAX_BUFLEN) {
			dev_err(kvq_dev(kvq), "buffer overflow %d\n",
				sg_dma_len(sg));
			return -EOVERFLOW;
		}
		packet_len += sg_dma_len(sg + idx);
		if (packet_len >= DMA_MAX_PKTLEN) {
			dev_err(kvq_dev(kvq), "packet overflow %d\n",
				packet_len);
			return -EOVERFLOW;
		}
	}

	packet_info = ((epib ? DESC_HAS_EPIB : 0)      |
		       (pslen << DESC_PSLEN_SHIFT) |
		       (psflag << DESC_PSFLAG_SHIFT) |
		       kvq->qnum_complete);

	error = kvq_get_descs(kvq, descs, num_sg);
	if (error)
		return error;

	error = dma_map_sg(kv_dev(kvq->kv), sg, num_sg, dir);
	if (error != num_sg)
		goto error_map_sg;

	for (idx = num_sg - 1, packet_len = 0; idx >= 0; idx--) {
		desc      = descs[idx];
		desc_size = DESC_MIN_HW_SIZE;

		memset(&desc->hw, 0, sizeof(desc->hw));
		memset(&desc->sw, 0, sizeof(desc->sw));

		desc->sw.sg        = sg[idx];
		desc->hw.next_desc = dma;
		desc->hw.buf       = sg_dma_address(sg + idx);
		desc->hw.orig_buf  = sg_dma_address(sg + idx);
		desc->hw.buf_len   = sg_dma_len(sg + idx);
		desc->hw.orig_len  = sg_dma_len(sg + idx);
		packet_len        += sg_dma_len(sg + idx);

		if (!idx) {
			desc->sw.epib        = epib;
			desc->sw.epiblen     = epiblen;
			desc->sw.psdata      = psdata;
			desc->sw.pslen       = pslen;
			desc->sw.user_data   = data;
			desc->hw.desc_info   = packet_len;
			desc->hw.flowtag     = flowtag << DESC_FLOWTAG_SHIFT;
			desc->hw.packet_info = packet_info;

			desc_size += (epiblen + pslen) * sizeof(u32);
			memcpy(desc->hw.epib, epib, epiblen);
			memcpy(desc->hw.psdata, psdata, pslen);
		}

		kvq_dump_desc(kvq, desc, "submit", idx);
		error = hwqueue_map(kvq->q_submit, desc, desc_size, &dma,
				    &desc_size);
		if (WARN_ON(error))
			goto error_map;
	}

	error = hwqueue_push(kvq->q_submit, dma, desc_size,
			     packet_len | HWQUEUE_HAS_PACKET_SIZE);
	if (WARN_ON(error))
		goto error_push;

	return kvq->free_count;

error_push:
	idx = 0;
	hwqueue_unmap(kvq->q_submit, dma, desc_size);

error_map:
	for (++idx; idx < num_sg; idx++)
		hwqueue_unmap(kvq->q_submit, descs[idx - 1]->hw.next_desc,
			      DESC_MIN_HW_SIZE);
	dma_unmap_sg(kv_dev(kvq->kv), sg, num_sg, dir);

error_map_sg:
	kvq_put_descs(kvq, descs, num_sg);

	return error;
}

static void *kvq_complete(struct keystone_vq *kvq, unsigned int *len,
			  struct hwqueue *queue)
{
	enum dma_data_direction dir = kvq->cfg.direction;
	struct keystone_vq_desc *desc;
	unsigned desc_size;
	dma_addr_t dma;
	int idx = 0;
	void *data;

	for (;;) {
		dma = hwqueue_pop(queue, &desc_size, NULL, 0);
		if (!dma)
			return NULL;
		desc = hwqueue_unmap(queue, dma, desc_size);
		if (desc)
			break;
		dev_warn(kvq_dev(kvq), "failed to unmap desc 0x%08x\n", dma);
	}

	if (unlikely(desc->sw.epib))
		memcpy(desc->sw.epib, desc->hw.epib, desc->sw.epiblen);
	if (unlikely(desc->sw.psdata))
		memcpy(desc->sw.psdata, desc->hw.psdata, desc->sw.pslen);
	if (len)
		*len = desc->hw.desc_info & DESC_LEN_MASK;
	data = desc->sw.user_data;

	for (idx = 0; desc; idx++) {
		kvq_dump_desc(kvq, desc, "complete", idx);
		dma_unmap_sg(kv_dev(kvq->kv), &desc->sw.sg, 1, dir);
		dma = desc->hw.next_desc;
		kvq_put_desc(kvq, desc);
		desc = (dma) ? hwqueue_unmap(queue, dma, DESC_MIN_HW_SIZE) : NULL;
	}

	return data;
}

static void *kvq_get_buf(struct virtqueue *vq, unsigned int *len)
{
	struct keystone_vq *kvq = vq_to_kvq(vq);
	return kvq_complete(kvq, len, kvq->q_complete);
}

static void *kvq_detach_unused_buf(struct virtqueue *vq)
{
	struct keystone_vq *kvq = vq_to_kvq(vq);
	return kvq_complete(kvq, NULL, kvq->q_submit);
}

static void kvq_disable_cb(struct virtqueue *vq)
{
	struct keystone_vq *kvq = vq_to_kvq(vq);
	hwqueue_disable_notifier(kvq->q_complete);
}

static bool kvq_enable_cb(struct virtqueue *vq, bool delayed)
{
	struct keystone_vq *kvq = vq_to_kvq(vq);
	hwqueue_enable_notifier(kvq->q_complete);
	return (hwqueue_get_count(kvq->q_complete) == 0); /* more? */
}

static struct virtqueue_ops kvq_ops = {
	.add_buf		= kvq_add_buf,
	.get_buf		= kvq_get_buf,
	.disable_cb		= kvq_disable_cb,
	.enable_cb		= kvq_enable_cb,
	.detach_unused_buf	= kvq_detach_unused_buf,
};

static struct hwqueue *kvq_open_pool(struct keystone_vq *kvq)
{
	struct hwqueue *q;

	q = hwqueue_open(kvq->cfg.q_pool, HWQUEUE_BYNAME, O_RDWR | O_NONBLOCK);
	if (IS_ERR(q))
		dev_err(kvq_dev(kvq), "%s: error %ld opening %s pool\n",
			kvq_name(kvq), PTR_ERR(q), kvq->cfg.q_pool);
	return q;
}

static struct hwqueue *kvq_open_queue(struct keystone_vq *kvq,
				      unsigned id, unsigned flags,
				      const char *type)
{
	struct hwqueue *q;
	char name[32];

	/* always non blocking */
	flags |= O_NONBLOCK;

	scnprintf(name, sizeof(name), "%s:%s:%s",
		  dev_name(kvq_dev(kvq)), kvq_name(kvq), type);
	q = hwqueue_open(name, id, flags);
	if (IS_ERR(q))
		dev_err(kvq_dev(kvq), "error %ld opening %s queue\n",
			PTR_ERR(q), type);
	return q;
}

static void kvq_release_queues(struct keystone_vq *kvq)
{
	if (kvq->q_complete) {
		hwqueue_set_notifier(kvq->q_complete, NULL, NULL);
		hwqueue_close(kvq->q_complete);
	}
	kvq->q_complete = NULL;
	kvq->qnum_complete = 0;

	if (kvq->q_submit)
		hwqueue_close(kvq->q_submit);
	kvq->q_submit = NULL;
	kvq->qnum_submit = 0;

	if (kvq->q_pool)
		hwqueue_close(kvq->q_pool);
	kvq->q_pool = NULL;
}

static void kvq_complete_callback(void *arg)
{
	struct keystone_vq *kvq = arg;
	struct virtqueue *vq = kvq_to_vq(kvq);

	if (vq->callback)
		vq->callback(vq);
}

static int kvq_alloc_queues(struct keystone_vq *kvq)
{
	struct hwqueue *q;
	unsigned flags;
	int ret = 0;

	/* open pool queue */
	q = kvq_open_pool(kvq);
	if (IS_ERR(q)) {
		dev_err(kvq_dev(kvq), "failed to open pool queue (%ld)\n",
			PTR_ERR(q));
		goto fail;
	}
	kvq->q_pool = q;

	/* Open submit queue */
	flags = O_RDWR | O_EXCL;
	q = kvq_open_queue(kvq, kvq->cfg.q_submit, flags, "submit");
	if (IS_ERR(q)) {
		dev_err(kvq_dev(kvq), "failed to open submit queue (%ld)\n",
			PTR_ERR(q));
		goto fail;
	}
	kvq->qnum_submit = hwqueue_get_id(q);
	kvq->q_submit = q;

	/* open completion queue */
	flags = O_RDONLY | O_HIGHTHROUGHPUT | O_EXCL;
	q = kvq_open_queue(kvq, kvq->cfg.q_complete, flags, "complete");
	if (IS_ERR(q)) {
		dev_err(kvq_dev(kvq), "failed to open complete queue (%ld)\n",
			PTR_ERR(q));
		goto fail;
	}
	kvq->qnum_complete = hwqueue_get_id(q);
	kvq->q_complete = q;

	/* setup queue notifier */
	ret = hwqueue_set_notifier(q, kvq_complete_callback, kvq);
	if (ret < 0) {
		dev_err(kvq_dev(kvq), "failed to setup queue notify (%d)\n",
			ret);
		goto fail;
	}
	hwqueue_disable_notifier(q);

	kvq_dbg(kvq, "opened queues: submit %d, complete %d\n",
		kvq->qnum_submit, kvq->qnum_complete);

	return 0;

fail:
	kvq_release_queues(kvq);
	return IS_ERR(q) ? PTR_ERR(q) : ret;
}

static void kvq_release_descs(struct keystone_vq *kvq)
{
	struct keystone_vq_desc *desc;
	dma_addr_t dma;
	unsigned dma_len;
	int error;

	while (!list_empty(&kvq->free_list)) {
		desc = list_first_entry(&kvq->free_list,
					struct keystone_vq_desc, ctl.list);
		list_del(&desc->ctl.list);

		error = hwqueue_map(kvq->q_pool, desc, desc->ctl.desc_len,
				    &dma, &dma_len);
		if (WARN_ON_ONCE(error))
			continue;

		error = hwqueue_push(kvq->q_pool, dma, dma_len, 0);
		if (WARN_ON_ONCE(error))
			continue;

		kvq->free_count--;
	}

	WARN(kvq->used_count + kvq->free_count,
	     "leaked %d used and %d free descriptors\n",
	     kvq->used_count, kvq->free_count);
}

static int kvq_alloc_descs(struct keystone_vq *kvq)
{
	struct keystone_vq_desc *desc;
	unsigned desc_len;
	dma_addr_t dma;

	while (kvq->free_count < kvq->cfg.max_descs) {
		dma = hwqueue_pop(kvq->q_pool, &desc_len, NULL, 0);
		if (!dma)
			break;
		if (desc_len < DESC_MIN_SIZE) {
			hwqueue_push(kvq->q_pool, dma, desc_len, 0);
			break;
		}
		desc = hwqueue_unmap(kvq->q_pool, dma, desc_len);
		if (IS_ERR_OR_NULL(desc)) {
			hwqueue_push(kvq->q_pool, dma, desc_len, 0);
			break;
		}
		desc->ctl.kvq      = kvq;
		desc->ctl.desc_len = desc_len;
		list_add_tail(&desc->ctl.list, &kvq->free_list);
		kvq->free_count ++;
	}

	if (kvq->free_count < kvq->cfg.min_descs) {
		dev_err(kvdev_dev(kvq->kvdev), "failed to alloc descs\n");
		kvq_release_descs(kvq);
		return -ENOMEM;
	}

	return 0;
}

static int kvq_start(struct keystone_vq *kvq)
{
	u32 v;

	if (kvq->reg_chan) {
		__raw_writel(0, &kvq->reg_chan->mode);
		__raw_writel(DMA_ENABLE, &kvq->reg_chan->control);
	}

	if (kvq->reg_tx_sched)
		__raw_writel(kvq->cfg.priority, &kvq->reg_tx_sched->prio);

	if (kvq->reg_rx_flow) {
		v  = DMA_HAS_EPIB | DMA_HAS_PSINFO;
		v |= kvq->qnum_complete | DESC_TYPE_HOST;
		__raw_writel(v, &kvq->reg_rx_flow->control);
		__raw_writel(0, &kvq->reg_rx_flow->tags);
		__raw_writel(0, &kvq->reg_rx_flow->tag_sel);
		v = kvq->qnum_submit << 16;
		__raw_writel(v, &kvq->reg_rx_flow->fdq_sel[0]);
		__raw_writel(0, &kvq->reg_rx_flow->fdq_sel[1]);
		__raw_writel(0, &kvq->reg_rx_flow->thresh[0]);
		__raw_writel(0, &kvq->reg_rx_flow->thresh[1]);
		__raw_writel(0, &kvq->reg_rx_flow->thresh[2]);
	}

	kvq_dbg(kvq, "channel started\n");

	return 0;
}

static void kvq_stop(struct keystone_vq *kvq)
{
	unsigned long end;

	if (kvq->reg_rx_flow) {
		/* first detach fdqs, starve out the flow */
		__raw_writel(0, &kvq->reg_rx_flow->fdq_sel[0]);
		__raw_writel(0, &kvq->reg_rx_flow->fdq_sel[1]);
		__raw_writel(0, &kvq->reg_rx_flow->thresh[0]);
		__raw_writel(0, &kvq->reg_rx_flow->thresh[1]);
		__raw_writel(0, &kvq->reg_rx_flow->thresh[2]);
	}

	/* drain submitted buffers */
	while (kvq_detach_unused_buf(kvq_to_vq(kvq)))
		/* nothing */ ;

	/* wait for active transfers to complete */
	end = jiffies + msecs_to_jiffies(DMA_TIMEOUT);
	do {
		kvq_get_buf(kvq_to_vq(kvq), NULL);
		if (!kvq->used_count || signal_pending(current))
			break;
		schedule_timeout_interruptible(DMA_TIMEOUT / 10);
	} while (time_after(end, jiffies));

	/* then disconnect the completion side and hope for the best */
	if (kvq->reg_rx_flow) {
		__raw_writel(0, &kvq->reg_rx_flow->control);
		__raw_writel(0, &kvq->reg_rx_flow->tags);
		__raw_writel(0, &kvq->reg_rx_flow->tag_sel);
	}

	kvq_vdbg(kvq, "channel stopped\n");
}

static int kvirtio_hw_init(struct keystone_virtio *kv)
{
	unsigned v;
	int i;

	v  = kv->loopback ? DMA_LOOPBACK : 0;
	__raw_writel(v, &kv->reg_global->emulation_control);

	v = ((kv->tx_priority << 0) | (kv->rx_priority << 16));
	__raw_writel(v, &kv->reg_global->priority_control);

	if (kv->enable_all) {
		for (i = 0; i < kv->max_rx_channel; i++)
			__raw_writel(DMA_ENABLE, &kv->reg_rx_chan[i].control);

		for (i = 0; i < kv->max_tx_channel; i++) {
			__raw_writel(0, &kv->reg_tx_chan[i].mode);
			__raw_writel(DMA_ENABLE, &kv->reg_tx_chan[i].control);
		}
	}

	for (i = 0; i < kv->num_queue_mgrs; i++)
		__raw_writel(kv->queue_mgr_base[i],
			     &kv->reg_global->queue_mgr_base[i]);
	return 0;
}

static void kvirtio_hw_exit(struct keystone_virtio *kv)
{
	/* Do nothing for now */
}

static int kvq_open(struct keystone_vq *kvq,
		    void (*callback)(struct virtqueue *vq),
		    const char *name)
{
	struct virtqueue *vq = kvq_to_vq(kvq);
	struct keystone_virtio *kv = kvq->kv;
	int error;

	if (atomic_inc_return(&kv->num_users) <= 1) {
		error = kvirtio_hw_init(kv);
		if (error)
			goto fail_hw_init;
	}

	error = kvq_start(kvq);
	if (error)
		goto fail_start;

	vq->callback	= callback;
	vq->name	= name;
	kvq->in_use	= true;

fail_start:
	if (atomic_dec_return(&kv->num_users) <= 0)
		kvirtio_hw_exit(kv);
fail_hw_init:
	return error;
}

static void kvq_close(struct keystone_vq *kvq)
{
	struct keystone_virtio *kv = kvq->kv;

	if (!kvq->in_use)
		return;
	kvq_stop(kvq);
	if (atomic_dec_return(&kv->num_users) <= 0)
		kvirtio_hw_exit(kv);
	kvq->in_use = false;
}

static int kvdev_add_vq(struct keystone_vdev *kvdev,
			struct keystone_vq_config *cfg, bool dynamic)
{
	struct keystone_virtio *kv = kvdev->kv;
	struct device *dev = kvdev_dev(kvdev);
	struct keystone_vq *kvq;
	struct virtqueue *vq;
	int error;

	if (!cfg->q_pool) {
		dev_err(dev, "%s: unspecified descriptor pool\n", cfg->name);
		return -EINVAL;
	}

	if (is_tx_cfg(cfg)) {
		if (cfg->channel != -1 && cfg->channel >= kv->max_tx_channel) {
			dev_err(dev, "%s: invalid channel %d\n", cfg->name,
				cfg->channel);
			return -EINVAL;
		}
	} else if (is_rx_cfg(cfg)) {
		if (cfg->flow != -1 && cfg->flow >= kv->max_rx_flow) {
			dev_err(dev, "%s: invalid flow %d\n", cfg->name,
				cfg->flow);
			return -EINVAL;
		}
		if (cfg->channel != -1 && cfg->channel >= kv->max_rx_channel) {
			dev_err(dev, "%s: invalid channel %d\n", cfg->name,
				cfg->channel);
			return -EINVAL;
		}
	} else {
		dev_err(dev, "%s: bad direction\n", cfg->name);
		return -EINVAL;
	}

	kvq = devm_kzalloc(kv_dev(kv), sizeof(*kvq), GFP_KERNEL);
	if (!kvq)
		return -ENOMEM;

	kvq->kv		= kv;
	kvq->kvdev	= kvdev;
	kvq->cfg	= *cfg;
	kvq->dynamic	= dynamic;
	INIT_LIST_HEAD(&kvq->free_list);
	INIT_LIST_HEAD(&kvq->used_list);
	spin_lock_init(&kvq->lock);

	vq = kvq_to_vq(kvq);
	vq->vdev = kvdev_to_vdev(kvdev);
	vq->ops  = &kvq_ops;

	error = kvq_alloc_queues(kvq);
	if (error) {
		devm_kfree(kv_dev(kv), kvq);
		return error;
	}

	error = kvq_alloc_descs(kvq);
	if (error) {
		kvq_release_queues(kvq);
		devm_kfree(kv_dev(kv), kvq);
		return error;
	}

	spin_lock(&kvdev->lock);

	if (cfg->channel != -1) {
		if (is_tx_vq(kvq) && kv->reg_tx_chan)
			kvq->reg_chan = kv->reg_tx_chan + cfg->channel;
		if (is_rx_vq(kvq) && kv->reg_rx_chan)
			kvq->reg_chan = kv->reg_rx_chan + cfg->channel;
		if (is_tx_vq(kvq) && kv->reg_tx_sched)
			kvq->reg_tx_sched = kv->reg_tx_sched + cfg->channel;
	}

	if (is_rx_vq(kvq) && cfg->flow != -1 && kv->reg_rx_flow)
		kvq->reg_rx_flow = kv->reg_rx_flow + cfg->flow;

	list_add_tail(&vq->list, &kvdev->vqs);

	spin_unlock(&kvdev->lock);

	return 0;
}

static int kvdev_del_vq(struct keystone_vq *kvq)
{
	struct keystone_virtio *kv = kvq->kv;
	struct keystone_vdev *kvdev = kvq->kvdev;
	struct virtqueue *vq = kvq_to_vq(kvq);
	int error = 0;

	spin_lock(&kvdev->lock);

	if (!kvq->in_use) {
		kvq_release_descs(kvq);
		kvq_release_queues(kvq);
		list_del(&vq->list);
		devm_kfree(kv_dev(kv), kvq);
	} else
		error = -EBUSY;

	spin_unlock(&kvdev->lock);

	return error;
}

static int kvdev_add_vq_node(struct keystone_vdev *kvdev,
				       struct device_node *node)
{
	struct device *dev = kvdev_dev(kvdev);
	struct keystone_vq_config cfg;
	u32 t;

	memset(&cfg, 0, sizeof(cfg));
	cfg.name = node->name;
	of_property_read_string(node, "label", &cfg.name);

	of_property_read_string(node, "pool", &cfg.q_pool);

	cfg.q_submit = HWQUEUE_ANY;
	if (!of_property_read_u32(node, "submit-queue", &t))
		cfg.q_submit = t;

	cfg.q_complete = HWQUEUE_ANY;
	if (!of_property_read_u32(node, "complete-queue", &t))
		cfg.q_complete = t;

	cfg.channel = -1;
	if (!of_property_read_u32(node, "channel", &t))
		cfg.channel = t;

	cfg.max_descs = DMA_DEFAULT_MAX_DESCS;
	if (!of_property_read_u32(node, "max-descs", &t))
		cfg.max_descs = t;

	cfg.min_descs = 1;
	if (!of_property_read_u32(node, "min-descs", &t))
		cfg.min_descs = t;

	cfg.debug = of_property_read_bool(node, "debug") || kvdev->debug;

	if (of_find_property(node, "transmit", NULL)) {
		cfg.direction = DMA_TO_DEVICE;

		cfg.priority = DMA_DEFAULT_TX_PRIO;
		if (!of_property_read_u32(node, "priority", &t))
			cfg.priority = t;

		cfg.flowtag = -1;
		if (!of_property_read_u32(node, "flowtag", &t))
			cfg.flowtag = t;

		dev_dbg(kvdev_dev(kvdev), "%s tx channel: pool %s, channel %d, "
			"prio %d, tag %x, submit %d, complete %d\n",
			cfg.name, cfg.q_pool, cfg.channel, cfg.priority,
			cfg.flowtag, cfg.q_submit, cfg.q_complete);
	} else if (of_find_property(node, "receive", NULL)) {
		cfg.direction = DMA_FROM_DEVICE;

		cfg.flow = -1;
		if (!of_property_read_u32(node, "flow", &t))
			cfg.flow = t;

		dev_dbg(kvdev_dev(kvdev), "%s rx channel: pool %s, channel %d, "
			"flow %d, submit %d, complete %d\n",
			cfg.name, cfg.q_pool, cfg.channel, cfg.flow,
			cfg.q_submit, cfg.q_complete);
	} else
		dev_err(dev, "unspecified direction for %s\n", cfg.name);

	return kvdev_add_vq(kvdev, &cfg, false);
}

static void kvdev_open(struct keystone_vdev *kvdev)
{
	get_device(kvdev_dev(kvdev));
}

static void kvdev_close(struct keystone_vdev *kvdev)
{
	put_device(kvdev_dev(kvdev));
}

static u8 kvdev_get_status(struct virtio_device *vdev)
{
	return vdev_to_kvdev(vdev)->status;
}

static void kvdev_set_status(struct virtio_device *vdev, u8 newstatus)
{
	struct keystone_vdev *kvdev = vdev_to_kvdev(vdev);
	struct keystone_virtio *kv = kvdev->kv;
	u8 oldstatus = kvdev->status;

	spin_lock(&kvdev->lock);
	if (!status_is_busy(oldstatus) && status_is_busy(newstatus))
		kvdev_open(kvdev);
	else if (status_is_busy(oldstatus) && !status_is_busy(newstatus))
		kvdev_close(kvdev);
	spin_unlock(&kvdev->lock);

	dev_dbg(kv_dev(kv), "device %s %s%s%s\n", kvdev->name,
		status_is_probing(newstatus) ? "probing" : "",
		status_is_busy(newstatus) ? ", busy" : "",
		status_is_probed(newstatus) ? ", probed" : "");
	kvdev->status = newstatus;
}

static void kvdev_reset(struct virtio_device *vdev)
{
	kvdev_set_status(vdev, 0);
}

static int kvdev_open_vqs(struct virtio_device *vdev, unsigned nvqs,
			  struct virtqueue *vqs[], vq_callback_t *callbacks[],
			  const char *names[])
{
	struct keystone_vdev *kvdev = vdev_to_kvdev(vdev);
	struct keystone_vq *kvq;
	struct virtqueue *vq;
	int index = 0, error;

	spin_lock(&kvdev->lock);

	/* fill up requested slots, opening vqs as needed... */
	list_for_each_entry(vq, &kvdev->vqs, list) {
		error = 0;
		if (index >= nvqs)
			break;
		kvq = vq_to_kvq(vq);
		if (!kvq->in_use)
			error = kvq_open(kvq, callbacks[index], names[index]);
		if (!error)
			vqs[index] = vq;
		index++;
	}

	spin_unlock(&kvdev->lock);

	/* empty out remaining slots */
	while (index < nvqs)
		vqs[index++] = NULL;

	return 0;
}

static void kvdev_close_vqs(struct virtio_device *vdev)
{
	struct keystone_vdev *kvdev = vdev_to_kvdev(vdev);
	struct virtqueue *vq, *n;

	spin_lock(&kvdev->lock);

	list_for_each_entry_safe(vq, n, &kvdev->vqs, list)
		kvq_close(vq_to_kvq(vq));

	spin_unlock(&kvdev->lock);
}

static void kvdev_get(struct virtio_device *vdev, unsigned offset,
		      void *buf, unsigned len)
{
}

static void kvdev_set(struct virtio_device *vdev, unsigned offset,
		      const void *buf, unsigned len)
{
}

static u32 kvdev_get_features(struct virtio_device *vdev)
{
	return 0;
}

static void kvdev_finalize_features(struct virtio_device *vdev)
{
}

static const char *kvdev_bus_name(struct virtio_device *vdev)
{
	struct keystone_vdev *kvdev = vdev_to_kvdev(vdev);
	return dev_name(kv_dev(kvdev->kv));
}

static struct virtio_config_ops kvdev_config_ops = {
	.get			= kvdev_get,
	.set			= kvdev_set,
	.get_status		= kvdev_get_status,
	.set_status		= kvdev_set_status,
	.reset			= kvdev_reset,
	.find_vqs		= kvdev_open_vqs,
	.del_vqs		= kvdev_close_vqs,
	.get_features		= kvdev_get_features,
	.finalize_features	= kvdev_finalize_features,
	.bus_name		= kvdev_bus_name,
};

static void kvdev_release(struct device *dev)
{
	struct virtio_device *vdev = dev_to_virtio(dev);
	struct keystone_vdev *kvdev = vdev_to_kvdev(vdev);
	list_del(&kvdev->node);
	kfree(kvdev);
}

static int kvdev_add(struct keystone_virtio *kv,
			       struct device_node *node)
{
	struct keystone_vdev *kvdev;
	struct virtio_device *vdev;
	struct device_node *child;
	struct virtqueue *vq, *n;
	const char *name;
	bool debug;
	int error;
	u32 id;

	name = node->name;
	of_property_read_string(node, "label", &name);

	debug = of_property_read_bool(node, "debug") || kv->debug;

	error = of_property_read_u32(node, "id", &id);
	if (error) {
		dev_dbg(kv_dev(kv), "device %s, unspecified id\n", name);
		return error;
	}

	error = -ENOMEM;
	kvdev = devm_kzalloc(kv_dev(kv), sizeof(*kvdev), GFP_KERNEL);
	if (!kvdev) {
		dev_err(kv_dev(kv), "device %s, failed to alloc vdev\n", name);
		return error;
	}

	kvdev->name	= name;
	kvdev->kv	= kv;
	kvdev->debug	= debug;
	INIT_LIST_HEAD(&kvdev->vqs);
	spin_lock_init(&kvdev->lock);

	vdev = kvdev_to_vdev(kvdev);
	vdev->id.vendor		= PCI_VENDOR_ID_TI;
	vdev->id.device		= id;
	vdev->config		= &kvdev_config_ops;
	vdev->dev.parent	= kv_dev(kv);
	vdev->dev.release	= kvdev_release;
	vdev->dev.of_node	= node;

	error = register_virtio_device(vdev);
	if (error) {
		dev_err(kv_dev(kv), "device %s, failed to register vdev\n", name);
		goto fail_register;
	}

	for_each_child_of_node(node, child) {
		error = kvdev_add_vq_node(kvdev, child);
		if (error)
			goto fail_vqs;
	}

	list_add_tail(&kvdev->node, &kv->devices);

	return 0;

fail_vqs:
	list_for_each_entry_safe(vq, n, &kvdev->vqs, list)
		kvdev_del_vq(vq_to_kvq(vq));
fail_register:
	devm_kfree(kv_dev(kv), kvdev);
	return error;
}

static int kvdev_remove(struct keystone_vdev *kvdev)
{
	if (status_is_busy(kvdev->status))
		return -EBUSY;
	unregister_virtio_device(&kvdev->vdev);
	return 0;
}

static void __iomem *
kvirtio_get_regs(struct keystone_virtio *kv, int index, const char *name,
	     unsigned *_size)
{
	struct device *dev = kv_dev(kv);
	struct device_node *node = dev->of_node;
	resource_size_t size;
	struct resource res;
	void __iomem *regs;

	if (of_address_to_resource(node, index, &res)) {
		dev_err(dev, "could not find %s resource (index %d)\n",
			name, index);
		return NULL;
	}
	size = resource_size(&res);

	if (!devm_request_mem_region(dev, res.start, size, dev_name(dev))) {
		dev_err(dev, "could not reserve %s resource (index %d)\n",
			name, index);
		return NULL;
	}

	regs = devm_ioremap_nocache(dev, res.start, size);
	if (!regs) {
		dev_err(dev, "could not map %s resource (index %d)\n",
			name, index);
		return NULL;
	}

	dev_vdbg(dev, "index: %d, res:%s, size:%x, phys:%x, virt:%p\n",
		 index, name, size, res.start, regs);

	if (_size)
		*_size = size;

	return regs;
}

static int kvirtio_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node, *child;
	struct keystone_virtio *kv;
	int num_devices = 0, error;
	unsigned size;
	u32 priority;

	REGS_BUILD_CHECK();
	DESC_BUILD_CHECK();

	if (!node) {
		dev_err(&pdev->dev, "could not find device info\n");
		return -EINVAL;
	}

	kv = devm_kzalloc(&pdev->dev, sizeof(*kv), GFP_KERNEL);
	if (!kv) {
		dev_err(&pdev->dev, "could not allocate driver mem\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, kv);
	atomic_set(&kv->num_users, 0);
	kv_dev(kv) = &pdev->dev;
	INIT_LIST_HEAD(&kv->devices);

	kv->clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(kv->clk)) {
		dev_warn(&pdev->dev, "no device clock found, continuing...\n");
		kv->clk = NULL;
	}

	if (kv->clk)
		clk_prepare_enable(kv->clk);

	kv->reg_global = kvirtio_get_regs(kv, 0, "global", &size);
	if (!kv->reg_global)
		return -ENODEV;
	if (size < sizeof(struct reg_global)) {
		dev_err(kv_dev(kv), "bad size (%d) for global regs\n",
			size);
		return -ENODEV;
	}

	kv->reg_tx_chan = kvirtio_get_regs(kv, 1, "txchan", &size);
	if (!kv->reg_tx_chan)
		return -ENODEV;
	kv->max_tx_channel = size / sizeof(struct reg_chan);

	kv->reg_rx_chan = kvirtio_get_regs(kv, 2, "rxchan", &size);
	if (!kv->reg_rx_chan)
		return -ENODEV;
	kv->max_rx_channel = size / sizeof(struct reg_chan);

	kv->reg_tx_sched = kvirtio_get_regs(kv, 3, "txsched", &size);
	if (!kv->reg_tx_sched)
		return -ENODEV;
	size /= sizeof(struct reg_tx_sched);
	if (kv->max_tx_channel != size) {
		dev_warn(kv_dev(kv), "tx count mismatch: channel %d, sched %d\n",
			 kv->max_tx_channel, size);
		kv->max_tx_channel = min(kv->max_tx_channel, size);
	}

	kv->reg_rx_flow = kvirtio_get_regs(kv, 4, "rxflow", &size);
	if (!kv->reg_rx_flow)
		return -ENODEV;
	kv->max_rx_flow = size / sizeof(struct reg_rx_flow);

	kv->enable_all  = of_property_read_bool(node, "enable-all");
	kv->big_endian  = of_property_read_bool(node, "big-endian");
	kv->loopback    = of_property_read_bool(node, "loop-back");
	kv->debug       = of_property_read_bool(node, "debug");

	error = of_property_read_u32(node, "rx-priority", &priority);
	if (error < 0) {
		dev_dbg(&pdev->dev, "unspecified rx priority using 0\n");
		priority = DMA_DEFAULT_RX_PRIO;
	}
	kv->rx_priority = priority;

	error = of_property_read_u32(node, "tx-priority", &priority);
	if (error < 0) {
		dev_dbg(&pdev->dev, "unspecified tx priority using 0\n");
		priority = DMA_DEFAULT_TX_PRIO;
	}
	kv->tx_priority = priority;

	if (of_find_property(node, "queue-managers", &size)) {
		kv->num_queue_mgrs = size / sizeof(u32);
		if (kv->num_queue_mgrs > DMA_MAX_QMS) {
			dev_warn(&pdev->dev, "too many queue managers\n");
			kv->num_queue_mgrs = DMA_MAX_QMS;
		}
		error = of_property_read_u32(node, "queue-managers",
					     kv->queue_mgr_base);
		if (error < 0) {
			dev_warn(&pdev->dev, "invalid queue managers\n");
			kv->num_queue_mgrs = 0;
		}
	} else {
		kv->num_queue_mgrs = 0;
		dev_warn(&pdev->dev, "unspecified queue manager base\n");
	}

	for_each_child_of_node(node, child) {
		if (kvdev_add(kv, child) >= 0)
			num_devices++;
	}

	dev_info(kv_dev(kv), "registered: dev %d, flow %d, tx %d, rx %d%s%s\n",
		 num_devices, kv->max_rx_flow, kv->max_tx_channel,
		 kv->max_rx_channel,
		 kv->big_endian ? ", big-endian" : "",
		 kv->loopback ? ", loopback" : "");

	return 0;
}

static int kvirtio_remove(struct platform_device *pdev)
{
	struct keystone_virtio *kv = platform_get_drvdata(pdev);
	struct keystone_vdev *kvdev, *n;
	int error;

	list_for_each_entry_safe(kvdev, n, &kv->devices, node) {
		error = kvdev_remove(kvdev);
		if (error)
			return error;
	}

	if (kv->clk) {
		clk_disable_unprepare(kv->clk);
		clk_put(kv->clk);
	}
	kv->clk = NULL;
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static struct of_device_id of_match[] = {
	{ .compatible = "ti,virtio-keystone", },
	{},
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver kvirtio_driver = {
	.probe	= kvirtio_probe,
	.remove	= kvirtio_remove,
	.driver = {
		.name		= "virtio-keystone",
		.owner		= THIS_MODULE,
		.of_match_table	= of_match,
	},
};
module_platform_driver(kvirtio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cyril Chemparathy <cyril@ti.com>");
MODULE_DESCRIPTION("TI Keystone VirtIO DMA driver");
