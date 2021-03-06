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

#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/eventfd.h>
#include <linux/mm_types.h>
#include <linux/virtio_ring.h>
#include <linux/udma.h>
#include <linux/dma-contiguous.h>
#include <linux/anon_inodes.h>
#include <linux/poll.h>
#include <linux/workqueue.h>

#define UDMA_MAX_VQS	64

static DEFINE_SPINLOCK(udma_lock);

struct udma_device {
	struct device		*dev;
	struct miscdevice	 misc;
	struct list_head	 users;
	struct kref		 refcount;
	const char		*name;
	struct list_head	 channels;
};
#define from_misc(misc)	container_of(misc, struct udma_device, misc)
#define to_misc(udma)	(&(udma)->misc)
#define udma_dev(udma)	((udma)->dev)

struct udma_user {
	struct udma_device	*udma;
	struct list_head	 node;
	struct list_head	 maps;
	struct file		*file;
};
#define udma_user_dev(user)	udma_dev((user)->udma)

struct udma_map {
	struct udma_user	*user;
	struct list_head	 node;
	size_t			 size;
	struct page		*page;
	void			*cpu_addr;
	struct kref		 refcount;
};
#define udma_map_dev(map)	udma_user_dev((map)->user)

struct udma_chan {
	struct vring			 vring;
	struct vm_area_struct		*last_vma;
	unsigned			 last_avail_idx;
	enum dma_data_direction		 dir;
	struct udma_map			*map;
	struct file			*file;
	struct list_head		 node;
	int				 id;
	wait_queue_head_t		 wqh;
	struct virtqueue		*vq;
	const char			*name;
	atomic_t			 count;
};

#define udma_chan_dev(chan)	udma_map_dev((chan)->map)
#define udma_chan_name(chan)	((chan)->name)

static int udma_chan_detach(struct udma_chan *chan);

static void udma_device_release(struct kref *kref)
{
	struct udma_device *udma;

	udma = container_of(kref, struct udma_device, refcount);

	WARN_ON(!list_empty(&udma->users));

	dev_dbg(udma_dev(udma), "released udma device instance\n");
	kfree(udma);
}

static inline struct udma_device *udma_device_get(struct udma_device *udma)
{
	kref_get(&udma->refcount);
	return udma;
}

static inline void udma_device_put(struct udma_device *udma)
{
	kref_put(&udma->refcount, udma_device_release);
}

static void udma_add_user(struct udma_device *udma, struct udma_user *user)
{
	spin_lock(&udma_lock);
	list_add_tail(&user->node, &udma->users);
	spin_unlock(&udma_lock);
	udma_device_get(udma);
}

static void udma_del_user(struct udma_device *udma, struct udma_user *user)
{
	spin_lock(&udma_lock);
	list_del(&user->node);
	spin_unlock(&udma_lock);
	udma_device_put(udma);
}

static void udma_user_add_map(struct udma_user *user, struct udma_map *map)
{
	spin_lock(&udma_lock);
	list_add_tail(&map->node, &user->maps);
	spin_unlock(&udma_lock);
}

static void udma_user_del_map(struct udma_user *user, struct udma_map *map)
{
	spin_lock(&udma_lock);
	list_del(&map->node);
	spin_unlock(&udma_lock);
}

static struct udma_map *udma_user_first_map(struct udma_user *user)
{
	struct udma_map *map = NULL;

	spin_lock(&udma_lock);
	if (!list_empty(&user->maps))
		map = list_first_entry(&user->maps, struct udma_map, node);
	spin_unlock(&udma_lock);
	return map;
}

static struct udma_map *
__udma_find_map(struct udma_user *user, struct udma_chan *chan,
		unsigned long start, unsigned long end, unsigned long *offset)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->active_mm;
	struct udma_map *map = NULL;

	down_read(&mm->mmap_sem);

	vma = find_vma(mm, start);

	if (likely(vma && start >= vma->vm_start && end <= vma->vm_end &&
		   vma->vm_file == user->file)) {
		map = vma->vm_private_data;
		*offset = start - vma->vm_start;
		if (likely(chan))
			chan->last_vma = vma;
	}

	up_read(&mm->mmap_sem);

	return map;
}

