/* SPDX-License-Identifier: MIT
 *
 * fat.c — FAT12/16/32, one implementation.
 */
#include <haven/fat.h>
#include <haven/vfs.h>
#include <haven/block.h>
#include <haven/mm.h>
#include <haven/mutex.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/printf.h>
#include <haven/time.h>

/* ── On-disk structures ──────────────────────────────────────────────── */

struct HAVEN_PACKED fat_bpb {
	uint8_t  jmp[3];
	char     oem[8];
	uint16_t bytes_per_sector;
	uint8_t  sectors_per_cluster;
	uint16_t reserved_sectors;
	uint8_t  num_fats;
	uint16_t root_entries;        /* 0 on FAT32 */
	uint16_t total_sectors_16;
	uint8_t  media;
	uint16_t fat_size_16;         /* 0 on FAT32 */
	uint16_t sectors_per_track;
	uint16_t num_heads;
	uint32_t hidden_sectors;
	uint32_t total_sectors_32;

	/* FAT32 extension; meaningless when fat_size_16 is non-zero. */
	uint32_t fat_size_32;
	uint16_t ext_flags;
	uint16_t fs_version;
	uint32_t root_cluster;
	uint16_t fs_info_sector;
	uint16_t backup_boot_sector;
	uint8_t  reserved[12];
	uint8_t  drive_number;
	uint8_t  reserved1;
	uint8_t  boot_signature;
	uint32_t volume_id;
	char     volume_label[11];
	char     fs_type[8];
};

HAVEN_STATIC_ASSERT(sizeof(struct fat_bpb) == 90,
                    "the FAT boot parameter block layout is fixed");
HAVEN_STATIC_ASSERT(offsetof(struct fat_bpb, root_cluster) == 44,
                    "FAT32 root cluster must be at offset 44");

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F   /* the four flags above, all set */

struct HAVEN_PACKED fat_dirent {
	char     name[11];        /* 8.3, space-padded, no separator */
	uint8_t  attr;
	uint8_t  nt_reserved;
	uint8_t  create_time_tenths;
	uint16_t create_time;
	uint16_t create_date;
	uint16_t access_date;
	uint16_t cluster_high;    /* FAT32 only */
	uint16_t modify_time;
	uint16_t modify_date;
	uint16_t cluster_low;
	uint32_t size;
};

HAVEN_STATIC_ASSERT(sizeof(struct fat_dirent) == 32,
                    "a FAT directory entry is 32 bytes");

/* A VFAT long-name entry occupies a directory slot and is distinguished
   by its attribute byte.  Thirteen UTF-16 characters per entry, in three
   runs, which is why the name fields are split. */
struct HAVEN_PACKED fat_lfn {
	uint8_t  sequence;
	uint16_t name1[5];
	uint8_t  attr;            /* always FAT_ATTR_LFN */
	uint8_t  type;
	uint8_t  checksum;        /* of the matching short name */
	uint16_t name2[6];
	uint16_t zero;
	uint16_t name3[2];
};

HAVEN_STATIC_ASSERT(sizeof(struct fat_lfn) == 32,
                    "a VFAT long-name entry is 32 bytes");

#define FAT_LFN_LAST  0x40
#define FAT_LFN_SEQ   0x1F

#define DIRENT_FREE    0xE5
#define DIRENT_END     0x00

/* ── In-memory state ─────────────────────────────────────────────────── */

struct fat_super {
	struct block_device *dev;
	enum fat_variant     variant;

	uint32_t bytes_per_sector;
	uint32_t sectors_per_cluster;
	uint32_t bytes_per_cluster;
	uint32_t first_fat_sector;
	uint32_t fat_sectors;
	uint32_t num_fats;
	uint32_t first_data_sector;
	uint32_t root_entries;        /* FAT12/16 */
	uint32_t root_dir_sectors;    /* FAT12/16 */
	uint32_t first_root_sector;   /* FAT12/16 */
	uint32_t root_cluster;        /* FAT32 */
	uint32_t total_clusters;
	uint32_t eoc_marker;

	struct vfs_node root;
	struct mutex    lock;
	bool            read_only;

	/* One cluster-sized scratch buffer, allocated once.  A per-call
	   allocation would put a kmalloc on the path of every directory
	   lookup. */
	uint8_t *scratch;
};

/* A FAT node's identity is where its directory entry lives, because FAT
   has no inode number: the entry IS the metadata.  So a node records the
   sector and offset of its own entry, which is what makes updating a
   file's size on write possible. */
struct fat_node {
	struct vfs_node vnode;
	struct fat_super *sb;
	uint32_t first_cluster;
	uint32_t entry_sector;
	uint32_t entry_offset;
	bool     is_root;
};

const char *fat_variant_name(enum fat_variant v)
{
	switch (v) {
	case FAT12: return "FAT12";
	case FAT16: return "FAT16";
	case FAT32: return "FAT32";
	default:    return "not FAT";
	}
}

/* ── Cluster arithmetic ──────────────────────────────────────────────── */

static uint32_t cluster_to_sector(const struct fat_super *sb, uint32_t cluster)
{
	/* Clusters are numbered from 2: 0 and 1 are reserved and hold the
	   media descriptor and the end-of-chain marker in the FAT itself.
	   Forgetting the -2 shifts every file by two clusters, which reads
	   plausible-looking garbage. */
	return sb->first_data_sector +
	       (cluster - 2) * sb->sectors_per_cluster;
}

static bool cluster_valid(const struct fat_super *sb, uint32_t cluster)
{
	return cluster >= 2 && cluster < sb->total_clusters + 2;
}

/*
 * Read one FAT entry.
 *
 * FAT12 is the awkward one: entries are 12 bits, so two entries share
 * three bytes and an entry can straddle a sector boundary.  Reading it as
 * a 16-bit value at (cluster * 3 / 2) and shifting is the standard
 * approach, and the straddle is handled by reading two bytes
 * individually rather than one 16-bit value, which would fault on an
 * odd offset at the end of the buffer.
 */
static int fat_entry_read(struct fat_super *sb, uint32_t cluster,
                          uint32_t *out)
{
	if (cluster > sb->total_clusters + 1)
		return -HAVEN_EINVAL;

	uint64_t byte_offset;
	uint32_t width;

	switch (sb->variant) {
	case FAT12: byte_offset = (uint64_t)cluster * 3 / 2; width = 2; break;
	case FAT16: byte_offset = (uint64_t)cluster * 2;     width = 2; break;
	case FAT32: byte_offset = (uint64_t)cluster * 4;     width = 4; break;
	default:    return -HAVEN_EINVAL;
	}

	uint8_t  raw[4] = { 0 };
	uint64_t pos = (uint64_t)sb->first_fat_sector * sb->bytes_per_sector
	             + byte_offset;

	ssize_t_k got = block_read_bytes(sb->dev, pos, raw, width);
	if (got != (ssize_t_k)width)
		return got < 0 ? (int)got : -HAVEN_EIO;

	uint32_t value;
	switch (sb->variant) {
	case FAT12:
		value = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8);
		/* An odd cluster's entry is in the high 12 bits of the pair. */
		value = (cluster & 1) ? (value >> 4) : (value & 0x0FFF);
		break;
	case FAT16:
		value = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8);
		break;
	case FAT32:
	default:
		value = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8)
		      | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
		/* The top four bits of a FAT32 entry are reserved and must be
		   masked off; treating them as part of the cluster number
		   produces an out-of-range cluster on some formatters. */
		value &= 0x0FFFFFFF;
		break;
	}

	*out = value;
	return 0;
}

