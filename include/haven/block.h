/* SPDX-License-Identifier: MIT
 *
 * block.h — the block device layer.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had no block layer.  Its FAT16 code called ata_read_sector()
 * or virtio_read_sector() depending on which of two file-static booleans
 * was set, which is why fat16_init() had to "probe VirtIO then ATA
 * internally" and why its own VFS comment has to explain that
 * fat16_detected() "returns 1 in both cases if a valid partition was
 * found, so the boot decision in main.c works identically on both
 * platforms".  The filesystem knew about transports.
 *
 * Here a filesystem asks a struct block_device to read LBA n.  Whether
 * that is ATA PIO, a VirtIO queue or an NVMe submission is the device's
 * business.  Adding NVMe -- which HavenDOS lists as unsupported and
 * documents as the reason its tested laptop could not install -- becomes
 * a new driver rather than a third branch in the filesystem.
 */
#ifndef _HAVEN_BLOCK_H
#define _HAVEN_BLOCK_H

#include <haven/types.h>
#include <haven/list.h>
#include <haven/mutex.h>

#define BLOCK_NAME_MAX    32
#define BLOCK_SECTOR_SIZE 512
#define BLOCK_MAX_DEVICES 16

struct block_device;

struct block_ops {
	/* Read or write `count` sectors starting at `lba`.  Returns the
	   number of sectors transferred, or -HAVEN_E*.  A short transfer is
	   reported as a short count, not as success -- which is the
	   distinction that lets a filesystem retry the tail rather than
	   believing data it never wrote. */
	ssize_t_k (*read)(struct block_device *dev, uint64_t lba,
	                  void *buf, uint32_t count);
	ssize_t_k (*write)(struct block_device *dev, uint64_t lba,
	                   const void *buf, uint32_t count);
	int       (*flush)(struct block_device *dev);
};

struct block_device {
	char     name[BLOCK_NAME_MAX];
	const struct block_ops *ops;
	void    *driver_data;

	uint32_t sector_size;
	uint64_t sector_count;
	bool     read_only;
	bool     removable;

	/* One lock per device.  Two threads issuing overlapping transfers
	   to the same ATA channel would interleave their register writes
	   and each read the other's data. */
	struct mutex lock;

	struct list_head node;
	uint64_t sectors_read;
	uint64_t sectors_written;
	uint64_t errors;
};

void block_init(void);

/* Register a device.  Also publishes it as /dev/<name>. */
int  block_register(struct block_device *dev);
int  block_unregister(struct block_device *dev);

struct block_device *block_find(const char *name);
struct block_device *block_by_index(unsigned index);
unsigned             block_count(void);

/* Sector-granular access, which is what a filesystem uses. */
ssize_t_k block_read(struct block_device *dev, uint64_t lba, void *buf,
                     uint32_t count);
ssize_t_k block_write(struct block_device *dev, uint64_t lba,
                      const void *buf, uint32_t count);
int       block_flush(struct block_device *dev);

/* Byte-granular access, for a caller that does not want to think in
   sectors.  Handles the read-modify-write of a partial sector, which is
   the part that is easy to get wrong. */
ssize_t_k block_read_bytes(struct block_device *dev, uint64_t offset,
                           void *buf, size_t len);
ssize_t_k block_write_bytes(struct block_device *dev, uint64_t offset,
                            const void *buf, size_t len);

void block_dump(void);

/* ── Partitions ──────────────────────────────────────────────────────── */

#define MBR_MAX_PARTITIONS 4

struct partition {
	uint64_t start_lba;
	uint64_t sector_count;
	uint8_t  type;
	bool     bootable;
	bool     present;
};

/* Read and parse the MBR partition table.  HavenDOS's installer wrote an
   MBR with a FAT16 partition and its fat16_init() looked for exactly
   that; parsing the table properly means a HavenOS install can coexist
   with other partitions on the same disk. */
int block_read_partitions(struct block_device *dev,
                          struct partition out[MBR_MAX_PARTITIONS]);

/* Register a partition as its own block device, named <disk>p<n>, so a
   filesystem mounts a partition rather than computing offsets. */
int block_register_partition(struct block_device *parent, unsigned index,
                             const struct partition *part);

#define MBR_TYPE_EMPTY     0x00
#define MBR_TYPE_FAT12     0x01
#define MBR_TYPE_FAT16_SM  0x04
#define MBR_TYPE_EXTENDED  0x05
#define MBR_TYPE_FAT16     0x06
#define MBR_TYPE_NTFS      0x07
#define MBR_TYPE_FAT32     0x0B
#define MBR_TYPE_FAT32_LBA 0x0C
#define MBR_TYPE_FAT16_LBA 0x0E
#define MBR_TYPE_LINUX     0x83
#define MBR_TYPE_HAVENOS   0x7E   /* HavenOS native, not yet implemented */

const char *mbr_type_name(uint8_t type);

#endif /* _HAVEN_BLOCK_H */
