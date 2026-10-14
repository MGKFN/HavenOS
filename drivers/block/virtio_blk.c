/* SPDX-License-Identifier: MIT
 *
 * virtio_blk.c — VirtIO block device, legacy (0.9.5) interface.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS carried a VirtIO block driver because UTM SE presents one and
 * its ATA path finds nothing there.  HavenOS keeps it for the same
 * reason, and because a VirtIO disk avoids the per-sector port I/O that
 * makes emulated IDE slow.
 *
 * WHY THE LEGACY INTERFACE
 * The modern (1.0) interface puts its registers in a set of PCI
 * capabilities that have to be walked, each in a different BAR.  The
 * legacy interface is a flat set of I/O ports at BAR0, which every
 * hypervisor still offers and which needs no capability parsing.  For a
 * driver whose purpose is to make a disk work, the simpler one is the
 * right trade; the block layer means a modern driver can replace this
 * without touching any filesystem.
 *
 * THE VIRTQUEUE, IN ONE PARAGRAPH
 * Three arrays share one allocation.  The descriptor table holds
 * {address, length, flags, next} entries describing buffers.  The
 * available ring is where the driver publishes descriptor indices it
 * wants the device to process.  The used ring is where the device
 * publishes indices it has finished.  A request is three chained
 * descriptors: the header the driver writes, the data buffer, and a
 * one-byte status the device writes.  Chaining them matters -- the device
 * must see them as one request, not three.
 */
#include <haven/block.h>
#include <haven/device.h>
#include <haven/pci.h>
#include <haven/mm.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/printf.h>
#include <haven/time.h>
#include <haven/virtio.h>
#include <haven/dma.h>
#include <haven/arch/x86_64/io.h>

/* Legacy port offsets from BAR0. */
#define VIRTIO_PCI_HOST_FEATURES   0x00
#define VIRTIO_PCI_GUEST_FEATURES  0x04
#define VIRTIO_PCI_QUEUE_PFN       0x08
#define VIRTIO_PCI_QUEUE_SIZE      0x0C
#define VIRTIO_PCI_QUEUE_SEL       0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_STATUS          0x12
#define VIRTIO_PCI_ISR             0x13

/* Block-device configuration, at offset 0x14 in the legacy layout. */
#define VIRTIO_BLK_CFG_CAPACITY    0x14   /* 64-bit, in 512-byte sectors */
#define VIRTIO_BLK_CFG_SIZE_MAX    0x1C
#define VIRTIO_BLK_CFG_SEG_MAX     0x20
#define VIRTIO_BLK_CFG_BLK_SIZE    0x28

#define VIRTIO_STATUS_ACK      0x01
#define VIRTIO_STATUS_DRIVER   0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FAILED   0x80

#define VIRTIO_BLK_F_RO        (1u << 5)
#define VIRTIO_BLK_F_BLK_SIZE  (1u << 6)

#define VIRTIO_BLK_T_IN        0
#define VIRTIO_BLK_T_OUT       1
#define VIRTIO_BLK_T_FLUSH     4

#define VIRTQ_DESC_F_NEXT      1
#define VIRTQ_DESC_F_WRITE     2   /* device writes into this buffer */

/* An upper bound on what this driver will accept, not a size it chooses.
   In the LEGACY virtio-pci interface the queue-size register is READ
   ONLY: the device states its size and the driver must lay out a ring of
   exactly that many entries.  There is no way to ask for fewer.

   The first version of this driver clamped the device's reported size to
   64, so the ring it built had its available and used rings at offsets
   the device did not agree with -- the device looked for completions
   where nothing was written and every request timed out after five
   seconds.  The symptom gave no hint of the cause, which is why the
   constant is now named for what it is. */
#define QUEUE_SIZE_MAX 1024

struct HAVEN_PACKED virtq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct HAVEN_PACKED virtq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
};

struct HAVEN_PACKED virtq_used_elem {
	uint32_t id;
	uint32_t len;
};

struct HAVEN_PACKED virtq_used {
	uint16_t flags;
	uint16_t idx;
	struct virtq_used_elem ring[];
};

struct HAVEN_PACKED virtio_blk_req {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};

struct virtio_blk {
	struct block_device bdev;
	uint16_t io_base;
	uint16_t queue_size;