static inline struct udma_map *
udma_find_map(struct udma_user *user, struct udma_chan *chan,
	      void * __user ptr, size_t size, unsigned long *offset)
{
	struct vm_area_struct *vma;
	unsigned long start = (unsigned long)ptr;
	unsigned long end = start + size;

	vma = likely(chan) ? chan->last_vma : NULL;
	if (likely(vma && start >= vma->vm_start && end <= vma->vm_end)) {
		if (offset)
			*offset = start - vma->vm_start;
		return vma->vm_private_data;
	}

	return __udma_find_map(user, chan, start, end, offset);
}

static void udma_chan_complete_one(struct udma_chan *chan,
				   struct vring_desc *desc,
				   int status)
{
	struct vring *vring = &chan->vring;
	int id = desc - vring->desc;
	__u16 used_idx;

	/* return desc to the used list */
	used_idx = vring->used->idx & (vring->num - 1);
	vring->used->ring[used_idx].id = id;
	vring->used->ring[used_idx].len = status;
	vring->used->idx++;

	atomic_dec(&chan->count);

	dev_vdbg(udma_chan_dev(chan), "(%s) used %d, status %s (%u), cnt %d\n",
		 udma_chan_name(chan), vring->used->idx,
		 (status >= 0) ? "success" : "error", status,
		 atomic_read(&chan->count));
}

static int udma_chan_submit_one(struct udma_chan *chan, struct vring_desc *desc)
{
	int out = (chan->dir == DMA_TO_DEVICE) ? 1 : 0;
	unsigned long buf_size = desc->len;
	int in = 1 - out, error;
	struct scatterlist sg;
	void __user *buf_virt;
	struct udma_map *map;
	unsigned long offset;

	buf_virt = (void __user *)(unsigned long)desc->addr;

	atomic_inc(&chan->count);

	map = udma_find_map(chan->map->user, chan, buf_virt, buf_size, &offset);
	if (unlikely(!map)) {
		dev_err(udma_chan_dev(chan), "(%s) buffer %p, outside map\n",
			udma_chan_name(chan), buf_virt);
		udma_chan_complete_one(chan, desc, -EFAULT);
		return 1; /* continue to next */
	}

	sg_set_buf(&sg, map->cpu_addr + offset, buf_size);

	error = virtqueue_add_buf(chan->vq, &sg, out, in, desc, GFP_KERNEL);

	if (unlikely(error == -ENOMEM)) {
		/* out of descriptors - retry later */
		atomic_dec(&chan->count);
	} else if (unlikely(error < 0)) {
		/* some other error - return this to user */
		dev_err(udma_chan_dev(chan), "(%s) error %d adding buff\n",
			udma_chan_name(chan), error);
		udma_chan_complete_one(chan, desc, error);
		return 1; /* continue to next */
	}

	dev_vdbg(udma_chan_dev(chan), "(%s) add_buf (%p) ret %d, cnt %d\n",
		 udma_chan_name(chan), desc, error, atomic_read(&chan->count));

	return error;
}

static void udma_chan_submit(struct udma_chan *chan)
{
	struct vring *vring = &chan->vring;
	__u16 desc_idx, idx = chan->last_avail_idx;
	int error;

	while (idx != vring->avail->idx) {
		desc_idx = vring->avail->ring[idx & (vring->num - 1)];
		dev_vdbg(udma_chan_dev(chan), "(%s) submit, idx %d, avail %d\n",
			 udma_chan_name(chan), idx, vring->avail->idx);
		error = udma_chan_submit_one(chan, vring->desc + desc_idx);
		if (error >= 0)
			idx++;
		if (error <= 0)
			break;
	}
	chan->last_avail_idx = idx;
	vring_avail_event(&chan->vring) = idx;
}

static void udma_chan_complete(struct udma_chan *chan)
{
	struct vring_desc *desc;
	unsigned len;

	for (;;) {
		desc = virtqueue_get_buf(chan->vq, &len);
		if (!desc)
			break;
		dev_vdbg(udma_chan_dev(chan), "(%s) get_buf returned %p\n",
			 udma_chan_name(chan), desc);
		udma_chan_complete_one(chan, desc, len);
	}
}

