/* SPDX-License-Identifier: MIT
 *
 * fat.h — the FAT filesystem.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's persistent filesystem was FAT16, with FAT12 as a fallback,
 * in fs/fat16.c (1150 lines) and fs/fat12.c (534 lines) -- two separate
 * implementations of the same filesystem, differing mainly in how a FAT
 * entry is read.  Its installer wrote a FAT16 partition and its update
 * mechanism read ISO images from one.  That capability is kept, because
 * FAT is the only filesystem both HavenOS and every other system can
 * write, which makes it the practical way to move files onto a HavenOS
 * disk.
 *
 * ONE IMPLEMENTATION, THREE VARIANTS
 * FAT12, FAT16 and FAT32 differ in exactly three things: the width of a
 * FAT entry, where the root directory lives, and the end-of-chain
 * marker.  Everything else -- the boot parameter block, directory
 * entries, cluster chains -- is identical.  So this is one implementation
 * with a variant field, not three files.  HavenDOS's two files meant the
 * path-handling bug its VFS note describes had to be fixed twice.
 *
 * WHAT IS AND IS NOT SUPPORTED
 *   read  FAT12, FAT16, FAT32, with VFAT long names
 *   write FAT12, FAT16, FAT32, with 8.3 short names only
 *
 * Writing long names means generating and maintaining the long-name
 * directory entries and their checksums, and getting that subtly wrong
 * corrupts a filesystem other systems will read.  Reading them costs
 * nothing and makes a disk written elsewhere usable, so the asymmetry is
 * deliberate rather than an omission.  A write with a name that does not
 * fit 8.3 is refused, not silently mangled.
 */
#ifndef _HAVEN_FAT_H
#define _HAVEN_FAT_H

#include <haven/types.h>

struct block_device;

enum fat_variant {
	FAT_UNKNOWN = 0,
	FAT12,
	FAT16,
	FAT32,
};

/* Probe `dev` for a FAT filesystem and mount it at `path`.
   Returns 0, or -HAVEN_E* -- in particular -HAVEN_ENODEV when the device
   holds no recognisable FAT, which is information rather than a
   failure. */
int fat_mount(struct block_device *dev, const char *path, bool read_only);

/* Identify the variant on `dev` without mounting. */
enum fat_variant fat_probe(struct block_device *dev);

const char *fat_variant_name(enum fat_variant v);

#endif /* _HAVEN_FAT_H */