	/* The queue's three arrays, in one physically contiguous
	   allocation because the device is told a single page-frame
	   number and derives the rest by offset. */
	paddr_t  queue_phys;
	vaddr_t  queue_virt;
	size_t   queue_pages;

	struct virtq_desc  *desc;
	struct virtq_avail *avail;
	struct virtq_used  *used;

	uint16_t last_used_idx;
	uint16_t next_desc;

	/* Request header and status byte, in DMA-visible memory.  They
	   cannot be stack-resident: the device reads and writes them
	   directly, and a stack address is only valid while that frame
	   lives. */
	struct dma_region      aux;
	struct virtio_blk_req *hdr;
	uint8_t               *status;
	paddr_t                hdr_phys;
	paddr_t                status_phys;

	/* A staging buffer for transfers whose caller-supplied buffer is
	   not DMA-safe.
	 *
	 * This is not an optimisation; it is a correctness requirement.
	 * A device is given a PHYSICAL address and writes to it with no
	 * page tables involved, so a kmalloc'd or vmm_alloc'd buffer --
	 * not physically contiguous, and with no fixed relationship
	 * between its virtual and physical addresses -- cannot be handed
	 * to one at all.  The first version of this driver tried, and
	 * arch_virt_to_phys() panicked on the first kmalloc'd sector
	 * buffer the partition parser passed in.  See the note at the top
	 * of include/haven/dma.h. */
	struct dma_region      bounce;
};

/* 64 KiB, so a 128-sector transfer needs one round trip.  Larger than
   any single filesystem request HavenOS issues today, and small enough
   that one per device is not worth economising on. */
#define VBLK_BOUNCE_BYTES (64 * 1024)

static struct virtio_blk *devices[4];
static unsigned           device_count_vblk;

/*
 * Queue layout, as the legacy specification defines it.  The used ring
 * must start on a 4096-byte boundary after the available ring, which is
 * why this is a function rather than three struct members: getting the
 * padding wrong means the device writes completions into the middle of
 * the available ring.
 */
static size_t queue_layout(uint16_t qsize, size_t *used_offset)
{
	size_t desc_bytes  = sizeof(struct virtq_desc) * qsize;
	size_t avail_bytes = sizeof(uint16_t) * (3 + qsize);
	size_t aligned     = ALIGN_UP(desc_bytes + avail_bytes, 4096);
	size_t used_bytes  = sizeof(uint16_t) * 3 +
	                     sizeof(struct virtq_used_elem) * qsize;

	if (used_offset)
		*used_offset = aligned;

	return ALIGN_UP(aligned + used_bytes, 4096);
}

