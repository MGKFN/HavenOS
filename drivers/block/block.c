/* SPDX-License-Identifier: MIT
 *
 * block.c — the block device layer and MBR partition parsing.
 */
#include <haven/block.h>
#include <haven/devfs.h>
#include <haven/mm.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/printf.h>
#include <haven/spinlock.h>

static LIST_HEAD(block_devices);
static DEFINE_SPINLOCK(block_lock, "block-devices");
static unsigned block_total;

/* A partition is a block device that forwards to its parent with an
   offset, so a filesystem mounted on a partition cannot reach outside
   it -- which is the containment a bare offset calculation in the
   filesystem would not provide. */
struct partition_dev {
	struct block_device  bdev;
	struct block_device *parent;
	uint64_t             start_lba;
};

const char *mbr_type_name(uint8_t type)
{
	switch (type) {
	case MBR_TYPE_EMPTY:     return "empty";
	case MBR_TYPE_FAT12:     return "FAT12";
	case MBR_TYPE_FAT16_SM:  return "FAT16 (<32 MiB)";
	case MBR_TYPE_EXTENDED:  return "extended";
	case MBR_TYPE_FAT16:     return "FAT16";
	case MBR_TYPE_NTFS:      return "NTFS/exFAT";
	case MBR_TYPE_FAT32:     return "FAT32";
	case MBR_TYPE_FAT32_LBA: return "FAT32 (LBA)";
	case MBR_TYPE_FAT16_LBA: return "FAT16 (LBA)";
	case MBR_TYPE_LINUX:     return "Linux";
	case MBR_TYPE_HAVENOS:   return "HavenOS";
	default:                 return "unknown";
	}
}

/* ── devfs bridge ────────────────────────────────────────────────────── */

static ssize_t_k block_devfs_read(void *data, void *buf, size_t len,
                                  haven_off_t offset)
{
	return block_read_bytes((struct block_device *)data, offset, buf, len);
}

static ssize_t_k block_devfs_write(void *data, const void *buf, size_t len,
                                   haven_off_t offset)
{
	return block_write_bytes((struct block_device *)data, offset, buf, len);
}

static const struct devfs_driver block_devfs_driver = {
	.name  = "block",
	.read  = block_devfs_read,
	.write = block_devfs_write,
};

/* ── Registration ────────────────────────────────────────────────────── */

int block_register(struct block_device *dev)
{
	if (!dev || !dev->ops || !dev->ops->read)
		return -HAVEN_EINVAL;
	if (dev->sector_size == 0)
		dev->sector_size = BLOCK_SECTOR_SIZE;

	irqflags_t f = spin_lock(&block_lock);

	struct block_device *pos;
	list_for_each_entry(pos, &block_devices, node) {
		if (strcmp(pos->name, dev->name) == 0) {
			spin_unlock(&block_lock, f);
			kerr("block: '%s' is already registered", dev->name);
			return -HAVEN_EEXIST;
		}
	}

	if (block_total >= BLOCK_MAX_DEVICES) {
		spin_unlock(&block_lock, f);
		return -HAVEN_ENOSPC;
	}

	mutex_init(&dev->lock, dev->name);
	list_init(&dev->node);
	list_add_tail(&dev->node, &block_devices);
	block_total++;
	spin_unlock(&block_lock, f);

	uint64_t mib = (dev->sector_count * dev->sector_size) / (1024 * 1024);
	kinfo("block: %s, %llu sectors of %u bytes (%llu MiB)%s",
	      dev->name, (unsigned long long)dev->sector_count,
	      dev->sector_size, (unsigned long long)mib,
	      dev->read_only ? ", read-only" : "");

	/* Publish as /dev/<name>, so `dd` and a partition editor are
	   ordinary programs rather than kernel features. */
	devfs_register(dev->name, &block_devfs_driver, dev,
	               VFS_NODE_BLOCKDEV, DEV_MAJOR_BLOCK, block_total,
	               dev->read_only ? 0444 : 0660);

	return 0;
}

int block_unregister(struct block_device *dev)
{
	if (!dev)
		return -HAVEN_EINVAL;

	irqflags_t f = spin_lock(&block_lock);
	list_del(&dev->node);
	if (block_total)
		block_total--;
	spin_unlock(&block_lock, f);

	devfs_unregister(dev->name);
	return 0;
}