static void udma_chan_drain(struct udma_chan *chan)
{
	struct vring_desc *desc;

	for (;;) {
		desc = virtqueue_detach_unused_buf(chan->vq);
		if (!desc)
			break;
		udma_chan_complete_one(chan, desc, -ENODEV);
	}
}

static unsigned int udma_chan_fop_poll(struct file *file, poll_table *wait)
{
	struct udma_chan *chan = file->private_data;
	struct device *dev = udma_chan_dev(chan);
	struct vring *vring = &chan->vring;
	unsigned long flags;
	int pollval;

	poll_wait(file, &chan->wqh, wait);

	if (chan->dir == DMA_FROM_DEVICE)
		pollval = POLLIN | POLLRDNORM;
	else
		pollval = POLLOUT | POLLWRNORM;

	dev_dbg(dev, "(%s) udma_chan_fop_poll() called\n",
		udma_chan_name(chan));

	udma_chan_submit(chan);
	udma_chan_complete(chan);

	spin_lock_irqsave(&udma_lock, flags);

	/*
	 * The user has stuff to do if the kernel's view of used index and the
	 * user's view don't match ...
	 */
	if (vring_used_event(&chan->vring) != vring->used->idx) {
		spin_unlock_irq(&udma_lock);
		dev_dbg(dev, "(%s) udma_chan_fop_poll() done - have work\n",
			udma_chan_name(chan));
		return pollval;
	}

	/* ... otherwise enable callbacks and wait for one */
	virtqueue_enable_cb(chan->vq);

	spin_unlock_irqrestore(&udma_lock, flags);

	dev_dbg(dev, "(%s) udma_chan_fop_poll() done - blocking\n",
		udma_chan_name(chan));
	return 0;
}

static int udma_chan_fop_release(struct inode *inode, struct file *file)
{
	return udma_chan_detach(file->private_data);
}

static const struct file_operations udma_chan_fops = {
	.release	= udma_chan_fop_release,
	.poll		= udma_chan_fop_poll,
};

static int udma_chan_init_fd(struct udma_chan *chan)
{
	struct file *file;
	int fd;

	fd = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return fd;

	file = anon_inode_getfile(udma_chan_name(chan), &udma_chan_fops, chan,
				  O_RDWR | O_CLOEXEC);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		return PTR_ERR(file);
	}

	chan->file = file;

	fd_install(fd, file);

	return fd;
}

static int udma_chan_setup_dma(struct udma_chan *chan)
{
	/* nothing to do */
	return 0;
}

static void udma_chan_shutdown_dma(struct udma_chan *chan)
{
	virtqueue_disable_cb(chan->vq);
	udma_chan_drain(chan);
	udma_chan_complete(chan);
	WARN_ON(atomic_read(&chan->count));
}

static void udma_chan_callback(struct virtqueue *vq)
{
	struct udma_chan *chan = virtqueue_get_drvdata(vq);
	struct device *dev = udma_chan_dev(chan);
	int pollval;

	dev_dbg(dev, "(%s) udma_chan_callback() called\n",
		udma_chan_name(chan));

	if (chan->dir == DMA_FROM_DEVICE)
		pollval = POLLIN | POLLRDNORM;
	else
		pollval = POLLOUT | POLLWRNORM;

	virtqueue_disable_cb(vq);
	if (waitqueue_active(&chan->wqh))
		wake_up_locked_poll(&chan->wqh, pollval);
}

static int udma_chan_detach(struct udma_chan *chan)
{
	if (!chan)
		return 0;

	udma_chan_shutdown_dma(chan);

	if (chan->file)
		chan->file->private_data = NULL;
	chan->file = NULL;
	chan->map = NULL;

	return 0;
}

