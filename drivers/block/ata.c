/* SPDX-License-Identifier: MIT
 *
 * ata.c — ATA/SATA disks over programmed I/O.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * Inherited in substance from HavenDOS's drivers/ata.c, which did the
 * same job and whose 0.7.x releases added LBA48 and SATA support.  Three
 * things change.
 *
 * First, it registers a block device rather than exporting
 * ata_read_sector() for the filesystem to call, so FAT no longer has to
 * know which transport it is talking to.
 *
 * Second, every wait is bounded.  An unbounded `while (!(status & DRQ))`
 * hangs the whole machine when a drive does not respond, and a drive that
 * does not respond is exactly what happens on the hardware a new OS gets
 * tested on.  HavenDOS's own documentation records the symptom from the
 * other side: its ATA path "no-ops on UTM SE (0x1F0 = 0xFF)" -- that is,
 * it had to special-case reading 0xFF from an absent controller.
 *
 * Third, the floating-bus check is explicit.  When no drive is present
 * the data lines float and every register reads 0xFF, which has the DRQ
 * and ERR bits set, so naive polling sees a drive that is simultaneously
 * ready and faulted.
 *
 * WHY PIO AND NOT DMA
 * PIO is slow -- one 16-bit port read per two bytes -- but it needs no
 * bus-master setup, no physical region descriptor table and no coherent
 * DMA buffer, and it works identically on every ATA controller ever made.
 * For a kernel whose block layer exists so that a faster driver can be
 * added later, correct and universal beats fast.  See
 * docs/DESIGN_DECISIONS.md#storage.
 */
#include <haven/block.h>
#include <haven/device.h>
#include <haven/mm.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/printf.h>
#include <haven/time.h>
#include <haven/arch/x86_64/io.h>
#include <haven/ata.h>

/* The two legacy channels, at the addresses every PC has used since
   1986. */
#define ATA_PRIMARY_IO     0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

/* Register offsets from the I/O base. */
#define ATA_REG_DATA       0
#define ATA_REG_ERROR      1
#define ATA_REG_FEATURES   1
#define ATA_REG_SECCOUNT   2
#define ATA_REG_LBA0       3
#define ATA_REG_LBA1       4
#define ATA_REG_LBA2       5
#define ATA_REG_DRIVE      6
#define ATA_REG_STATUS     7
#define ATA_REG_COMMAND    7

/* Status bits. */
#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_SRV  0x10
#define ATA_SR_DF   0x20
#define ATA_SR_RDY  0x40
#define ATA_SR_BSY  0x80

#define ATA_CMD_READ_PIO       0x20
#define ATA_CMD_READ_PIO_EXT   0x24
#define ATA_CMD_WRITE_PIO      0x30
#define ATA_CMD_WRITE_PIO_EXT  0x34
#define ATA_CMD_CACHE_FLUSH    0xE7
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA
#define ATA_CMD_IDENTIFY       0xEC

/* Timeouts.  A drive that has not responded in this long is not going to:
   the ATA specification allows 31 seconds for a spin-up, but a device
   that takes more than a second to answer IDENTIFY on a system that has
   already been running for a second is absent or broken. */
#define ATA_TIMEOUT_US 1000000

#define ATA_MAX_DRIVES 4

struct ata_drive {
	struct block_device bdev;
	uint16_t io_base;
	uint16_t ctrl_base;
	uint8_t  slave;          /* 0 = master, 1 = slave */
	bool     lba48;
	char     model[41];
};

static struct ata_drive drives[ATA_MAX_DRIVES];
static unsigned         drive_count;

static inline uint8_t ata_status(const struct ata_drive *d)
{
	return inb(d->io_base + ATA_REG_STATUS);
}

/*
 * The 400 ns settling delay the ATA specification requires after
 * selecting a drive or issuing a command.  Four reads of the ALTERNATE
 * status register, which has no side effects -- reading the primary
 * status register would clear a pending interrupt.
 */
static void ata_delay400(const struct ata_drive *d)
{
	for (int i = 0; i < 4; i++)
		(void)inb(d->ctrl_base);
}