struct block_device *block_find(const char *name)
{
	irqflags_t f = spin_lock(&block_lock);
	struct block_device *pos;
	list_for_each_entry(pos, &block_devices, node) {
		if (strcmp(pos->name, name) == 0) {
			spin_unlock(&block_lock, f);
			return pos;
		}
	}
	spin_unlock(&block_lock, f);
	return NULL;
}

struct block_device *block_by_index(unsigned index)
{
	irqflags_t f = spin_lock(&block_lock);
	unsigned seen = 0;
	struct block_device *pos;
	list_for_each_entry(pos, &block_devices, node) {
		if (seen++ == index) {
			spin_unlock(&block_lock, f);
			return pos;
		}
	}
	spin_unlock(&block_lock, f);
	return NULL;
}

unsigned block_count(void) { return block_total; }

/* ── Sector access ───────────────────────────────────────────────────── */

ssize_t_k block_read(struct block_device *dev, uint64_t lba, void *buf,
                     uint32_t count)
{
	if (!dev || !buf)
		return -HAVEN_EINVAL;
	if (count == 0)
		return 0;

	/* Bounds-check against the device's real size.  Without this, a
	   filesystem with a corrupt superblock would read whatever the
	   controller returns past the end of the medium -- which on some
	   devices is the start of it again. */
	if (lba >= dev->sector_count || count > dev->sector_count - lba) {
		kerr("block: %s read of %u sectors at LBA %llu is past the "
		     "end of the device (%llu sectors)",
		     dev->name, count, (unsigned long long)lba,
		     (unsigned long long)dev->sector_count);
		return -HAVEN_EINVAL;
	}

	mutex_lock(&dev->lock);
	ssize_t_k rc = dev->ops->read(dev, lba, buf, count);
	if (rc > 0)
		dev->sectors_read += (uint64_t)rc;
	else
		dev->errors++;
	mutex_unlock(&dev->lock);

	return rc;
}

ssize_t_k block_write(struct block_device *dev, uint64_t lba,
                      const void *buf, uint32_t count)
{
	if (!dev || !buf)
		return -HAVEN_EINVAL;
	if (count == 0)
		return 0;
	if (dev->read_only)
		return -HAVEN_EROFS;
	if (!dev->ops->write)
		return -HAVEN_ENOSYS;

	if (lba >= dev->sector_count || count > dev->sector_count - lba) {
		kerr("block: %s write of %u sectors at LBA %llu is past the "
		     "end of the device", dev->name, count,
		     (unsigned long long)lba);
		return -HAVEN_EINVAL;
	}

	mutex_lock(&dev->lock);
	ssize_t_k rc = dev->ops->write(dev, lba, buf, count);
	if (rc > 0)
		dev->sectors_written += (uint64_t)rc;
	else
		dev->errors++;
	mutex_unlock(&dev->lock);

	return rc;
}

int block_flush(struct block_device *dev)
{
	if (!dev)
		return -HAVEN_EINVAL;
	if (!dev->ops->flush)
		return 0;   /* nothing cached is not a failure */

	mutex_lock(&dev->lock);
	int rc = dev->ops->flush(dev);
	mutex_unlock(&dev->lock);
	return rc;
}

/* ── Byte access ─────────────────────────────────────────────────────
   A device can only transfer whole sectors, so a byte-granular read of a
   range that does not start or end on a sector boundary needs a bounce
   buffer, and a byte-granular WRITE needs a read-modify-write of the
   partial sectors at each end.  Getting the partial-sector case wrong
   destroys the bytes either side of the written range, which is a
   particularly nasty filesystem corruption. */