static int udma_chan_attach(struct udma_user *user, struct udma_chan_data *data)
{
	struct udma_device	*udma = user->udma;
	struct device		*dev = udma_user_dev(user);
	struct udma_chan	*chan = NULL, *t;
	size_t			 ring_size;
	void __user		*ring_user;
	struct udma_map		*map;
	void			*ring_virt;
	unsigned long		 offset = 0;
	int			 error;
	enum dma_data_direction	 dir;

	ring_size = vring_size(data->num_desc, data->align);
	dev_dbg(dev, "(%s) chan_attach\n",
		data->name);
	if (ring_size != data->ring_size) {
		dev_err(dev, "(%s) bad chan size %d, expect %d\n",
			data->name,
			data->ring_size, ring_size);
		return -EOVERFLOW;
	}

	if (data->direction == DMA_MEM_TO_DEV)
		dir = DMA_TO_DEVICE;
	else if (data->direction == DMA_DEV_TO_MEM)
		dir = DMA_FROM_DEVICE;
	else {
		dev_err(dev, "(%s) bad direction %d\n", data->name,
			data->direction);
		return -EINVAL;
	}

	ring_user = (void __user *)data->ring_virt;
	map = udma_find_map(user, NULL, ring_user, ring_size, &offset);
	if (!map) {
		dev_err(dev, "(%s) chan does not belong to map\n", data->name);
		return -ENODEV;
	}

	ring_virt = map->cpu_addr + offset;
	memset(ring_virt, 0, ring_size);

	list_for_each_entry(t, &udma->channels, node) {
		if (!strcmp(t->name, data->name)) {
			chan = t;
			break;
		}
	}

	if (!chan) {
		dev_err(dev, "(%s) failed to find channel\n", data->name);
		return -ENOENT;
	}

	if (chan->map) {
		dev_err(dev, "(%s) channel busy\n", data->name);
		return -EBUSY;
	}

	if (chan->dir != dir) {
		dev_err(dev, "(%s) mismatched direction\n", data->name);
		return -EBUSY;
	}

	vring_init(&chan->vring, data->num_desc, ring_virt, data->align);
	atomic_set(&chan->count, 0);
	chan->last_vma = NULL;
	chan->last_avail_idx = 0;

	chan->id = udma_chan_init_fd(chan);
	if (chan->id < 0) {
		dev_err(dev, "(%s) failed to allocate chan id\n",
			udma_chan_name(chan));
		return -ENOMEM;
	}

	error = udma_chan_setup_dma(chan);
	if (error < 0) {
		put_unused_fd(chan->id);
		return error;
	}

	chan->map = map;

	dev_dbg(dev, "(%s) attach: usr %lx, kern %lx, ofs %lx, id %d\n",
		udma_chan_name(chan), (unsigned long)data->ring_virt,
		(unsigned long)(map->cpu_addr + offset), offset, chan->id);

	return chan->id;
}

static int udma_chan_create(struct udma_device *udma, struct device_node *node,
			    struct virtqueue *vq)
{
	struct device		*dev = udma_dev(udma);
	struct udma_chan	*chan;
	const char		*name;
	enum dma_data_direction	 dir = DMA_NONE;
	
	name = node->name;
	of_property_read_string(node, "label", &name);

	if (of_property_read_bool(node, "transmit"))
		dir = DMA_TO_DEVICE;
	else if (of_property_read_bool(node, "receive"))
		dir = DMA_FROM_DEVICE;
	else {
		dev_err(dev, "(%s) no direction\n", name);
		return -EINVAL;
	}

	if (!vq) {
		dev_err(dev, "(%s) no virtqueue\n", name);
		return -ENOMEM;
	}

	chan = devm_kzalloc(dev, sizeof(*chan), GFP_KERNEL);
	if (!chan) {
		dev_err(dev, "(%s) failed to allocate chan\n", name);
		return -ENOMEM;
	}

	init_waitqueue_head(&chan->wqh);
	chan->dir  = dir;
	chan->name = name;
	chan->vq   = vq;

	virtqueue_set_drvdata(vq, chan);

	dev_dbg(dev, "(%s) added channel\n", udma_chan_name(chan));

	list_add_tail(&chan->node, &udma->channels);

	return 0;
}

