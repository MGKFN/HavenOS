/* SPDX-License-Identifier: MIT
 *
 * ramfs.h — the in-memory filesystem.
 */
#ifndef _HAVEN_RAMFS_H
#define _HAVEN_RAMFS_H

#include <haven/types.h>

struct vfs_mount;

/* Create a fresh RAM filesystem and mount it at `path`. */
int      ramfs_mount_at(const char *path);
uint64_t ramfs_bytes_used(struct vfs_mount *mount);

#endif /* _HAVEN_RAMFS_H */