/* Issue one request whose data buffer is already DMA-safe. */
static int virtio_submit_dma(struct virtio_blk *v, uint32_t type,
                             uint64_t sector, paddr_t buf_phys, uint32_t len,
                             bool device_writes)
{
	/* Three descriptors: header, data, status.  Allocated round-robin
	   from the table; with one request outstanding at a time this
	   cannot collide, and serialising on the block device's mutex is
	   what guarantees that. */
	uint16_t d0 = v->next_desc;
	uint16_t d1 = (uint16_t)((d0 + 1) % v->queue_size);
	uint16_t d2 = (uint16_t)((d0 + 2) % v->queue_size);
	v->next_desc = (uint16_t)((d0 + 3) % v->queue_size);

	v->hdr->type     = type;
	v->hdr->reserved = 0;
	v->hdr->sector   = sector;
	*v->status       = 0xFF;   /* a value the device never writes, so a
	                              device that does not respond is
	                              distinguishable from one reporting
	                              success */

	v->desc[d0].addr  = v->hdr_phys;
	v->desc[d0].len   = sizeof(*v->hdr);
	v->desc[d0].flags = VIRTQ_DESC_F_NEXT;
	v->desc[d0].next  = d1;

	v->desc[d1].addr  = buf_phys;
	v->desc[d1].len   = len;
	v->desc[d1].flags = (uint16_t)(VIRTQ_DESC_F_NEXT |
	                               (device_writes ? VIRTQ_DESC_F_WRITE : 0));
	v->desc[d1].next  = d2;

	v->desc[d2].addr  = v->status_phys;
	v->desc[d2].len   = 1;
	v->desc[d2].flags = VIRTQ_DESC_F_WRITE;
	v->desc[d2].next  = 0;

	/* Publish the head of the chain, then advance the index.  The order
	   matters: the device may read idx at any moment, and incrementing
	   it before the ring entry is written would have it process a stale
	   descriptor index. */
	v->avail->ring[v->avail->idx % v->queue_size] = d0;
	arch_wmb();
	v->avail->idx++;
	arch_wmb();

	outw(v->io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

	/* Poll for completion.  Bounded, so a device that stops responding
	   fails the request instead of hanging the kernel -- the same
	   reasoning as the ATA driver's bounded waits. */
	for (uint64_t spin = 0; spin < 5000000; spin++) {
		arch_rmb();
		if (v->used->idx != v->last_used_idx) {
			v->last_used_idx = v->used->idx;

			uint8_t st = *v->status;
			if (st == 0)
				return 0;
			if (st == 0xFF) {
				kerr("virtio-blk: %s completed a request "
				     "without writing a status byte",
				     v->bdev.name);
				return -HAVEN_EIO;
			}
			kerr("virtio-blk: %s request failed with status %u",
			     v->bdev.name, st);
			return st == 2 ? -HAVEN_EINVAL : -HAVEN_EIO;
		}
		time_delay_us(1);
	}

	kerr("virtio-blk: %s did not complete a request within 5 seconds",
	     v->bdev.name);
	return -HAVEN_ETIMEDOUT;
}

/*
 * Issue a request for a buffer that may or may not be DMA-safe.
 *
 * When it is -- a PMM-allocated buffer reached through the direct map --
 * the device is pointed at it directly.  When it is not, the data goes
 * through the bounce buffer: copied in before a write, copied out after
 * a read.  Large transfers are split to fit.
 */
static int virtio_submit(struct virtio_blk *v, uint32_t type, uint64_t sector,
                         void *buf, uint32_t len, bool device_writes)
{
	if (len == 0 || !buf)
		return virtio_submit_dma(v, type, sector, 0, 0, false);

	if (dma_addr_is_safe((vaddr_t)buf, len, PMM_ZONE_DMA32_LIMIT)) {
		paddr_t phys = arch_virt_to_phys((vaddr_t)buf);
		return virtio_submit_dma(v, type, sector, phys, len,
		                         device_writes);
	}

	uint32_t sector_size = v->bdev.sector_size;
	uint32_t done = 0;

	while (done < len) {
		uint32_t chunk = len - done;
		if (chunk > VBLK_BOUNCE_BYTES)
			chunk = VBLK_BOUNCE_BYTES;

		/* Each chunk must be a whole number of sectors: the device
		   addresses the medium in sectors, so a partial one has no
		   LBA to name. */
		chunk -= chunk % sector_size;
		if (chunk == 0)
			return -HAVEN_EINVAL;

		if (!device_writes) {
			memcpy((void *)v->bounce.virt,
			       (const uint8_t *)buf + done, chunk);
		}

		int rc = virtio_submit_dma(v, type,
		                           sector + done / sector_size,
		                           v->bounce.phys, chunk,
		                           device_writes);
		if (rc != 0)
			return rc;

		if (device_writes) {
			memcpy((uint8_t *)buf + done,
			       (const void *)v->bounce.virt, chunk);
		}

		done += chunk;
	}

	return 0;
}

static ssize_t_k vblk_read(struct block_device *bdev, uint64_t lba,
                           void *buf, uint32_t count)
{
	struct virtio_blk *v = bdev->driver_data;
	uint32_t bytes = count * bdev->sector_size;

	int rc = virtio_submit(v, VIRTIO_BLK_T_IN, lba, buf, bytes, true);
	return rc == 0 ? (ssize_t_k)count : rc;
}

static ssize_t_k vblk_write(struct block_device *bdev, uint64_t lba,
                            const void *buf, uint32_t count)
{
	struct virtio_blk *v = bdev->driver_data;
	uint32_t bytes = count * bdev->sector_size;

	int rc = virtio_submit(v, VIRTIO_BLK_T_OUT, lba, (void *)buf, bytes,
	                       false);
	return rc == 0 ? (ssize_t_k)count : rc;
}

static int vblk_flush(struct block_device *bdev)
{
	struct virtio_blk *v = bdev->driver_data;
	return virtio_submit(v, VIRTIO_BLK_T_FLUSH, 0, NULL, 0, false);
}

static const struct block_ops vblk_ops = {
	.read  = vblk_read,
	.write = vblk_write,
	.flush = vblk_flush,
};

static bool vblk_match(const struct device *dev)
{
	/* Both the legacy (0x1001) and modern (0x1042) block device IDs:
	   a hypervisor configured for either presents a usable legacy
	   register interface. */
	return dev->id.pci.vendor == PCI_VENDOR_REDHAT &&
	       (dev->id.pci.device == 0x1001 || dev->id.pci.device == 0x1042);
}

static int vblk_probe(struct device *dev)
{
	if (device_count_vblk >= ARRAY_SIZE(devices))
		return -HAVEN_ENOSPC;
	if (!dev->io_base) {
		kerr("virtio-blk: %s has no I/O BAR; the modern "
		     "MMIO-only layout is not supported by this driver",
		     dev->name);
		return -HAVEN_ENODEV;
	}

	pci_enable_device(dev, true);

	struct virtio_blk *v = kzalloc(sizeof(*v));
	if (!v)
		return -HAVEN_ENOMEM;

	v->io_base = dev->io_base;

	/* Reset, then acknowledge.  A device left running by firmware with
	   a stale queue would process our first notify against its old
	   descriptor table. */
	outb(v->io_base + VIRTIO_PCI_STATUS, 0);
	outb(v->io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
	outb(v->io_base + VIRTIO_PCI_STATUS,
	     VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

	uint32_t host_features = inl(v->io_base + VIRTIO_PCI_HOST_FEATURES);

	/* Negotiate nothing beyond what is needed.  Accepting a feature
	   without implementing it is how a driver ends up with a device
	   using a layout it does not understand. */
	uint32_t want = 0;
	if (host_features & VIRTIO_BLK_F_BLK_SIZE)
		want |= VIRTIO_BLK_F_BLK_SIZE;
	if (host_features & VIRTIO_BLK_F_RO)
		want |= VIRTIO_BLK_F_RO;
	outl(v->io_base + VIRTIO_PCI_GUEST_FEATURES, want);

	/* Queue 0 is the request queue. */
	outw(v->io_base + VIRTIO_PCI_QUEUE_SEL, 0);
	uint16_t qsize = inw(v->io_base + VIRTIO_PCI_QUEUE_SIZE);
	if (qsize == 0) {
		kerr("virtio-blk: %s reports a zero-length queue", dev->name);
		outb(v->io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
		kfree(v);
		return -HAVEN_ENODEV;
	}
	if (qsize > QUEUE_SIZE_MAX) {
		/* Refuse rather than clamp: a smaller ring than the device
		   expects is not a smaller ring, it is a differently laid
		   out one. */
		kerr("virtio-blk: %s reports a queue of %u entries, over the "
		     "%u this driver supports", dev->name, qsize,
		     QUEUE_SIZE_MAX);
		outb(v->io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
		kfree(v);
		return -HAVEN_ENODEV;
	}
	if (qsize & (qsize - 1)) {
		/* The ring index wraps by masking, which only works for a
		   power of two. */
		kerr("virtio-blk: %s reports a queue of %u entries, which is "
		     "not a power of two", dev->name, qsize);
		outb(v->io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
		kfree(v);
		return -HAVEN_ENODEV;
	}
	v->queue_size = qsize;

	size_t used_offset = 0;
	size_t qbytes = queue_layout(qsize, &used_offset);
	size_t qpages = qbytes / HAVEN_PAGE_SIZE;

	/* The queue must be physically contiguous and below 4 GiB: the
	   device is told a 32-bit page-frame number, so a queue above
	   4 GiB cannot be described to it at all.  This is exactly the
	   constraint the PMM's DMA32 zone exists for. */
	paddr_t qphys = pmm_alloc_contig(qpages, PMM_ZONE_DMA32);
	if (!qphys) {
		kerr("virtio-blk: cannot allocate a %zu KiB contiguous queue "
		     "below 4 GiB", qbytes / 1024);
		outb(v->io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
		kfree(v);
		return -HAVEN_ENOMEM;
	}

	v->queue_phys  = qphys;
	v->queue_virt  = arch_phys_to_virt(qphys);
	v->queue_pages = qpages;
	memset((void *)v->queue_virt, 0, qbytes);

	v->desc  = (struct virtq_desc *)v->queue_virt;
	v->avail = (struct virtq_avail *)(v->queue_virt +
	                sizeof(struct virtq_desc) * qsize);
	v->used  = (struct virtq_used *)(v->queue_virt + used_offset);

	/* The header and status byte also have to be DMA-visible and below
	   4 GiB, for the same reason. */
	int rc = dma_alloc_coherent(&v->aux, HAVEN_PAGE_SIZE,
	                            PMM_ZONE_DMA32_LIMIT);
	if (rc != 0) {
		pmm_free_contig(qphys, qpages);
		kfree(v);
		return rc;
	}
	v->hdr         = (struct virtio_blk_req *)v->aux.virt;
	v->hdr_phys    = v->aux.phys;
	v->status      = (uint8_t *)(v->aux.virt + 64);
	v->status_phys = v->aux.phys + 64;

	/* The staging buffer for callers whose memory a device cannot
	   reach.  Allocated once per device rather than per request: a
	   per-request allocation would be a contiguous DMA32 allocation on
	   the path of every disk read. */
	rc = dma_alloc_coherent(&v->bounce, VBLK_BOUNCE_BYTES,
	                        PMM_ZONE_DMA32_LIMIT);
	if (rc != 0) {
		dma_free_coherent(&v->aux);
		pmm_free_contig(qphys, qpages);
		kfree(v);
		return rc;
	}

	/* Tell the device where the queue is, by page-frame number. */
	outl(v->io_base + VIRTIO_PCI_QUEUE_PFN,
	     (uint32_t)(qphys >> HAVEN_PAGE_SHIFT));

	/* Capacity is in 512-byte units regardless of the logical block
	   size, which is a detail that silently halves or doubles the
	   reported disk if assumed otherwise. */
	uint64_t capacity = (uint64_t)inl(v->io_base + VIRTIO_BLK_CFG_CAPACITY)
	                  | ((uint64_t)inl(v->io_base + VIRTIO_BLK_CFG_CAPACITY + 4) << 32);

	uint32_t blk_size = 512;
	if (want & VIRTIO_BLK_F_BLK_SIZE) {
		uint32_t reported = inl(v->io_base + VIRTIO_BLK_CFG_BLK_SIZE);
		if (reported >= 512 && reported <= 4096 &&
		    (reported & (reported - 1)) == 0)
			blk_size = reported;
	}

	snprintf(v->bdev.name, sizeof(v->bdev.name), "vd%c",
	         (char)('a' + device_count_vblk));
	v->bdev.ops          = &vblk_ops;
	v->bdev.driver_data  = v;
	v->bdev.sector_size  = blk_size;
	v->bdev.sector_count = capacity / (blk_size / 512);
	v->bdev.read_only    = (want & VIRTIO_BLK_F_RO) != 0;

	/* Only now is the device told the driver is ready.  Doing it before
	   the queue address is published would let it process requests
	   against a queue that does not exist. */
	outb(v->io_base + VIRTIO_PCI_STATUS,
	     VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

	dev->driver_data = v;
	devices[device_count_vblk++] = v;

	kinfo("virtio-blk: %s, %llu sectors of %u bytes, queue of %u%s",
	      v->bdev.name, (unsigned long long)v->bdev.sector_count,
	      blk_size, qsize, v->bdev.read_only ? ", read-only" : "");

	rc = block_register(&v->bdev);
	if (rc != 0)
		return rc;

	struct partition parts[MBR_MAX_PARTITIONS];
	int found = block_read_partitions(&v->bdev, parts);
	for (int i = 0; i < found && i < MBR_MAX_PARTITIONS; i++) {
		if (parts[i].present)
			block_register_partition(&v->bdev, (unsigned)i,
			                         &parts[i]);
	}

	return 0;
}

static const struct driver virtio_blk_driver = {
	.name  = "virtio-blk",
	.bus   = BUS_PCI,
	.match = vblk_match,
	.probe = vblk_probe,
};

HAVEN_DRIVER(virtio_blk_driver);

void virtio_blk_init(void)
{
	/* Nothing to do: the driver is in the link-time table and the PCI
	   bus probes it when it finds a matching device.  This function
	   exists only so the boot sequence can report the outcome, which is
	   the half HavenDOS's POST screen got wrong -- it asked whether a
	   device was found before the driver had run. */
	if (device_count_vblk == 0)
		kinfo("virtio-blk: no VirtIO block devices present");
	else
		kinfo("virtio-blk: %u device%s", device_count_vblk,
		      device_count_vblk == 1 ? "" : "s");
}

unsigned virtio_blk_count(void)
{
	return device_count_vblk;
}
