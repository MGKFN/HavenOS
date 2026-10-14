/* SPDX-License-Identifier: MIT
 *
 * devfs.h — the device filesystem.
 *
 * A driver that wants to be reachable as a file fills in a
 * struct devfs_driver and calls devfs_register().  The VFS then routes
 * open/read/write/ioctl on /dev/<name> to it, so the syscall layer needs
 * one implementation of read and write rather than one per device.
 */
#ifndef _HAVEN_DEVFS_H
#define _HAVEN_DEVFS_H

#include <haven/types.h>
#include <haven/vfs.h>

struct devfs_driver {
	const char *name;

	/* A NULL read or write means the device does not support that
	   direction, and the VFS returns -EBADF.  Leaving them NULL is the
	   way to express a write-only or read-only device; returning 0 from
	   a read would instead mean end of file. */
	ssize_t_k (*read)(void *data, void *buf, size_t len,
	                  haven_off_t offset);
	ssize_t_k (*write)(void *data, const void *buf, size_t len,
	                   haven_off_t offset);
	int       (*ioctl)(void *data, uint32_t cmd, void *arg);
};

int      devfs_mount_at(const char *path);
int      devfs_register(const char *name, const struct devfs_driver *driver,
                        void *driver_data, enum vfs_node_type type,
                        uint32_t major, uint32_t minor, uint32_t perm);
int      devfs_unregister(const char *name);
uint32_t devfs_device_count(void);

/* Device major numbers.  Fixed so a device node's identity is stable
   across boots, which matters the moment anything records one. */
#define DEV_MAJOR_CONSOLE 1
#define DEV_MAJOR_NULL    2
#define DEV_MAJOR_ZERO    3
#define DEV_MAJOR_RANDOM  4
#define DEV_MAJOR_SERIAL  5
#define DEV_MAJOR_KBD     6
#define DEV_MAJOR_BLOCK   8
#define DEV_MAJOR_RTC     10

#endif /* _HAVEN_DEVFS_H */