/*
 * Wait for BSY to clear, bounded.  Returns 0, or -HAVEN_ETIMEDOUT.
 *
 * The 0xFF check is the floating-bus case: with no drive attached the
 * data lines float high, so every register reads 0xFF.  That value has
 * BSY set, so a loop that only waits for BSY to clear waits forever --
 * and 0xFF also has ERR and DRQ set, so a loop that checks those sees a
 * drive that is ready, faulted and has data available all at once.
 */
static int ata_wait_not_busy(const struct ata_drive *d)
{
	for (uint64_t spin = 0; spin < ATA_TIMEOUT_US; spin++) {
		uint8_t st = ata_status(d);

		if (st == 0xFF)
			return -HAVEN_ENODEV;   /* floating bus: no drive */
		if (!(st & ATA_SR_BSY))
			return 0;

		time_delay_us(1);
	}
	return -HAVEN_ETIMEDOUT;
}

static int ata_wait_drq(const struct ata_drive *d)
{
	for (uint64_t spin = 0; spin < ATA_TIMEOUT_US; spin++) {
		uint8_t st = ata_status(d);

		if (st == 0xFF)
			return -HAVEN_ENODEV;
		if (st & (ATA_SR_ERR | ATA_SR_DF)) {
			uint8_t err = inb(d->io_base + ATA_REG_ERROR);
			kerr("ata: %s reported an error: status 0x%02x, "
			     "error 0x%02x", d->bdev.name, st, err);
			return -HAVEN_EIO;
		}
		if (st & ATA_SR_DRQ)
			return 0;

		time_delay_us(1);
	}
	return -HAVEN_ETIMEDOUT;
}

static void ata_select(struct ata_drive *d, uint64_t lba, bool lba48)
{
	if (lba48) {
		outb(d->io_base + ATA_REG_DRIVE,
		     (uint8_t)(0x40 | (d->slave << 4)));
	} else {
		outb(d->io_base + ATA_REG_DRIVE,
		     (uint8_t)(0xE0 | (d->slave << 4) |
		               ((lba >> 24) & 0x0F)));
	}
	ata_delay400(d);
}

static int ata_transfer(struct ata_drive *d, uint64_t lba, void *buf,
                        uint32_t count, bool write)
{
	/* The sector count is one byte in LBA28 mode and two in LBA48, and
	   zero means 256 (or 65536).  Transferring in bounded chunks keeps
	   that encoding out of the caller's problem. */
	uint32_t max_per_cmd = d->lba48 ? 65536 : 256;
	if (count > max_per_cmd)
		count = max_per_cmd;

	int rc = ata_wait_not_busy(d);
	if (rc != 0)
		return rc;

	bool use48 = d->lba48 && (lba + count > 0x0FFFFFFFULL || count > 256);

	ata_select(d, lba, use48);

	if (use48) {
		/* LBA48 writes the high bytes first, then the low: the
		   registers are a two-deep FIFO and the second write to each
		   pushes the first into the high half. */
		outb(d->io_base + ATA_REG_SECCOUNT, (uint8_t)(count >> 8));
		outb(d->io_base + ATA_REG_LBA0, (uint8_t)(lba >> 24));
		outb(d->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 32));
		outb(d->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 40));

		outb(d->io_base + ATA_REG_SECCOUNT, (uint8_t)(count & 0xFF));
		outb(d->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
		outb(d->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
		outb(d->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));

		outb(d->io_base + ATA_REG_COMMAND,
		     write ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_READ_PIO_EXT);
	} else {
		outb(d->io_base + ATA_REG_SECCOUNT,
		     (uint8_t)(count == 256 ? 0 : count));
		outb(d->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
		outb(d->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
		outb(d->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));

		outb(d->io_base + ATA_REG_COMMAND,
		     write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);
	}

	uint16_t *words = buf;
	uint32_t  per_sector = d->bdev.sector_size / 2;

	for (uint32_t s = 0; s < count; s++) {
		rc = ata_wait_drq(d);
		if (rc != 0) {
			/* A partial transfer is reported as a partial count,
			   so the caller can retry the remainder rather than
			   believing all of it succeeded. */
			return s > 0 ? (int)s : rc;
		}

		if (write) {
			for (uint32_t i = 0; i < per_sector; i++)
				outw(d->io_base + ATA_REG_DATA,
				     words[s * per_sector + i]);

			/* A write must be followed by a cache flush before the
			   data is on the medium.  Skipping it means a power
			   loss after a "successful" write loses it. */
			rc = ata_wait_not_busy(d);
			if (rc != 0)
				return s > 0 ? (int)s : rc;
		} else {
			for (uint32_t i = 0; i < per_sector; i++)
				words[s * per_sector + i] =
					inw(d->io_base + ATA_REG_DATA);
		}
	}

	if (write) {
		outb(d->io_base + ATA_REG_COMMAND,
		     use48 ? ATA_CMD_CACHE_FLUSH_EXT : ATA_CMD_CACHE_FLUSH);
		ata_wait_not_busy(d);
	}

	return (int)count;
}