static void udma_map_release(struct kref *kref)
{
	struct udma_map *map = container_of(kref, struct udma_map, refcount);
	struct udma_user *user = map->user;
	struct udma_device *udma = user->udma;
	struct udma_chan *chan, *n;

	list_for_each_entry_safe(chan, n, &udma->channels, node) {
		if (chan->map == map)
			udma_chan_detach(chan);
	}

	dev_dbg(udma_map_dev(map), "closed map kern %lx, size %lx\n",
		(unsigned long)map->cpu_addr, (unsigned long)map->size);

	dma_release_from_contiguous(udma->dev, map->page,
				   map->size >> PAGE_SHIFT);
	udma_user_del_map(user, map);
	kfree(map);
}

static inline struct udma_map *udma_map_get(struct udma_map *map)
{
	kref_get(&map->refcount);
	return map;
}

static inline void udma_map_put(struct udma_map *map)
{
	kref_put(&map->refcount, udma_map_release);
}

static void udma_vma_open(struct vm_area_struct *vma)
{
	udma_map_get(vma->vm_private_data);
}

static void udma_vma_close(struct vm_area_struct *vma)
{
	udma_map_put(vma->vm_private_data);
}

static struct vm_operations_struct udma_vm_ops = {
	.open	= udma_vma_open,
	.close	= udma_vma_close,
};

static struct udma_map *udma_map_create(struct udma_user *user,
					struct vm_area_struct *vma)
{
	struct udma_device *udma = user->udma;
	struct udma_map *map;
	unsigned long order;
	size_t count;
	int ret;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		dev_err(udma_user_dev(user), "failed to allocate map\n");
		return ERR_PTR(-ENOMEM);
	}

	map->user = user;
	map->size = vma->vm_end - vma->vm_start;

	kref_init(&map->refcount);

	count = map->size >> PAGE_SHIFT;
	order = get_order(map->size);
	map->page = dma_alloc_from_contiguous(udma->dev, count, order);
	if (!map->page) {
		dev_err(udma_map_dev(map), "failed to allocate dma memory\n");
		kfree(map);
		return ERR_PTR(-ENOMEM);
	}

	map->cpu_addr = page_address(map->page);
	memset(map->cpu_addr, 0, map->size);

	ret = remap_pfn_range(vma, vma->vm_start, page_to_pfn(map->page),
			      map->size, vma->vm_page_prot);
	if (ret) {
		dev_err(udma_map_dev(map), "failed to user map dma memory\n");
		dma_release_from_contiguous(udma->dev, map->page, count);
		kfree(map);
		return ERR_PTR(-ENOMEM);
	}

	vma->vm_private_data	= map;
	vma->vm_ops		= &udma_vm_ops;

	udma_user_add_map(user, map);

	dev_dbg(udma_map_dev(map), "opened map %lx..%lx, kern %lx\n",
		vma->vm_start, vma->vm_end - 1, (unsigned long)map->cpu_addr);

	return map;
}

static struct udma_user *
udma_user_create(struct udma_device *udma, struct file *file)
{
	struct udma_user *user;

	user = kzalloc(sizeof(*user), GFP_KERNEL);
	if (!user) {
		dev_err(udma_dev(udma), "failed to allocate user\n");
		return ERR_PTR(-ENOMEM);
	}

	user->udma = udma;
	INIT_LIST_HEAD(&user->maps);
	user->file = file;
	file->private_data = user;

	udma_add_user(udma, user);

	dev_dbg(udma_user_dev(user), "opened user\n");

	return user;
}

static int udma_dev_fop_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct udma_user *user = file->private_data;
	struct udma_map *map;

	if (vma->vm_end < vma->vm_start) {
		dev_err(udma_user_dev(user),
			"strangely inverted vm area\n");
		return -EINVAL;
	}

	if (vma->vm_pgoff) {
		dev_err(udma_user_dev(user),
			"cannot mmap from non-zero offset\n");
		return -EINVAL;
	}


	if (vma->vm_start & ~PAGE_MASK) {
		dev_err(udma_user_dev(user),
			"must mmap at page boundary\n");
		return -EINVAL;
	}

	map = udma_map_create(user, vma);

	return IS_ERR(map) ? PTR_ERR(map) : 0;
}