ssize_t_k block_read_bytes(struct block_device *dev, uint64_t offset,
                           void *buf, size_t len)
{
	if (!dev || !buf)
		return -HAVEN_EINVAL;
	if (len == 0)
		return 0;

	uint32_t ss = dev->sector_size;
	uint64_t total_bytes = dev->sector_count * ss;

	if (offset >= total_bytes)
		return 0;   /* past the end reads as end of file */
	if (len > total_bytes - offset)
		len = (size_t)(total_bytes - offset);

	uint8_t *sector = kmalloc(ss);
	if (!sector)
		return -HAVEN_ENOMEM;

	uint8_t *out  = buf;
	size_t   done = 0;

	while (done < len) {
		uint64_t pos    = offset + done;
		uint64_t lba    = pos / ss;
		uint32_t in_sec = (uint32_t)(pos % ss);
		size_t   chunk  = ss - in_sec;
		if (chunk > len - done)
			chunk = len - done;

		if (in_sec == 0 && chunk == ss) {
			/* Whole sectors: read them straight into the caller's
			   buffer, as many as are contiguous. */
			uint32_t nsec = (uint32_t)((len - done) / ss);
			ssize_t_k got = block_read(dev, lba, out + done, nsec);
			if (got <= 0) {
				kfree(sector);
				return done ? (ssize_t_k)done : got;
			}
			done += (size_t)got * ss;
			continue;
		}

		ssize_t_k got = block_read(dev, lba, sector, 1);
		if (got <= 0) {
			kfree(sector);
			return done ? (ssize_t_k)done : got;
		}
		memcpy(out + done, sector + in_sec, chunk);
		done += chunk;
	}

	kfree(sector);
	return (ssize_t_k)done;
}

ssize_t_k block_write_bytes(struct block_device *dev, uint64_t offset,
                            const void *buf, size_t len)
{
	if (!dev || !buf)
		return -HAVEN_EINVAL;
	if (len == 0)
		return 0;
	if (dev->read_only)
		return -HAVEN_EROFS;

	uint32_t ss = dev->sector_size;
	uint64_t total_bytes = dev->sector_count * ss;

	if (offset >= total_bytes)
		return -HAVEN_ENOSPC;
	if (len > total_bytes - offset)
		len = (size_t)(total_bytes - offset);

	uint8_t *sector = kmalloc(ss);
	if (!sector)
		return -HAVEN_ENOMEM;

	const uint8_t *in   = buf;
	size_t         done = 0;

	while (done < len) {
		uint64_t pos    = offset + done;
		uint64_t lba    = pos / ss;
		uint32_t in_sec = (uint32_t)(pos % ss);
		size_t   chunk  = ss - in_sec;
		if (chunk > len - done)
			chunk = len - done;

		if (in_sec == 0 && chunk == ss) {
			uint32_t nsec = (uint32_t)((len - done) / ss);
			ssize_t_k put = block_write(dev, lba, in + done, nsec);
			if (put <= 0) {
				kfree(sector);
				return done ? (ssize_t_k)done : put;
			}
			done += (size_t)put * ss;
			continue;
		}

		/* Partial sector: read it, modify the part being written, and
		   write the whole sector back.  Skipping the read would zero
		   the bytes either side of the range. */
		ssize_t_k got = block_read(dev, lba, sector, 1);
		if (got <= 0) {
			kfree(sector);
			return done ? (ssize_t_k)done : got;
		}

		memcpy(sector + in_sec, in + done, chunk);

		ssize_t_k put = block_write(dev, lba, sector, 1);
		if (put <= 0) {
			kfree(sector);
			return done ? (ssize_t_k)done : put;
		}
		done += chunk;
	}

	kfree(sector);
	return (ssize_t_k)done;
}

/* ── Partitions ──────────────────────────────────────────────────────── */

struct HAVEN_PACKED mbr_entry {
	uint8_t  status;
	uint8_t  chs_first[3];
	uint8_t  type;
	uint8_t  chs_last[3];
	uint32_t lba_first;
	uint32_t sector_count;
};

HAVEN_STATIC_ASSERT(sizeof(struct mbr_entry) == 16,
                    "an MBR partition entry is 16 bytes");

int block_read_partitions(struct block_device *dev,
                          struct partition out[MBR_MAX_PARTITIONS])
{
	if (!dev || !out)
		return -HAVEN_EINVAL;

	memset(out, 0, sizeof(struct partition) * MBR_MAX_PARTITIONS);

	uint8_t *sector = kmalloc(dev->sector_size);
	if (!sector)
		return -HAVEN_ENOMEM;

	ssize_t_k got = block_read(dev, 0, sector, 1);
	if (got != 1) {
		kfree(sector);
		return got < 0 ? (int)got : -HAVEN_EIO;
	}

	/* The boot signature.  Without it the 64 bytes at offset 446 are
	   not a partition table, and interpreting them as one produces
	   partitions at random offsets -- which a filesystem would then
	   try to mount. */
	if (sector[510] != 0x55 || sector[511] != 0xAA) {
		kdebug("block: %s has no MBR signature (0x%02x%02x); treating "
		       "it as unpartitioned", dev->name,
		       sector[510], sector[511]);
		kfree(sector);
		return 0;
	}