static int fat_entry_write(struct fat_super *sb, uint32_t cluster,
                           uint32_t value)
{
	if (sb->read_only)
		return -HAVEN_EROFS;
	if (cluster > sb->total_clusters + 1)
		return -HAVEN_EINVAL;

	uint64_t byte_offset;
	uint32_t width;

	switch (sb->variant) {
	case FAT12: byte_offset = (uint64_t)cluster * 3 / 2; width = 2; break;
	case FAT16: byte_offset = (uint64_t)cluster * 2;     width = 2; break;
	case FAT32: byte_offset = (uint64_t)cluster * 4;     width = 4; break;
	default:    return -HAVEN_EINVAL;
	}

	/* Write the same entry into every FAT copy.  A filesystem with two
	   FATs that disagree is one that chkdsk will "repair" by picking
	   one, and the one it picks may not be ours. */
	for (uint32_t copy = 0; copy < sb->num_fats; copy++) {
		uint64_t pos = ((uint64_t)sb->first_fat_sector +
		                (uint64_t)copy * sb->fat_sectors) *
		               sb->bytes_per_sector + byte_offset;

		uint8_t raw[4] = { 0 };
		ssize_t_k got = block_read_bytes(sb->dev, pos, raw, width);
		if (got != (ssize_t_k)width)
			return got < 0 ? (int)got : -HAVEN_EIO;

		switch (sb->variant) {
		case FAT12: {
			uint16_t pair = (uint16_t)(raw[0] | (raw[1] << 8));
			if (cluster & 1)
				pair = (uint16_t)((pair & 0x000F) |
				                  ((value & 0x0FFF) << 4));
			else
				pair = (uint16_t)((pair & 0xF000) |
				                  (value & 0x0FFF));
			raw[0] = (uint8_t)(pair & 0xFF);
			raw[1] = (uint8_t)(pair >> 8);
			break;
		}
		case FAT16:
			raw[0] = (uint8_t)(value & 0xFF);
			raw[1] = (uint8_t)((value >> 8) & 0xFF);
			break;
		case FAT32:
		default:
			/* Preserve the reserved top four bits rather than
			   zeroing them: some formatters use them and a driver
			   that clears them changes metadata it does not
			   understand. */
			raw[0] = (uint8_t)(value & 0xFF);
			raw[1] = (uint8_t)((value >> 8) & 0xFF);
			raw[2] = (uint8_t)((value >> 16) & 0xFF);
			raw[3] = (uint8_t)((raw[3] & 0xF0) |
			                   ((value >> 24) & 0x0F));
			break;
		}

		ssize_t_k put = block_write_bytes(sb->dev, pos, raw, width);
		if (put != (ssize_t_k)width)
			return put < 0 ? (int)put : -HAVEN_EIO;
	}

	return 0;
}

static bool is_eoc(const struct fat_super *sb, uint32_t entry)
{
	switch (sb->variant) {
	case FAT12: return entry >= 0x0FF8;
	case FAT16: return entry >= 0xFFF8;
	case FAT32: return entry >= 0x0FFFFFF8;
	default:    return true;
	}
}

/* Find a free cluster and claim it, returning its number or 0. */
static uint32_t cluster_alloc(struct fat_super *sb)
{
	for (uint32_t c = 2; c < sb->total_clusters + 2; c++) {
		uint32_t entry;
		if (fat_entry_read(sb, c, &entry) != 0)
			return 0;
		if (entry != 0)
			continue;

		if (fat_entry_write(sb, c, sb->eoc_marker) != 0)
			return 0;

		/* Zero the cluster.  A newly allocated cluster containing a
		   deleted file's data would expose it, and a directory
		   cluster must start as all-free entries. */
		memset(sb->scratch, 0, sb->bytes_per_cluster);
		uint64_t pos = (uint64_t)cluster_to_sector(sb, c) *
		               sb->bytes_per_sector;
		if (block_write_bytes(sb->dev, pos, sb->scratch,
		                      sb->bytes_per_cluster) < 0) {
			fat_entry_write(sb, c, 0);
			return 0;
		}

		return c;
	}

	kerr("fat: no free clusters on %s", sb->dev->name);
	return 0;
}

static void chain_free(struct fat_super *sb, uint32_t first)
{
	uint32_t c = first;
	uint32_t guard = 0;

	while (cluster_valid(sb, c)) {
		/* A corrupt FAT can contain a cycle, which would make this
		   loop forever.  The bound is the cluster count, past which
		   the chain cannot be legitimate. */
		if (++guard > sb->total_clusters) {
			kerr("fat: cluster chain from %u does not terminate; "
			     "the FAT is corrupt", first);
			return;
		}

		uint32_t next;
		if (fat_entry_read(sb, c, &next) != 0)
			return;

		fat_entry_write(sb, c, 0);

		if (is_eoc(sb, next))
			return;
		c = next;
	}
}

/* Walk `index` links along a chain.  Returns 0 if the chain is shorter. */
static uint32_t chain_nth(struct fat_super *sb, uint32_t first, uint32_t index)
{
	uint32_t c = first;

	for (uint32_t i = 0; i < index; i++) {
		if (!cluster_valid(sb, c))
			return 0;

		uint32_t next;
		if (fat_entry_read(sb, c, &next) != 0)
			return 0;
		if (is_eoc(sb, next))
			return 0;
		c = next;
	}

	return cluster_valid(sb, c) ? c : 0;
}

/* Extend a chain by one cluster, returning the new cluster or 0. */
static uint32_t chain_extend(struct fat_super *sb, uint32_t first)
{
	uint32_t fresh = cluster_alloc(sb);
	if (!fresh)
		return 0;

	if (first == 0)
		return fresh;   /* the caller links it as the first cluster */

	/* Find the tail. */
	uint32_t c = first;
	uint32_t guard = 0;
	for (;;) {
		if (++guard > sb->total_clusters) {
			kerr("fat: chain from %u does not terminate while "
			     "extending it", first);
			fat_entry_write(sb, fresh, 0);
			return 0;
		}

		uint32_t next;
		if (fat_entry_read(sb, c, &next) != 0) {
			fat_entry_write(sb, fresh, 0);
			return 0;
		}
		if (is_eoc(sb, next))
			break;
		c = next;
	}

	if (fat_entry_write(sb, c, fresh) != 0) {
		fat_entry_write(sb, fresh, 0);
		return 0;
	}

	return fresh;
}