static long udma_dev_fop_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct udma_user *user = file->private_data;
	void __user *argp = (void __user *)arg;
	struct udma_chan_data data;
	int error;

	if (likely(cmd == UDMA_IOC_ATTACH)) {
		if (copy_from_user(&data, argp, sizeof(data)))
			return -EFAULT;

		error = udma_chan_attach(user, &data);
		if (error < 0)
			return error;
		data.handle = error;
		if (copy_to_user(argp, &data, sizeof(data)))
			return -EFAULT;
	} else
		return -EINVAL;
	return 0;
}

static int udma_dev_fop_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct udma_device *udma = from_misc(misc);
	struct udma_user *user;

	user = udma_user_create(udma, file);

	return IS_ERR(user) ? PTR_ERR(user) : 0;
}

static int udma_dev_fop_release(struct inode *inode, struct file *file)
{
	struct udma_user *user = file->private_data;
	struct udma_device *udma = user->udma;
	struct udma_map *map;

	for (;;) {
		map = udma_user_first_map(user);
		if (!map)
			break;
		udma_user_del_map(user, map);
		udma_map_put(map);
	}

	dev_dbg(udma_user_dev(user), "closed user\n");
	udma_del_user(udma, user);
	kfree(user);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations udma_dev_fops = {
	.owner          = THIS_MODULE,
	.open           = udma_dev_fop_open,
	.release        = udma_dev_fop_release,
	.mmap		= udma_dev_fop_mmap,
	.unlocked_ioctl = udma_dev_fop_ioctl,
};

static int vudma_probe(struct virtio_device *vdev)
{
	struct device_node *node = vdev->dev.of_node, *child;
	struct virtqueue *vqs[UDMA_MAX_VQS];
	vq_callback_t *cbs[] = { [0 ... UDMA_MAX_VQS] = &udma_chan_callback };
	const char *names[UDMA_MAX_VQS];
	struct device *dev = &vdev->dev;
	struct udma_device *udma;
	struct miscdevice *misc;
	int error, i;

	if (!node) {
		dev_err(dev, "no channels defined\n");
		return -ENODEV;
	}

	udma = devm_kzalloc(dev, sizeof(*udma), GFP_KERNEL);
	if (!udma) {
		dev_err(dev, "could not allocate driver mem\n");
		return -ENOMEM;
	}
	virtio_set_drvdata(vdev, udma);
	misc = to_misc(udma);

	udma->dev = dev;
	udma->name = node->name;
	of_property_read_string(node, "label", &node->name);
	INIT_LIST_HEAD(&udma->users);
	INIT_LIST_HEAD(&udma->channels);
	kref_init(&udma->refcount);

	misc->minor	= MISC_DYNAMIC_MINOR;
	misc->name	= udma->name;
	misc->fops	= &udma_dev_fops;
	misc->parent	= dev;

	error = misc_register(misc);
	if (error) {
		dev_err(dev, "could not register misc device\n");
		return error;
	}

	error = vdev->config->find_vqs(vdev, UDMA_MAX_VQS, vqs, cbs, names);
	i = 0;
	for_each_child_of_node(node, child) {
		error = udma_chan_create(udma, child, vqs[i]);
		i++;
	}

	dev_info(udma_dev(udma), "registered udma device %s\n", misc->name);

	return 0;
}

static void vudma_remove(struct virtio_device *vdev)
{
	struct udma_device *udma = virtio_get_drvdata(vdev);
	struct miscdevice *misc = to_misc(udma);

	misc_deregister(misc);
	virtio_set_drvdata(vdev, NULL);
	udma_device_put(udma);
}

static struct virtio_device_id vudma_id_table[] = {
	{ VIRTIO_ID_UDMA, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int vudma_features[] = {
};

static struct virtio_driver vudma_driver = {
	.feature_table		= vudma_features,
	.feature_table_size	= ARRAY_SIZE(vudma_features),
	.driver.name		= KBUILD_MODNAME,
	.driver.owner		= THIS_MODULE,
	.id_table		= vudma_id_table,
	.probe			= vudma_probe,
	.remove			= vudma_remove,
};

module_virtio_driver(vudma_driver);

MODULE_DEVICE_TABLE(virtio, vudma_id_table);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cyril Chemparathy <cyril@ti.com>");
MODULE_DESCRIPTION("TI Keystone User DMA driver");