	const struct mbr_entry *e = (const struct mbr_entry *)(sector + 446);
	int found = 0;

	for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
		if (e[i].type == MBR_TYPE_EMPTY || e[i].sector_count == 0)
			continue;

		/* A partition that claims to extend past the device is a
		   corrupt or hostile table.  Clamping it rather than
		   rejecting the whole disk keeps a usable partition usable,
		   but the clamp must happen: a filesystem given the
		   unclamped size would read past the medium. */
		uint64_t start = e[i].lba_first;
		uint64_t count = e[i].sector_count;

		if (start >= dev->sector_count) {
			kwarn("block: %s partition %d starts at LBA %llu, "
			      "past the end of the device; ignoring",
			      dev->name, i + 1, (unsigned long long)start);
			continue;
		}
		if (count > dev->sector_count - start) {
			kwarn("block: %s partition %d claims %llu sectors but "
			      "only %llu remain; clamping",
			      dev->name, i + 1, (unsigned long long)count,
			      (unsigned long long)(dev->sector_count - start));
			count = dev->sector_count - start;
		}

		out[i].start_lba    = start;
		out[i].sector_count = count;
		out[i].type         = e[i].type;
		out[i].bootable     = (e[i].status & 0x80) != 0;
		out[i].present      = true;
		found++;

		kinfo("block: %s partition %d: LBA %llu + %llu sectors, "
		      "type 0x%02x (%s)%s",
		      dev->name, i + 1, (unsigned long long)start,
		      (unsigned long long)count, e[i].type,
		      mbr_type_name(e[i].type),
		      out[i].bootable ? ", bootable" : "");
	}

	kfree(sector);
	return found;
}

static ssize_t_k part_read(struct block_device *dev, uint64_t lba,
                           void *buf, uint32_t count)
{
	struct partition_dev *p = dev->driver_data;
	return block_read(p->parent, p->start_lba + lba, buf, count);
}

static ssize_t_k part_write(struct block_device *dev, uint64_t lba,
                            const void *buf, uint32_t count)
{
	struct partition_dev *p = dev->driver_data;
	return block_write(p->parent, p->start_lba + lba, buf, count);
}

static int part_flush(struct block_device *dev)
{
	struct partition_dev *p = dev->driver_data;
	return block_flush(p->parent);
}

static const struct block_ops partition_ops = {
	.read  = part_read,
	.write = part_write,
	.flush = part_flush,
};

int block_register_partition(struct block_device *parent, unsigned index,
                             const struct partition *part)
{
	if (!parent || !part || !part->present)
		return -HAVEN_EINVAL;

	struct partition_dev *p = kzalloc(sizeof(*p));
	if (!p)
		return -HAVEN_ENOMEM;

	p->parent    = parent;
	p->start_lba = part->start_lba;

	snprintf(p->bdev.name, sizeof(p->bdev.name), "%sp%u",
	         parent->name, index + 1);
	p->bdev.ops          = &partition_ops;
	p->bdev.driver_data  = p;
	p->bdev.sector_size  = parent->sector_size;
	/* The partition's size, not the disk's: this is what confines a
	   filesystem to its own partition.  block_read() bounds-checks
	   against it, so a filesystem with a corrupt superblock cannot read
	   its neighbour's data. */
	p->bdev.sector_count = part->sector_count;
	p->bdev.read_only    = parent->read_only;
	p->bdev.removable    = parent->removable;

	int rc = block_register(&p->bdev);
	if (rc != 0)
		kfree(p);
	return rc;
}

void block_dump(void)
{
	kinfo("block: %u devices", block_total);

	irqflags_t f = spin_lock(&block_lock);
	struct block_device *pos;
	list_for_each_entry(pos, &block_devices, node) {
		kinfo("block:   %-12s %10llu sectors  r:%llu w:%llu e:%llu",
		      pos->name, (unsigned long long)pos->sector_count,
		      (unsigned long long)pos->sectors_read,
		      (unsigned long long)pos->sectors_written,
		      (unsigned long long)pos->errors);
	}
	spin_unlock(&block_lock, f);
}

void block_init(void)
{
	list_init(&block_devices);
	spin_lock_init(&block_lock, "block-devices");
	block_total = 0;

	kinfo("block: device layer ready; filesystems address LBAs, not "
	      "transports");
}
