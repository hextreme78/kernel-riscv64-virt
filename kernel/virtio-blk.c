#include <kernel/virtio-blk.h>
#include <kernel/kprintf.h>
#include <kernel/alloc.h>
#include <kernel/klib.h>
#include <kernel/errno.h>
#include <kernel/plic-sifive.h>
#include <kernel/sched.h>
#include <kernel/wchan.h>

virtio_blk_t virtio_blk_list[VIRTIO_MAX];

void virtio_blk_init(void)
{
	for (size_t i = 0; i < VIRTIO_MAX; i++) {
		virtio_blk_list[i].isvalid = false;
		spinlock_init(&virtio_blk_list[i].lock);
	}
}

void virtio_blk_dev_init(size_t devnum)
{
	int err;
	virtio_blk_t *dev = &virtio_blk_list[devnum];

	/* set mmio base */
	dev->base = (virtio_blk_mmio_t *) VIRTIO_MMIO_BASE(devnum);

	/* set the driver status bit */
	dev->base->virtio_mmio.status |= VIRTIO_STATUS_DRIVER;

	/* read device features */
	dev->features = dev->base->virtio_mmio.device_features;

	/* select no features */
	dev->base->virtio_mmio.device_features_sel = 0;

	/* set the features_ok status bit */
	dev->base->virtio_mmio.status |= VIRTIO_STATUS_FEATURES_OK;

	/* re-read device status and ensure the features_ok bit is still set */
	if (!(dev->base->virtio_mmio.status & VIRTIO_STATUS_FEATURES_OK)) {
		dev->base->virtio_mmio.status |= VIRTIO_STATUS_FAILED;
		kprintf_s("virtio_blk_dev_init: features are not supported\n");
		return;
	}

	/* init requestq */
	err = virtq_init(&dev->base->virtio_mmio, &dev->requestq,
			VIRTIO_BLK_REQUESTQ);
	if (err) {
		dev->base->virtio_mmio.status |= VIRTIO_STATUS_FAILED;
		kprintf_s("virtio_blk_dev_init: queue init failed (%d)\n", err);
		return;
	}

	/* read number of blocks */
	dev->capacity = dev->base->capacity;

	/* set the driver_ok status bit */
	dev->base->virtio_mmio.status |= VIRTIO_STATUS_DRIVER_OK;

	/* now device entry is valid */
	dev->isvalid = true;
}