/* ── Names ───────────────────────────────────────────────────────────── */

/* Convert an 8.3 on-disk name to a readable one. */
static void name_from_83(const char raw[11], char *out, size_t outsz)
{
	size_t n = 0;

	for (int i = 0; i < 8 && raw[i] != ' '; i++) {
		if (n + 1 < outsz)
			out[n++] = raw[i];
	}

	if (raw[8] != ' ') {
		if (n + 1 < outsz)
			out[n++] = '.';
		for (int i = 8; i < 11 && raw[i] != ' '; i++) {
			if (n + 1 < outsz)
				out[n++] = raw[i];
		}
	}

	out[n] = '\0';

	/* FAT stores names upper-case.  Presenting them lower-case is what
	   every other system does and is what makes a FAT disk pleasant to
	   use; the comparison is case-insensitive either way. */
	str_lower(out, n);
}

/*
 * Convert a name to 8.3 form, refusing anything that does not fit.
 *
 * Refusing is the point.  HavenDOS passed whole paths to its name_to_83()
 * and got "BOOT/GRU" -- a file with a separator in its name.  Silently
 * mangling a name produces a file the user did not ask for and cannot
 * find; -ENAMETOOLONG tells them why.
 */
static int name_to_83(const char *name, char out[11])
{
	memset(out, ' ', 11);

	if (!name || !name[0])
		return -HAVEN_EINVAL;

	/* "." and ".." are the two legitimate names that are not 8.3. */
	if (strcmp(name, ".") == 0) {
		out[0] = '.';
		return 0;
	}
	if (strcmp(name, "..") == 0) {
		out[0] = '.';
		out[1] = '.';
		return 0;
	}

	const char *dot = strrchr(name, '.');
	size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
	size_t ext_len  = dot ? strlen(dot + 1) : 0;

	if (base_len == 0 || base_len > 8 || ext_len > 3)
		return -HAVEN_ENAMETOOLONG;

	/* Characters FAT forbids.  A separator among them is what caught
	   HavenDOS out. */
	static const char *forbidden = "\"*+,/:;<=>?[\\]|";

	for (size_t i = 0; i < base_len; i++) {
		char c = name[i];
		if ((unsigned char)c < 0x20 || strchr(forbidden, c))
			return -HAVEN_EINVAL;
		out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
	}
	for (size_t i = 0; i < ext_len; i++) {
		char c = dot[1 + i];
		if ((unsigned char)c < 0x20 || strchr(forbidden, c))
			return -HAVEN_EINVAL;
		out[8 + i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
	}

	return 0;
}

/*
 * Extract the printable ASCII of a long-name entry into `out` at the
 * position its sequence number implies.
 *
 * The three name runs are read through a byte pointer with explicit
 * offsets rather than through the struct members.  Taking the address of
 * a member of a packed struct yields a pointer the compiler cannot
 * assume is aligned, and dereferencing it as a uint16_t* is undefined
 * behaviour that happens to work on x86 and faults on a stricter
 * architecture -- which matters, because ARM64 portability is a goal.
 * GCC's -Waddress-of-packed-member caught this.
 */
#define LFN_OFF_NAME1 1
#define LFN_OFF_NAME2 14
#define LFN_OFF_NAME3 28

static void lfn_extract(const struct fat_lfn *lfn, char *out, size_t outsz)
{
	unsigned seq = (lfn->sequence & FAT_LFN_SEQ);
	if (seq == 0)
		return;

	const uint8_t *raw = (const uint8_t *)lfn;

	static const size_t run_off[3] = {
		LFN_OFF_NAME1, LFN_OFF_NAME2, LFN_OFF_NAME3
	};
	static const size_t run_len[3] = { 5, 6, 2 };

	size_t base = (seq - 1) * 13;
	size_t k    = 0;

	for (int r = 0; r < 3; r++) {
		for (size_t i = 0; i < run_len[r]; i++, k++) {
			if (base + k >= outsz - 1)
				return;

			size_t   off = run_off[r] + i * 2;
			uint16_t wc  = (uint16_t)raw[off] |
			               ((uint16_t)raw[off + 1] << 8);

			if (wc == 0x0000 || wc == 0xFFFF)
				return;

			/* Non-ASCII is replaced rather than truncated: a file
			   whose name HavenOS cannot represent should still be
			   listed and openable by its short name. */
			out[base + k] = (wc < 0x80) ? (char)wc : '?';
		}
	}
}

/* ── Directory traversal ─────────────────────────────────────────────── */

struct dir_iter {
	struct fat_super *sb;
	uint32_t cluster;        /* 0 for a FAT12/16 fixed root */
	uint32_t sector_in_dir;
	uint32_t offset_in_sector;
	uint32_t fixed_root_sectors;
	bool     done;
};

static void dir_iter_init(struct dir_iter *it, struct fat_super *sb,
                          const struct fat_node *dir)
{
	memset(it, 0, sizeof(*it));
	it->sb = sb;

	if (dir->is_root && sb->variant != FAT32) {
		it->cluster            = 0;
		it->fixed_root_sectors = sb->root_dir_sectors;
	} else {
		it->cluster = dir->is_root ? sb->root_cluster
		                           : dir->first_cluster;
	}
}

/* Read the next raw 32-byte entry, reporting where it lives so a caller
   can rewrite it. */
static int dir_iter_next(struct dir_iter *it, struct fat_dirent *out,
                         uint32_t *out_sector, uint32_t *out_offset)
{
	struct fat_super *sb = it->sb;

	if (it->done)
		return -HAVEN_ENOENT;

	for (;;) {
		uint32_t abs_sector;

		if (it->fixed_root_sectors) {
			if (it->sector_in_dir >= it->fixed_root_sectors) {
				it->done = true;
				return -HAVEN_ENOENT;
			}
			abs_sector = sb->first_root_sector + it->sector_in_dir;
		} else {
			uint32_t cluster_index =
				it->sector_in_dir / sb->sectors_per_cluster;
			uint32_t sector_in_cluster =
				it->sector_in_dir % sb->sectors_per_cluster;

			uint32_t cluster = chain_nth(sb, it->cluster,
			                             cluster_index);
			if (!cluster) {
				it->done = true;
				return -HAVEN_ENOENT;
			}
			abs_sector = cluster_to_sector(sb, cluster) +
			             sector_in_cluster;
		}

		uint8_t *buf = sb->scratch;
		ssize_t_k got = block_read_bytes(sb->dev,
		                (uint64_t)abs_sector * sb->bytes_per_sector,
		                buf, sb->bytes_per_sector);
		if (got != (ssize_t_k)sb->bytes_per_sector)
			return got < 0 ? (int)got : -HAVEN_EIO;

		struct fat_dirent *entries = (struct fat_dirent *)buf;
		uint32_t per_sector = sb->bytes_per_sector /
		                      sizeof(struct fat_dirent);

		while (it->offset_in_sector < per_sector) {
			uint32_t idx = it->offset_in_sector++;
			struct fat_dirent *e = &entries[idx];

			if ((uint8_t)e->name[0] == DIRENT_END) {
				/* A zero first byte ends the directory: no
				   entry after it is in use, so scanning on
				   would read stale entries from a previously
				   larger directory. */
				it->done = true;
				return -HAVEN_ENOENT;
			}

			*out = *e;
			if (out_sector)
				*out_sector = abs_sector;
			if (out_offset)
				*out_offset = idx *
				              (uint32_t)sizeof(struct fat_dirent);
			return 0;
		}

		it->offset_in_sector = 0;
		it->sector_in_dir++;
	}
}

/*
 * Find an entry by name, assembling long names as it goes.
 *
 * Long-name entries precede their short entry in reverse sequence order,
 * so the name is accumulated and only compared when the short entry
 * arrives.
 */
static int dir_find(struct fat_super *sb, const struct fat_node *dir,
                    const char *want, struct fat_dirent *out,
                    uint32_t *out_sector, uint32_t *out_offset)
{
	struct dir_iter it;
	dir_iter_init(&it, sb, dir);

	char lfn_name[VFS_NAME_MAX + 1];
	memset(lfn_name, 0, sizeof(lfn_name));
	bool have_lfn = false;

	for (;;) {
		struct fat_dirent e;
		uint32_t sector, offset;

		int rc = dir_iter_next(&it, &e, &sector, &offset);
		if (rc != 0)
			return rc;

		if ((uint8_t)e.name[0] == DIRENT_FREE) {
			memset(lfn_name, 0, sizeof(lfn_name));
			have_lfn = false;
			continue;
		}

		if ((e.attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
			lfn_extract((const struct fat_lfn *)&e, lfn_name,
			            sizeof(lfn_name));
			have_lfn = true;
			continue;
		}

		/* The volume label is an entry but not a file. */
		if (e.attr & FAT_ATTR_VOLUME_ID) {
			memset(lfn_name, 0, sizeof(lfn_name));
			have_lfn = false;
			continue;
		}

		char short_name[16];
		name_from_83(e.name, short_name, sizeof(short_name));

		bool match = strcasecmp(short_name, want) == 0 ||
		             (have_lfn && strcasecmp(lfn_name, want) == 0);

		memset(lfn_name, 0, sizeof(lfn_name));
		have_lfn = false;

		if (match) {
			*out = e;
			if (out_sector)
				*out_sector = sector;
			if (out_offset)
				*out_offset = offset;
			return 0;
		}
	}
}

static uint32_t dirent_cluster(const struct fat_super *sb,
                               const struct fat_dirent *e)
{
	uint32_t c = e->cluster_low;
	if (sb->variant == FAT32)
		c |= (uint32_t)e->cluster_high << 16;
	return c;
}

/* ── vfs_ops ─────────────────────────────────────────────────────────── */

static const struct vfs_ops fat_ops;

static struct fat_node *node_alloc(struct fat_super *sb, const char *name,
                                   const struct fat_dirent *e,
                                   uint32_t sector, uint32_t offset)
{
	struct fat_node *n = kzalloc(sizeof(*n));
	if (!n)
		return NULL;

	strlcpy(n->vnode.name, name, sizeof(n->vnode.name));
	n->vnode.type = (e->attr & FAT_ATTR_DIRECTORY) ? VFS_NODE_DIR
	                                               : VFS_NODE_FILE;
	/* FAT has no permission bits.  Reporting a plausible fixed mode is
	   honest -- the read-only attribute is the only permission the
	   filesystem actually stores. */
	n->vnode.perm = (e->attr & FAT_ATTR_READ_ONLY)
	              ? ((e->attr & FAT_ATTR_DIRECTORY) ? 0555 : 0444)
	              : ((e->attr & FAT_ATTR_DIRECTORY) ? 0755 : 0644);
	n->vnode.size     = (e->attr & FAT_ATTR_DIRECTORY) ? 0 : e->size;
	n->vnode.refcount = 1;
	n->vnode.fs_data  = n;

	n->sb            = sb;
	n->first_cluster = dirent_cluster(sb, e);
	n->entry_sector  = sector;
	n->entry_offset  = offset;

	return n;
}

static int fat_lookup(struct vfs_node *dir, const char *name,
                      struct vfs_node **out)
{
	struct fat_node  *d  = dir->fs_data;
	struct fat_super *sb = d ? d->sb : NULL;
	if (!sb)
		return -HAVEN_EIO;
	if (dir->type != VFS_NODE_DIR)
		return -HAVEN_ENOTDIR;

	mutex_lock(&sb->lock);

	struct fat_dirent e;
	uint32_t sector, offset;
	int rc = dir_find(sb, d, name, &e, &sector, &offset);
	if (rc != 0) {
		mutex_unlock(&sb->lock);
		return rc;
	}

	struct fat_node *n = node_alloc(sb, name, &e, sector, offset);
	mutex_unlock(&sb->lock);

	if (!n)
		return -HAVEN_ENOMEM;

	n->vnode.mount = dir->mount;
	*out = &n->vnode;
	return 0;
}

static ssize_t_k fat_read(struct vfs_node *node, void *buf, size_t len,
                          haven_off_t offset)
{
	struct fat_node  *n  = node->fs_data;
	struct fat_super *sb = n ? n->sb : NULL;
	if (!sb)
		return -HAVEN_EIO;
	if (node->type == VFS_NODE_DIR)
		return -HAVEN_EISDIR;

	if (offset >= node->size)
		return 0;
	if (len > node->size - offset)
		len = (size_t)(node->size - offset);
	if (len == 0)
		return 0;

	mutex_lock(&sb->lock);

	uint8_t *out  = buf;
	size_t   done = 0;

	while (done < len) {
		uint64_t pos           = offset + done;
		uint32_t cluster_index = (uint32_t)(pos / sb->bytes_per_cluster);
		uint32_t in_cluster    = (uint32_t)(pos % sb->bytes_per_cluster);
		size_t   chunk         = sb->bytes_per_cluster - in_cluster;
		if (chunk > len - done)
			chunk = len - done;

		uint32_t cluster = chain_nth(sb, n->first_cluster,
		                             cluster_index);
		if (!cluster) {
			/* The chain ended before the size said it should.
			   Reporting what was read beats reporting a clean
			   success for data that is not there. */
			kwarn("fat: %s claims %llu bytes but its cluster chain "
			      "ends at offset %llu", node->name,
			      (unsigned long long)node->size,
			      (unsigned long long)pos);
			break;
		}

		uint64_t disk = (uint64_t)cluster_to_sector(sb, cluster) *
		                sb->bytes_per_sector + in_cluster;

		ssize_t_k got = block_read_bytes(sb->dev, disk, out + done,
		                                 chunk);
		if (got <= 0)
			break;
		done += (size_t)got;
	}

	mutex_unlock(&sb->lock);
	return done > 0 ? (ssize_t_k)done : -HAVEN_EIO;
}

/* Write the node's directory entry back, so a changed size or first
   cluster reaches the disk.  Without this a write appears to work until
   the volume is remounted. */
static int entry_flush(struct fat_super *sb, struct fat_node *n)
{
	if (n->is_root)
		return 0;

	uint64_t pos = (uint64_t)n->entry_sector * sb->bytes_per_sector +
	               n->entry_offset;

	struct fat_dirent e;
	ssize_t_k got = block_read_bytes(sb->dev, pos, &e, sizeof(e));
	if (got != (ssize_t_k)sizeof(e))
		return got < 0 ? (int)got : -HAVEN_EIO;

	e.size         = (uint32_t)n->vnode.size;
	e.cluster_low  = (uint16_t)(n->first_cluster & 0xFFFF);
	if (sb->variant == FAT32)
		e.cluster_high = (uint16_t)(n->first_cluster >> 16);

	ssize_t_k put = block_write_bytes(sb->dev, pos, &e, sizeof(e));
	return put == (ssize_t_k)sizeof(e) ? 0
	     : (put < 0 ? (int)put : -HAVEN_EIO);
}

static ssize_t_k fat_write(struct vfs_node *node, const void *buf, size_t len,
                           haven_off_t offset)
{
	struct fat_node  *n  = node->fs_data;
	struct fat_super *sb = n ? n->sb : NULL;
	if (!sb)
		return -HAVEN_EIO;
	if (sb->read_only)
		return -HAVEN_EROFS;
	if (node->type == VFS_NODE_DIR)
		return -HAVEN_EISDIR;
	if (len == 0)
		return 0;

	/* FAT stores the size as a 32-bit field, so a file cannot exceed
	   4 GiB minus one.  Refusing beyond that is better than wrapping
	   the size, which would make the file appear tiny. */
	if (offset + len > 0xFFFFFFFFULL)
		return -HAVEN_EFBIG;

	mutex_lock(&sb->lock);

	const uint8_t *in   = buf;
	size_t         done = 0;

	while (done < len) {
		uint64_t pos           = offset + done;
		uint32_t cluster_index = (uint32_t)(pos / sb->bytes_per_cluster);
		uint32_t in_cluster    = (uint32_t)(pos % sb->bytes_per_cluster);
		size_t   chunk         = sb->bytes_per_cluster - in_cluster;
		if (chunk > len - done)
			chunk = len - done;

		uint32_t cluster = chain_nth(sb, n->first_cluster,
		                             cluster_index);
		if (!cluster) {
			/* Grow the chain one cluster at a time until the
			   write's position is covered.  Allocating the whole
			   span up front would have to be undone on failure. */
			uint32_t have = 0;
			if (n->first_cluster)
				have = (uint32_t)(node->size +
				        sb->bytes_per_cluster - 1) /
				        sb->bytes_per_cluster;

			while (have <= cluster_index) {
				uint32_t fresh = chain_extend(sb,
				                              n->first_cluster);
				if (!fresh)
					goto out;
				if (!n->first_cluster)
					n->first_cluster = fresh;
				have++;
			}

			cluster = chain_nth(sb, n->first_cluster,
			                    cluster_index);
			if (!cluster)
				goto out;
		}

		uint64_t disk = (uint64_t)cluster_to_sector(sb, cluster) *
		                sb->bytes_per_sector + in_cluster;

		ssize_t_k put = block_write_bytes(sb->dev, disk, in + done,
		                                  chunk);
		if (put <= 0)
			goto out;

		done += (size_t)put;
		if (pos + (uint64_t)put > node->size)
			node->size = pos + (uint64_t)put;
	}

out:
	if (done > 0) {
		node->mtime_ns = time_ns();
		entry_flush(sb, n);
	}
	mutex_unlock(&sb->lock);

	if (done == 0)
		return -HAVEN_ENOSPC;
	return (ssize_t_k)done;
}

/* Find a free directory slot, extending the directory if needed. */
static int dir_alloc_slot(struct fat_super *sb, struct fat_node *dir,
                          uint32_t *out_sector, uint32_t *out_offset)
{
	struct dir_iter it;
	dir_iter_init(&it, sb, dir);

	/* Walk looking for a free or end marker.  dir_iter_next() stops at
	   the end marker, so a separate scan is needed to find a reusable
	   deleted slot. */
	uint32_t per_sector = sb->bytes_per_sector /
	                      (uint32_t)sizeof(struct fat_dirent);

	for (uint32_t s = 0; ; s++) {
		uint32_t abs_sector;

		if (it.fixed_root_sectors) {
			if (s >= it.fixed_root_sectors) {
				/* A FAT12/16 root directory is a fixed size
				   and cannot grow.  That is a real limit of
				   the format, not of this driver. */
				kerr("fat: the root directory of %s is full "
				     "(%u entries); FAT12/16 cannot grow it",
				     sb->dev->name, sb->root_entries);
				return -HAVEN_ENOSPC;
			}
			abs_sector = sb->first_root_sector + s;
		} else {
			uint32_t ci  = s / sb->sectors_per_cluster;
			uint32_t sic = s % sb->sectors_per_cluster;
			uint32_t base = dir->is_root ? sb->root_cluster
			                             : dir->first_cluster;

			uint32_t cluster = chain_nth(sb, base, ci);
			if (!cluster) {
				cluster = chain_extend(sb, base);
				if (!cluster)
					return -HAVEN_ENOSPC;
			}
			abs_sector = cluster_to_sector(sb, cluster) + sic;
		}

		uint8_t *buf = sb->scratch;
		ssize_t_k got = block_read_bytes(sb->dev,
		                (uint64_t)abs_sector * sb->bytes_per_sector,
		                buf, sb->bytes_per_sector);
		if (got != (ssize_t_k)sb->bytes_per_sector)
			return got < 0 ? (int)got : -HAVEN_EIO;

		struct fat_dirent *entries = (struct fat_dirent *)buf;
		for (uint32_t i = 0; i < per_sector; i++) {
			uint8_t first = (uint8_t)entries[i].name[0];
			if (first == DIRENT_END || first == DIRENT_FREE) {
				*out_sector = abs_sector;
				*out_offset = i *
				    (uint32_t)sizeof(struct fat_dirent);
				return 0;
			}
		}
	}
}

static int fat_create(struct vfs_node *dir, const char *name, uint32_t perm,
                      enum vfs_node_type type, struct vfs_node **out)
{
	struct fat_node  *d  = dir->fs_data;
	struct fat_super *sb = d ? d->sb : NULL;
	if (!sb)
		return -HAVEN_EIO;
	if (sb->read_only)
		return -HAVEN_EROFS;
	if (type != VFS_NODE_FILE && type != VFS_NODE_DIR)
		return -HAVEN_EPERM;

	char short_name[11];
	int rc = name_to_83(name, short_name);
	if (rc != 0) {
		/* Refused rather than mangled; see the comment on
		   name_to_83(). */
		kwarn("fat: cannot create '%s': FAT short names are 8.3 and "
		      "this driver does not write long names", name);
		return rc;
	}

	mutex_lock(&sb->lock);

	struct fat_dirent existing;
	if (dir_find(sb, d, name, &existing, NULL, NULL) == 0) {
		mutex_unlock(&sb->lock);
		return -HAVEN_EEXIST;
	}

	/* Initialised because dir_alloc_slot() only writes them on success,
	   and GCC cannot see that the error path does not use them. */
	uint32_t sector = 0, offset = 0;
	rc = dir_alloc_slot(sb, d, &sector, &offset);
	if (rc != 0) {
		mutex_unlock(&sb->lock);
		return rc;
	}

	uint32_t first_cluster = 0;
	if (type == VFS_NODE_DIR) {
		/* A directory needs a cluster immediately, for its "." and
		   ".." entries. */
		first_cluster = cluster_alloc(sb);
		if (!first_cluster) {
			mutex_unlock(&sb->lock);
			return -HAVEN_ENOSPC;
		}
	}

	struct fat_dirent e;
	memset(&e, 0, sizeof(e));
	memcpy(e.name, short_name, 11);
	e.attr         = (type == VFS_NODE_DIR) ? FAT_ATTR_DIRECTORY
	                                        : FAT_ATTR_ARCHIVE;
	e.cluster_low  = (uint16_t)(first_cluster & 0xFFFF);
	if (sb->variant == FAT32)
		e.cluster_high = (uint16_t)(first_cluster >> 16);
	e.size = 0;

	uint64_t pos = (uint64_t)sector * sb->bytes_per_sector + offset;
	ssize_t_k put = block_write_bytes(sb->dev, pos, &e, sizeof(e));
	if (put != (ssize_t_k)sizeof(e)) {
		if (first_cluster)
			fat_entry_write(sb, first_cluster, 0);
		mutex_unlock(&sb->lock);
		return put < 0 ? (int)put : -HAVEN_EIO;
	}

	if (type == VFS_NODE_DIR) {
		/* "." and ".." are real entries on FAT, and a directory
		   without them is one other systems will report as damaged.
		   ".." of a directory in the root points at cluster 0 by
		   convention, not at the root's own cluster. */
		struct fat_dirent dots[2];
		memset(dots, 0, sizeof(dots));

		memset(dots[0].name, ' ', 11);
		dots[0].name[0]     = '.';
		dots[0].attr        = FAT_ATTR_DIRECTORY;
		dots[0].cluster_low = (uint16_t)(first_cluster & 0xFFFF);
		if (sb->variant == FAT32)
			dots[0].cluster_high = (uint16_t)(first_cluster >> 16);

		uint32_t parent = d->is_root ? 0 : d->first_cluster;
		memset(dots[1].name, ' ', 11);
		dots[1].name[0]     = '.';
		dots[1].name[1]     = '.';
		dots[1].attr        = FAT_ATTR_DIRECTORY;
		dots[1].cluster_low = (uint16_t)(parent & 0xFFFF);
		if (sb->variant == FAT32)
			dots[1].cluster_high = (uint16_t)(parent >> 16);

		uint64_t dpos = (uint64_t)cluster_to_sector(sb, first_cluster) *
		                sb->bytes_per_sector;
		block_write_bytes(sb->dev, dpos, dots, sizeof(dots));
	}

	char readable[16];
	name_from_83(e.name, readable, sizeof(readable));
	struct fat_node *n = node_alloc(sb, readable, &e, sector, offset);
	mutex_unlock(&sb->lock);

	if (!n)
		return -HAVEN_ENOMEM;

	n->vnode.mount = dir->mount;
	n->vnode.perm  = perm ? (perm & 07777) : n->vnode.perm;

	if (out)
		*out = &n->vnode;
	else
		vfs_node_put(&n->vnode);

	return 0;
}

static int fat_unlink(struct vfs_node *dir, const char *name)
{
	struct fat_node  *d  = dir->fs_data;
	struct fat_super *sb = d ? d->sb : NULL;
	if (!sb)
		return -HAVEN_EIO;
	if (sb->read_only)
		return -HAVEN_EROFS;

	mutex_lock(&sb->lock);

	struct fat_dirent e;
	uint32_t sector, offset;
	int rc = dir_find(sb, d, name, &e, &sector, &offset);
	if (rc != 0) {
		mutex_unlock(&sb->lock);
		return rc;
	}

	if (e.attr & FAT_ATTR_DIRECTORY) {
		/* A directory must be empty except for "." and "..".
		   Removing a populated one orphans every cluster beneath it,
		   which is a leak the filesystem has no way to recover
		   from. */
		struct fat_node probe;
		memset(&probe, 0, sizeof(probe));
		probe.sb            = sb;
		probe.first_cluster = dirent_cluster(sb, &e);
		probe.vnode.type    = VFS_NODE_DIR;

		struct dir_iter it;
		dir_iter_init(&it, sb, &probe);

		for (;;) {
			struct fat_dirent child;
			if (dir_iter_next(&it, &child, NULL, NULL) != 0)
				break;
			if ((uint8_t)child.name[0] == DIRENT_FREE)
				continue;
			if ((child.attr & FAT_ATTR_LFN) == FAT_ATTR_LFN)
				continue;

			char cname[16];
			name_from_83(child.name, cname, sizeof(cname));
			if (strcmp(cname, ".") == 0 || strcmp(cname, "..") == 0)
				continue;

			mutex_unlock(&sb->lock);
			return -HAVEN_ENOTEMPTY;
		}
	}

	uint32_t first = dirent_cluster(sb, &e);

	/* Mark the entry deleted, then free the chain.  This order matters:
	   if the power fails between them the clusters are leaked, which
	   chkdsk can reclaim.  The other order would leave a directory
	   entry pointing at freed clusters, which chkdsk cannot
	   distinguish from a valid file and may reallocate under. */
	uint8_t marker = DIRENT_FREE;
	uint64_t pos = (uint64_t)sector * sb->bytes_per_sector + offset;
	if (block_write_bytes(sb->dev, pos, &marker, 1) != 1) {
		mutex_unlock(&sb->lock);
		return -HAVEN_EIO;
	}

	if (cluster_valid(sb, first))
		chain_free(sb, first);

	mutex_unlock(&sb->lock);
	return 0;
}

static int fat_truncate(struct vfs_node *node, uint64_t length)
{
	struct fat_node  *n  = node->fs_data;
	struct fat_super *sb = n ? n->sb : NULL;
	if (!sb)
		return -HAVEN_EIO;
	if (sb->read_only)
		return -HAVEN_EROFS;
	if (length > 0xFFFFFFFFULL)
		return -HAVEN_EFBIG;

	mutex_lock(&sb->lock);

	if (length == 0) {
		if (cluster_valid(sb, n->first_cluster))
			chain_free(sb, n->first_cluster);
		n->first_cluster = 0;
		node->size       = 0;
	} else if (length < node->size) {
		uint32_t keep = (uint32_t)((length + sb->bytes_per_cluster - 1) /
		                           sb->bytes_per_cluster);
		uint32_t last = chain_nth(sb, n->first_cluster, keep - 1);
		if (last) {
			uint32_t next;
			if (fat_entry_read(sb, last, &next) == 0 &&
			    !is_eoc(sb, next)) {
				fat_entry_write(sb, last, sb->eoc_marker);
				chain_free(sb, next);
			}
		}
		node->size = length;
	} else {
		/* Extending leaves the new region reading as whatever the
		   clusters hold, so clusters are allocated (and zeroed by
		   cluster_alloc) rather than the size simply being raised. */
		uint32_t need = (uint32_t)((length + sb->bytes_per_cluster - 1) /
		                           sb->bytes_per_cluster);
		uint32_t have = (uint32_t)((node->size +
		                 sb->bytes_per_cluster - 1) /
		                 sb->bytes_per_cluster);
		while (have < need) {
			uint32_t fresh = chain_extend(sb, n->first_cluster);
			if (!fresh) {
				mutex_unlock(&sb->lock);
				return -HAVEN_ENOSPC;
			}
			if (!n->first_cluster)
				n->first_cluster = fresh;
			have++;
		}
		node->size = length;
	}

	entry_flush(sb, n);
	mutex_unlock(&sb->lock);
	return 0;
}

static int fat_readdir(struct vfs_node *dir, uint32_t index,
                       struct vfs_dirent *out)
{
	struct fat_node  *d  = dir->fs_data;
	struct fat_super *sb = d ? d->sb : NULL;
	if (!sb)
		return -HAVEN_EIO;
	if (dir->type != VFS_NODE_DIR)
		return -HAVEN_ENOTDIR;

	mutex_lock(&sb->lock);

	struct dir_iter it;
	dir_iter_init(&it, sb, d);

	char lfn_name[VFS_NAME_MAX + 1];
	memset(lfn_name, 0, sizeof(lfn_name));
	bool have_lfn = false;
	uint32_t seen = 0;

	for (;;) {
		struct fat_dirent e;
		int rc = dir_iter_next(&it, &e, NULL, NULL);
		if (rc != 0) {
			mutex_unlock(&sb->lock);
			return rc;
		}

		if ((uint8_t)e.name[0] == DIRENT_FREE) {
			memset(lfn_name, 0, sizeof(lfn_name));
			have_lfn = false;
			continue;
		}
		if ((e.attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
			lfn_extract((const struct fat_lfn *)&e, lfn_name,
			            sizeof(lfn_name));
			have_lfn = true;
			continue;
		}
		if (e.attr & FAT_ATTR_VOLUME_ID) {
			memset(lfn_name, 0, sizeof(lfn_name));
			have_lfn = false;
			continue;
		}

		char short_name[16];
		name_from_83(e.name, short_name, sizeof(short_name));

		/* "." and ".." are on-disk entries but the VFS supplies them
		   itself, so listing them here would duplicate them. */
		if (strcmp(short_name, ".") == 0 ||
		    strcmp(short_name, "..") == 0) {
			memset(lfn_name, 0, sizeof(lfn_name));
			have_lfn = false;
			continue;
		}

		if (seen++ != index) {
			memset(lfn_name, 0, sizeof(lfn_name));
			have_lfn = false;
			continue;
		}

		strlcpy(out->name, have_lfn && lfn_name[0] ? lfn_name
		                                           : short_name,
		        sizeof(out->name));
		out->size  = e.size;
		out->type  = (e.attr & FAT_ATTR_DIRECTORY) ? VFS_NODE_DIR
		                                           : VFS_NODE_FILE;
		out->index = index;

		mutex_unlock(&sb->lock);
		return 0;
	}
}

static int fat_stat(struct vfs_node *node, struct vfs_stat *out)
{
	memset(out, 0, sizeof(*out));
	out->size     = node->size;
	out->type     = node->type;
	out->perm     = node->perm;
	out->mtime_ns = node->mtime_ns;
	out->nlink    = 1;
	return 0;
}

static int fat_chmod(struct vfs_node *node, uint32_t perm)
{
	/* FAT stores one bit of permission: the read-only attribute.
	   Accepting a full mode and storing only what the format can hold
	   would make a later stat() report something that was never
	   written, so the limitation is reported instead. */
	(void)node;
	(void)perm;
	return -HAVEN_ENOSYS;
}

static int fat_sync(struct vfs_mount *mount)
{
	struct fat_super *sb = mount ? mount->fs_data : NULL;
	if (!sb)
		return -HAVEN_EINVAL;
	return block_flush(sb->dev);
}

static void fat_release(struct vfs_node *node)
{
	struct fat_node *n = node->fs_data;
	if (n && !n->is_root)
		kfree(n);
}

static void fat_unmount_fs(struct vfs_mount *mount)
{
	struct fat_super *sb = mount ? mount->fs_data : NULL;
	if (!sb)
		return;

	block_flush(sb->dev);
	if (sb->scratch)
		vmm_free((vaddr_t)sb->scratch, sb->bytes_per_cluster);
	kfree(sb);
}

static const struct vfs_ops fat_ops = {
	.name     = "fat",
	.lookup   = fat_lookup,
	.create   = fat_create,
	.unlink   = fat_unlink,
	.read     = fat_read,
	.write    = fat_write,
	.truncate = fat_truncate,
	.readdir  = fat_readdir,
	.stat     = fat_stat,
	.chmod    = fat_chmod,
	.sync     = fat_sync,
	.release  = fat_release,
	.unmount  = fat_unmount_fs,
};

/* ── Mount ───────────────────────────────────────────────────────────── */

/*
 * Work out the geometry and the variant from the boot parameter block.
 *
 * The variant is determined by the CLUSTER COUNT, not by the fs_type
 * string in the BPB.  That string is advisory and frequently wrong --
 * formatters write "FAT16   " on FAT12 volumes and vice versa -- and
 * trusting it means reading the FAT with the wrong entry width, which
 * produces a chain of plausible-looking garbage.  The thresholds (4085
 * and 65525) are from Microsoft's own specification.
 */
static int fat_parse_bpb(struct fat_super *sb, const struct fat_bpb *bpb)
{
	sb->bytes_per_sector    = bpb->bytes_per_sector;
	sb->sectors_per_cluster = bpb->sectors_per_cluster;
	sb->num_fats            = bpb->num_fats;

	if (sb->bytes_per_sector != 512 && sb->bytes_per_sector != 1024 &&
	    sb->bytes_per_sector != 2048 && sb->bytes_per_sector != 4096)
		return -HAVEN_ENODEV;
	if (sb->sectors_per_cluster == 0 ||
	    (sb->sectors_per_cluster & (sb->sectors_per_cluster - 1)))
		return -HAVEN_ENODEV;
	if (sb->num_fats == 0 || sb->num_fats > 4)
		return -HAVEN_ENODEV;
	if (bpb->reserved_sectors == 0)
		return -HAVEN_ENODEV;

	sb->bytes_per_cluster = sb->bytes_per_sector * sb->sectors_per_cluster;

	sb->fat_sectors = bpb->fat_size_16 ? bpb->fat_size_16
	                                   : bpb->fat_size_32;
	if (sb->fat_sectors == 0)
		return -HAVEN_ENODEV;

	uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16
	                                               : bpb->total_sectors_32;
	if (total_sectors == 0)
		return -HAVEN_ENODEV;

	sb->first_fat_sector = bpb->reserved_sectors;
	sb->root_entries     = bpb->root_entries;
	sb->root_dir_sectors = ((uint32_t)bpb->root_entries * 32 +
	                        sb->bytes_per_sector - 1) /
	                       sb->bytes_per_sector;
	sb->first_root_sector = sb->first_fat_sector +
	                        sb->num_fats * sb->fat_sectors;
	sb->first_data_sector = sb->first_root_sector + sb->root_dir_sectors;
	sb->root_cluster      = bpb->root_cluster;

	if (sb->first_data_sector >= total_sectors)
		return -HAVEN_ENODEV;

	uint32_t data_sectors = total_sectors - sb->first_data_sector;
	sb->total_clusters = data_sectors / sb->sectors_per_cluster;

	if (sb->total_clusters < 4085) {
		sb->variant    = FAT12;
		sb->eoc_marker = 0x0FFF;
	} else if (sb->total_clusters < 65525) {
		sb->variant    = FAT16;
		sb->eoc_marker = 0xFFFF;
	} else {
		sb->variant    = FAT32;
		sb->eoc_marker = 0x0FFFFFFF;
		/* A FAT32 volume has no fixed root directory; its root is a
		   normal cluster chain.  A zero root_cluster would make
		   every lookup read cluster 0, which is reserved. */
		if (!cluster_valid(sb, sb->root_cluster))
			return -HAVEN_ENODEV;
	}

	return 0;
}

enum fat_variant fat_probe(struct block_device *dev)
{
	if (!dev)
		return FAT_UNKNOWN;

	uint8_t *sector = kmalloc(512);
	if (!sector)
		return FAT_UNKNOWN;

	ssize_t_k got = block_read_bytes(dev, 0, sector, 512);
	if (got != 512) {
		kfree(sector);
		return FAT_UNKNOWN;
	}

	/* The boot signature.  Without it this is not a FAT boot sector and
	   the BPB fields are whatever happens to be there. */
	if (sector[510] != 0x55 || sector[511] != 0xAA) {
		kfree(sector);
		return FAT_UNKNOWN;
	}

	struct fat_super probe;
	memset(&probe, 0, sizeof(probe));
	probe.dev = dev;

	int rc = fat_parse_bpb(&probe, (const struct fat_bpb *)sector);
	kfree(sector);

	return rc == 0 ? probe.variant : FAT_UNKNOWN;
}

int fat_mount(struct block_device *dev, const char *path, bool read_only)
{
	if (!dev || !path)
		return -HAVEN_EINVAL;

	uint8_t *sector = kmalloc(512);
	if (!sector)
		return -HAVEN_ENOMEM;

	ssize_t_k got = block_read_bytes(dev, 0, sector, 512);
	if (got != 512) {
		kfree(sector);
		return got < 0 ? (int)got : -HAVEN_EIO;
	}

	if (sector[510] != 0x55 || sector[511] != 0xAA) {
		kfree(sector);
		return -HAVEN_ENODEV;
	}

	struct fat_super *sb = kzalloc(sizeof(*sb));
	if (!sb) {
		kfree(sector);
		return -HAVEN_ENOMEM;
	}

	sb->dev       = dev;
	sb->read_only = read_only || dev->read_only;

	int rc = fat_parse_bpb(sb, (const struct fat_bpb *)sector);
	if (rc != 0) {
		kfree(sector);
		kfree(sb);
		return rc;
	}

	/* The volume label, for the mount message.  Purely informational,
	   but it is how a user tells two disks apart. */
	char label[12];
	const struct fat_bpb *bpb = (const struct fat_bpb *)sector;
	memcpy(label, bpb->volume_label, 11);
	label[11] = '\0';
	for (int i = 10; i >= 0 && label[i] == ' '; i--)
		label[i] = '\0';

	kfree(sector);

	mutex_init(&sb->lock, "fat");

	sb->scratch = (uint8_t *)vmm_alloc(sb->bytes_per_cluster,
	                                   VM_PROT_READ | VM_PROT_WRITE);
	if (!sb->scratch) {
		kfree(sb);
		return -HAVEN_ENOMEM;
	}

	/* The root node.  On FAT12/16 it has no directory entry of its own,
	   which is why struct fat_node carries an is_root flag rather than
	   relying on entry_sector being zero. */
	struct fat_node *root = kzalloc(sizeof(*root));
	if (!root) {
		vmm_free((vaddr_t)sb->scratch, sb->bytes_per_cluster);
		kfree(sb);
		return -HAVEN_ENOMEM;
	}

	strlcpy(root->vnode.name, "/", sizeof(root->vnode.name));
	root->vnode.type     = VFS_NODE_DIR;
	root->vnode.perm     = sb->read_only ? 0555 : 0755;
	root->vnode.refcount = 1;
	root->vnode.fs_data  = root;
	root->sb             = sb;
	root->is_root        = true;
	root->first_cluster  = (sb->variant == FAT32) ? sb->root_cluster : 0;

	sb->root = root->vnode;

	rc = vfs_mount(path, &fat_ops, sb, &root->vnode);
	if (rc != 0) {
		kfree(root);
		vmm_free((vaddr_t)sb->scratch, sb->bytes_per_cluster);
		kfree(sb);
		return rc;
	}

	uint64_t mib = ((uint64_t)sb->total_clusters *
	                sb->bytes_per_cluster) / (1024 * 1024);

	kinfo("fat: %s on %s at %s: %llu clusters of %u bytes (%llu MiB)%s%s%s",
	      fat_variant_name(sb->variant), dev->name, path,
	      (unsigned long long)sb->total_clusters, sb->bytes_per_cluster,
	      (unsigned long long)mib,
	      label[0] ? ", label '" : "", label[0] ? label : "",
	      label[0] ? "'" : "");

	if (sb->read_only)
		kinfo("fat: mounted read-only");
	else
		kinfo("fat: writable, 8.3 short names only (long names are "
		      "read but not written)");

	return 0;
}