static ssize_t_k ata_block_read(struct block_device *bdev, uint64_t lba,
                                void *buf, uint32_t count)
{
	struct ata_drive *d = bdev->driver_data;
	uint32_t done = 0;

	while (done < count) {
		int rc = ata_transfer(d, lba + done,
		                      (uint8_t *)buf +
		                      (size_t)done * bdev->sector_size,
		                      count - done, false);
		if (rc <= 0)
			return done ? (ssize_t_k)done : rc;
		done += (uint32_t)rc;
	}
	return (ssize_t_k)done;
}

static ssize_t_k ata_block_write(struct block_device *bdev, uint64_t lba,
                                 const void *buf, uint32_t count)
{
	struct ata_drive *d = bdev->driver_data;
	uint32_t done = 0;

	while (done < count) {
		int rc = ata_transfer(d, lba + done,
		                      (void *)((const uint8_t *)buf +
		                      (size_t)done * bdev->sector_size),
		                      count - done, true);
		if (rc <= 0)
			return done ? (ssize_t_k)done : rc;
		done += (uint32_t)rc;
	}
	return (ssize_t_k)done;
}

static int ata_block_flush(struct block_device *bdev)
{
	struct ata_drive *d = bdev->driver_data;

	int rc = ata_wait_not_busy(d);
	if (rc != 0)
		return rc;

	ata_select(d, 0, d->lba48);
	outb(d->io_base + ATA_REG_COMMAND,
	     d->lba48 ? ATA_CMD_CACHE_FLUSH_EXT : ATA_CMD_CACHE_FLUSH);

	return ata_wait_not_busy(d);
}

static const struct block_ops ata_ops = {
	.read  = ata_block_read,
	.write = ata_block_write,
	.flush = ata_block_flush,
};

/*
 * IDENTIFY one drive.  Returns true if a usable ATA disk answered.
 */