int virtio_blk_read(size_t devnum, u64 sector, void *data)
{
	int irqflags;
	u8 status;
	virtio_blk_t *dev = &virtio_blk_list[devnum];
	virtio_blk_req_t *req;
	u16 desc0, desc1, desc2;

	spinlock_acquire_irqsave(&dev->lock, irqflags);

	if (sector >= dev->capacity) {
		spinlock_release_irqrestore(&dev->lock, irqflags);
		return -EIO;
	}

	req = kmalloc(sizeof(*req));
	if (!req) {
		spinlock_release_irqrestore(&dev->lock, irqflags);
		return -ENOMEM;
	}

	req->type = VIRTIO_BLK_T_IN;
	req->sector = sector;

	/* header descriptor */
	desc0 = virtq_desc_alloc_nofail(&dev->requestq, &dev->lock);
	dev->requestq.desc[desc0].addr = (u64) req;
	dev->requestq.desc[desc0].len = VIRTIO_BLK_REQ_HEAD_SIZE;
	dev->requestq.desc[desc0].flags = VIRTQ_DESC_F_NEXT;

	/* data descriptor */
	desc1 = virtq_desc_alloc_nofail(&dev->requestq, &dev->lock);
	dev->requestq.desc[desc1].addr = (u64) data;
	dev->requestq.desc[desc1].len = VIRTIO_BLK_REQ_DATA_SIZE;
	dev->requestq.desc[desc1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;

	/* tail descriptor */
	desc2 = virtq_desc_alloc_nofail(&dev->requestq, &dev->lock);
	dev->requestq.desc[desc2].addr = (u64) &req->status;
	dev->requestq.desc[desc2].len = VIRTIO_BLK_REQ_TAIL_SIZE;
	dev->requestq.desc[desc2].flags = VIRTQ_DESC_F_WRITE;

	/* link all descriptors */
	dev->requestq.desc[desc0].next = desc1;
	dev->requestq.desc[desc1].next = desc2;
	dev->requestq.desc[desc2].next = 0;

	/* add request to avail ring and increment idx */
	dev->requestq.avail->ring[
		dev->requestq.avail->idx % dev->requestq.virtqsz] = desc0;
	dev->requestq.avail->idx++;

	/* notify device */
	dev->base->virtio_mmio.queue_notify = VIRTIO_BLK_REQUESTQ;

	/* wait for block op */
	wchan_sleep(req, &dev->lock);

	/* save request status */
	status = req->status;

	virtq_desc_free_nofail(&dev->requestq, desc0);
	virtq_desc_free_nofail(&dev->requestq, desc1);
	virtq_desc_free_nofail(&dev->requestq, desc2);

	kfree(req);

	spinlock_release_irqrestore(&dev->lock, irqflags);

	if (status != VIRTIO_BLK_S_OK) {
		return -EIO;
	}

	return 0;
}

int virtio_blk_write(size_t devnum, u64 sector, void *data)
{
	int irqflags;
	u8 status;
	virtio_blk_t *dev = &virtio_blk_list[devnum];
	virtio_blk_req_t *req;
	u16 desc0, desc1, desc2;

	spinlock_acquire_irqsave(&dev->lock, irqflags);

	if (sector >= dev->capacity) {
		spinlock_release_irqrestore(&dev->lock, irqflags);
		return -EIO;
	}

	req = kmalloc(sizeof(*req));
	if (!req) {
		spinlock_release_irqrestore(&dev->lock, irqflags);
		return -ENOMEM;
	}

	req->type = VIRTIO_BLK_T_OUT;
	req->sector = sector;

	/* header descriptor */
	desc0 = virtq_desc_alloc_nofail(&dev->requestq, &dev->lock);
	dev->requestq.desc[desc0].addr = (u64) req;
	dev->requestq.desc[desc0].len = VIRTIO_BLK_REQ_HEAD_SIZE;
	dev->requestq.desc[desc0].flags = VIRTQ_DESC_F_NEXT;

	/* data descriptor */
	desc1 = virtq_desc_alloc_nofail(&dev->requestq, &dev->lock);
	dev->requestq.desc[desc1].addr = (u64) data;
	dev->requestq.desc[desc1].len = VIRTIO_BLK_REQ_DATA_SIZE;
	dev->requestq.desc[desc1].flags = VIRTQ_DESC_F_NEXT;

	/* tail descriptor */
	desc2 = virtq_desc_alloc_nofail(&dev->requestq, &dev->lock);
	dev->requestq.desc[desc2].addr = (u64) &req->status;
	dev->requestq.desc[desc2].len = VIRTIO_BLK_REQ_TAIL_SIZE;
	dev->requestq.desc[desc2].flags = VIRTQ_DESC_F_WRITE;

	/* link all descriptors */
	dev->requestq.desc[desc0].next = desc1;
	dev->requestq.desc[desc1].next = desc2;
	dev->requestq.desc[desc2].next = 0;

	/* add request to avail ring and increment idx */
	dev->requestq.avail->ring[
		dev->requestq.avail->idx % dev->requestq.virtqsz] = desc0;
	dev->requestq.avail->idx++;

	/* notify device */
	dev->base->virtio_mmio.queue_notify = VIRTIO_BLK_REQUESTQ;

	/* wait for block op */
	wchan_sleep(req, &dev->lock);

	/* save request status */
	status = req->status;

	/* free all descriptors */
	virtq_desc_free_nofail(&dev->requestq, desc0);
	virtq_desc_free_nofail(&dev->requestq, desc1);
	virtq_desc_free_nofail(&dev->requestq, desc2);

	/* free request memory */
	kfree(req);

	spinlock_release_irqrestore(&dev->lock, irqflags);

	if (status != VIRTIO_BLK_S_OK) {
		return -EIO;
	}

	return 0;
}

void virtio_blk_irq_handler(size_t devnum)
{
	int irqflags;
	u16 i;
	virtio_blk_t *dev = &virtio_blk_list[devnum];
	virtq_used_elem_t *used;
	virtq_desc_t *desc;
	virtio_blk_req_t *req;

	spinlock_acquire_irqsave(&dev->lock, irqflags);

	/* if block op is completed we will wakeup blocked procs */
	for (i = dev->requestq.lastusedidx % dev->requestq.virtqsz;
			i != dev->requestq.used->idx % dev->requestq.virtqsz;
			i = (i + 1) % dev->requestq.virtqsz) {
		used = &dev->requestq.used->ring[i];
		desc = &dev->requestq.desc[used->id];
		req = (void *) desc->addr;
		wchan_signal(req);
	}
	dev->requestq.lastusedidx = dev->requestq.used->idx;

	/* for virtio_blk_read_nosleep and virtio_blk_write_nosleep */
	dev->waitop = false;

	spinlock_release_irqrestore(&dev->lock, irqflags);
}

/* For fs layer init only.
 * Concurrency is not allowed.
 */
int virtio_blk_read_nosleep(size_t devnum, u64 sector, void *data)
{
	int irqflags;
	u8 status;
	virtio_blk_t *dev = &virtio_blk_list[devnum];
	virtio_blk_req_t *req;
	u16 desc0, desc1, desc2;

	spinlock_acquire_irqsave(&dev->lock, irqflags);

	if (sector >= dev->capacity) {
		spinlock_release_irqrestore(&dev->lock, irqflags);
		return -EIO;
	}

	req = kmalloc(sizeof(*req));
	if (!req) {
		spinlock_release_irqrestore(&dev->lock, irqflags);
		return -ENOMEM;
	}

	req->type = VIRTIO_BLK_T_IN;
	req->sector = sector;

	/* header descriptor */
	desc0 = virtq_desc_alloc(&dev->requestq);
	dev->requestq.desc[desc0].addr = (u64) req;
	dev->requestq.desc[desc0].len = VIRTIO_BLK_REQ_HEAD_SIZE;
	dev->requestq.desc[desc0].flags = VIRTQ_DESC_F_NEXT;

	/* data descriptor */
	desc1 = virtq_desc_alloc(&dev->requestq);
	dev->requestq.desc[desc1].addr = (u64) data;
	dev->requestq.desc[desc1].len = VIRTIO_BLK_REQ_DATA_SIZE;
	dev->requestq.desc[desc1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;

	/* tail descriptor */
	desc2 = virtq_desc_alloc(&dev->requestq);
	dev->requestq.desc[desc2].addr = (u64) &req->status;
	dev->requestq.desc[desc2].len = VIRTIO_BLK_REQ_TAIL_SIZE;
	dev->requestq.desc[desc2].flags = VIRTQ_DESC_F_WRITE;

	/* link all descriptors */
	dev->requestq.desc[desc0].next = desc1;
	dev->requestq.desc[desc1].next = desc2;
	dev->requestq.desc[desc2].next = 0;

	/* add request to avail ring and increment idx */
	dev->requestq.avail->ring[
		dev->requestq.avail->idx % dev->requestq.virtqsz] = desc0;
	dev->requestq.avail->idx++;

	/* notify device */
	dev->base->virtio_mmio.queue_notify = VIRTIO_BLK_REQUESTQ;

	/* set waitop flag */
	dev->waitop = true;

	/* wait for block op */
	while (dev->waitop) {
		spinlock_release_irq(&dev->lock);

		wfi();

		spinlock_acquire_irq(&dev->lock);
	}

	/* save request status */
	status = req->status;

	virtq_desc_free(&dev->requestq, desc0);
	virtq_desc_free(&dev->requestq, desc1);
	virtq_desc_free(&dev->requestq, desc2);

	kfree(req);

	spinlock_release_irqrestore(&dev->lock, irqflags);

	if (status != VIRTIO_BLK_S_OK) {
		return -EIO;
	}

	return 0;
}

/* For fs layer init only.
 * Concurrency is not allowed.
 */
int virtio_blk_write_nosleep(size_t devnum, u64 sector, void *data)
{
	int irqflags;
	u8 status;
	virtio_blk_t *dev = &virtio_blk_list[devnum];
	virtio_blk_req_t *req;
	u16 desc0, desc1, desc2;

	spinlock_acquire_irqsave(&dev->lock, irqflags);

	if (sector >= dev->capacity) {
		spinlock_release_irqrestore(&dev->lock, irqflags);
		return -EIO;
	}

	req = kmalloc(sizeof(*req));
	if (!req) {
		spinlock_release_irqrestore(&dev->lock, irqflags);
		return -ENOMEM;
	}

	req->type = VIRTIO_BLK_T_OUT;
	req->sector = sector;

	/* header descriptor */
	desc0 = virtq_desc_alloc(&dev->requestq);
	dev->requestq.desc[desc0].addr = (u64) req;
	dev->requestq.desc[desc0].len = VIRTIO_BLK_REQ_HEAD_SIZE;
	dev->requestq.desc[desc0].flags = VIRTQ_DESC_F_NEXT;

	/* data descriptor */
	desc1 = virtq_desc_alloc(&dev->requestq);
	dev->requestq.desc[desc1].addr = (u64) data;
	dev->requestq.desc[desc1].len = VIRTIO_BLK_REQ_DATA_SIZE;
	dev->requestq.desc[desc1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;

	/* tail descriptor */
	desc2 = virtq_desc_alloc(&dev->requestq);
	dev->requestq.desc[desc2].addr = (u64) &req->status;
	dev->requestq.desc[desc2].len = VIRTIO_BLK_REQ_TAIL_SIZE;
	dev->requestq.desc[desc2].flags = VIRTQ_DESC_F_WRITE;

	/* link all descriptors */
	dev->requestq.desc[desc0].next = desc1;
	dev->requestq.desc[desc1].next = desc2;
	dev->requestq.desc[desc2].next = 0;

	/* add request to avail ring and increment idx */
	dev->requestq.avail->ring[
		dev->requestq.avail->idx % dev->requestq.virtqsz] = desc0;
	dev->requestq.avail->idx++;

	/* notify device */
	dev->base->virtio_mmio.queue_notify = VIRTIO_BLK_REQUESTQ;

	/* set waitop flag */
	dev->waitop = true;

	/* wait for block op */
	while (dev->waitop) {
		spinlock_release_irq(&dev->lock);

		wfi();

		spinlock_acquire_irq(&dev->lock);
	}

	/* save request status */
	status = req->status;

	virtq_desc_free(&dev->requestq, desc0);
	virtq_desc_free(&dev->requestq, desc1);
	virtq_desc_free(&dev->requestq, desc2);

	kfree(req);

	spinlock_release_irqrestore(&dev->lock, irqflags);

	if (status != VIRTIO_BLK_S_OK) {
		return -EIO;
	}

	return 0;
}