static bool ata_identify(struct ata_drive *d)
{
	/* Select the drive and zero the LBA registers: a non-zero value
	   here makes some controllers answer IDENTIFY for a packet device
	   instead, which returns a different structure. */
	outb(d->io_base + ATA_REG_DRIVE, (uint8_t)(0xA0 | (d->slave << 4)));
	outb(d->io_base + ATA_REG_SECCOUNT, 0);
	outb(d->io_base + ATA_REG_LBA0, 0);
	outb(d->io_base + ATA_REG_LBA1, 0);
	outb(d->io_base + ATA_REG_LBA2, 0);
	ata_delay400(d);

	outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
	ata_delay400(d);

	uint8_t st = ata_status(d);
	if (st == 0 || st == 0xFF)
		return false;   /* nothing there */

	if (ata_wait_not_busy(d) != 0)
		return false;

	/* A non-zero LBA1/LBA2 after IDENTIFY means the device is ATAPI or
	   SATA and answered with a signature instead of identify data.
	   HavenDOS had a separate atapi.c for the CD-ROM case; here the
	   check prevents reading 256 words of signature as geometry. */
	uint8_t lba1 = inb(d->io_base + ATA_REG_LBA1);
	uint8_t lba2 = inb(d->io_base + ATA_REG_LBA2);
	if (lba1 != 0 || lba2 != 0) {
		kdebug("ata: %s:%u answered with signature %02x%02x "
		       "(ATAPI or SATA), not an ATA disk",
		       d->io_base == ATA_PRIMARY_IO ? "primary" : "secondary",
		       d->slave, lba2, lba1);
		return false;
	}

	if (ata_wait_drq(d) != 0)
		return false;

	uint16_t id[256];
	for (int i = 0; i < 256; i++)
		id[i] = inw(d->io_base + ATA_REG_DATA);

	/* The model string is 40 bytes at word 27, with each pair of bytes
	   byte-swapped -- a detail that produces an unreadable name if
	   missed. */
	for (int i = 0; i < 20; i++) {
		d->model[i * 2]     = (char)(id[27 + i] >> 8);
		d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
	}
	d->model[40] = '\0';
	for (int i = 39; i >= 0 && (d->model[i] == ' ' || d->model[i] == '\0');
	     i--)
		d->model[i] = '\0';

	/* Word 83 bit 10 advertises LBA48.  Without it, words 60-61 give a
	   28-bit sector count, capping the usable disk at 128 GiB. */
	d->lba48 = (id[83] & (1u << 10)) != 0;

	uint64_t sectors;
	if (d->lba48) {
		sectors = (uint64_t)id[100]
		        | ((uint64_t)id[101] << 16)
		        | ((uint64_t)id[102] << 32)
		        | ((uint64_t)id[103] << 48);
	} else {
		sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
	}

	if (sectors == 0) {
		kwarn("ata: a drive answered IDENTIFY but reports zero "
		      "sectors; ignoring it");
		return false;
	}

	d->bdev.sector_size  = 512;
	d->bdev.sector_count = sectors;
	d->bdev.ops          = &ata_ops;
	d->bdev.driver_data  = d;

	return true;
}

static void probe_channel(uint16_t io_base, uint16_t ctrl_base,
                          const char *channel)
{
	for (uint8_t slave = 0; slave < 2; slave++) {
		if (drive_count >= ATA_MAX_DRIVES)
			return;

		struct ata_drive *d = &drives[drive_count];
		memset(d, 0, sizeof(*d));
		d->io_base   = io_base;
		d->ctrl_base = ctrl_base;
		d->slave     = slave;

		snprintf(d->bdev.name, sizeof(d->bdev.name), "hd%c",
		         (char)('a' + drive_count));

		if (!ata_identify(d))
			continue;

		kinfo("ata: %s %s: %s, %llu sectors, LBA%s",
		      channel, slave ? "slave" : "master", d->model,
		      (unsigned long long)d->bdev.sector_count,
		      d->lba48 ? "48" : "28");

		if (block_register(&d->bdev) != 0)
			continue;

		/* Parse and publish partitions, so a filesystem mounts
		   /dev/hdap1 rather than computing an offset. */
		struct partition parts[MBR_MAX_PARTITIONS];
		int found = block_read_partitions(&d->bdev, parts);
		for (int i = 0; i < found && i < MBR_MAX_PARTITIONS; i++) {
			if (parts[i].present)
				block_register_partition(&d->bdev,
				                         (unsigned)i,
				                         &parts[i]);
		}

		drive_count++;
	}
}

void ata_init(void)
{
	drive_count = 0;

	/* Disable interrupts on both channels: this driver polls, and an
	   unexpected IRQ14 with no handler would be reported as unclaimed
	   on every transfer. */
	outb(ATA_PRIMARY_CTRL, 0x02);
	outb(ATA_SECONDARY_CTRL, 0x02);

	probe_channel(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, "primary");
	probe_channel(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, "secondary");

	if (drive_count == 0) {
		/* Not an error.  A machine booting from a VirtIO disk or from
		   a USB stick legitimately has no ATA drive, and HavenDOS's
		   own notes describe exactly this case on UTM SE, where
		   0x1F0 reads 0xFF. */
		kinfo("ata: no ATA disks found (the controller ports read as "
		      "a floating bus, which is normal on a machine with no "
		      "IDE/SATA drive)");
	} else {
		kinfo("ata: %u disk%s", drive_count,
		      drive_count == 1 ? "" : "s");
	}
}

unsigned ata_drive_count(void)
{
	return drive_count;
}
