/* SPDX-License-Identifier: MIT
 *
 * devfs.c — devices as files.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had no device filesystem.  Its shell talked to the keyboard by
 * calling keyboard_getchar() and to the screen by calling vga_puts(), so
 * a program could not be redirected, piped, or run anywhere other than
 * the console it was written against.
 *
 * With /dev, a device is a path.  `cat /dev/console` and
 * `shell < /dev/serial` become expressible, and more importantly the
 * syscall layer needs exactly one implementation of read and write rather
 * than one per device.  That is why the console is reached through a file
 * here even though the kernel can also call klog() directly.
 */
#include <haven/vfs.h>
#include <haven/mm.h>
#include <haven/mutex.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/time.h>
#include <haven/devfs.h>

#define DEVFS_MAX_DEVICES 32

struct devfs_entry {
	struct vfs_node            vnode;
	const struct devfs_driver *driver;
	void                      *driver_data;
	bool                       in_use;
};

struct devfs_super {
	struct vfs_node    root;
	struct devfs_entry devices[DEVFS_MAX_DEVICES];
	struct mutex       lock;
	uint32_t           count;
};

static struct devfs_super *the_devfs;

static int devfs_lookup(struct vfs_node *dir, const char *name,
                        struct vfs_node **out)
{
	struct devfs_super *sb = dir->mount ? dir->mount->fs_data : NULL;
	if (!sb)
		return -HAVEN_EIO;

	/* Only the root is a directory here: /dev has no subdirectories, so
	   a lookup inside anything else is ENOTDIR rather than ENOENT. */
	if (dir != &sb->root)
		return -HAVEN_ENOTDIR;

	mutex_lock(&sb->lock);
	for (size_t i = 0; i < DEVFS_MAX_DEVICES; i++) {
		if (!sb->devices[i].in_use)
			continue;
		if (strcmp(sb->devices[i].vnode.name, name) != 0)
			continue;

		vfs_node_get(&sb->devices[i].vnode);
		mutex_unlock(&sb->lock);
		*out = &sb->devices[i].vnode;
		return 0;
	}
	mutex_unlock(&sb->lock);
	return -HAVEN_ENOENT;
}

static ssize_t_k devfs_read(struct vfs_node *node, void *buf, size_t len,
                            haven_off_t offset)
{
	struct devfs_entry *e = node->fs_data;
	if (!e || !e->driver)
		return -HAVEN_ENXIO;
	if (!e->driver->read) {
		/* A write-only device.  -EBADF rather than 0: a read of 0
		   bytes means end of file, which would make a caller stop
		   rather than report the mistake. */
		return -HAVEN_EBADF;
	}
	return e->driver->read(e->driver_data, buf, len, offset);
}

static ssize_t_k devfs_write(struct vfs_node *node, const void *buf,
                             size_t len, haven_off_t offset)
{
	struct devfs_entry *e = node->fs_data;
	if (!e || !e->driver)
		return -HAVEN_ENXIO;
	if (!e->driver->write)
		return -HAVEN_EBADF;
	return e->driver->write(e->driver_data, buf, len, offset);
}

static int devfs_readdir(struct vfs_node *dir, uint32_t index,
                         struct vfs_dirent *out)
{
	struct devfs_super *sb = dir->mount ? dir->mount->fs_data : NULL;
	if (!sb)
		return -HAVEN_EIO;
	if (dir != &sb->root)
		return -HAVEN_ENOTDIR;

	mutex_lock(&sb->lock);

	uint32_t seen = 0;
	for (size_t i = 0; i < DEVFS_MAX_DEVICES; i++) {
		if (!sb->devices[i].in_use)
			continue;
		if (seen++ != index)
			continue;

		strlcpy(out->name, sb->devices[i].vnode.name,
		        sizeof(out->name));
		out->size  = 0;
		out->type  = sb->devices[i].vnode.type;
		out->index = index;

		mutex_unlock(&sb->lock);
		return 0;
	}

	mutex_unlock(&sb->lock);
	return -HAVEN_ENOENT;
}

static int devfs_stat(struct vfs_node *node, struct vfs_stat *out)
{
	memset(out, 0, sizeof(*out));
	out->type      = node->type;
	out->perm      = node->perm;
	out->uid       = node->uid;
	out->gid       = node->gid;
	out->nlink     = 1;
	out->dev_major = node->dev_major;
	out->dev_minor = node->dev_minor;
	return 0;
}

static int devfs_ioctl(struct vfs_node *node, uint32_t cmd, void *arg)
{
	struct devfs_entry *e = node->fs_data;
	if (!e || !e->driver)
		return -HAVEN_ENXIO;
	if (!e->driver->ioctl)
		return -HAVEN_ENOSYS;
	return e->driver->ioctl(e->driver_data, cmd, arg);
}

static int devfs_create(struct vfs_node *dir, const char *name, uint32_t perm,
                        enum vfs_node_type type, struct vfs_node **out)
{
	(void)dir; (void)name; (void)perm; (void)type; (void)out;
	/* Devices are registered by drivers, not created by users.  -EPERM
	   rather than -ENOSYS, because the operation exists and is refused
	   rather than being unimplemented. */
	return -HAVEN_EPERM;
}

static int devfs_sync(struct vfs_mount *mount)
{
	(void)mount;
	return 0;
}

static const struct vfs_ops devfs_ops = {
	.name    = "devfs",
	.lookup  = devfs_lookup,
	.create  = devfs_create,
	.read    = devfs_read,
	.write   = devfs_write,
	.readdir = devfs_readdir,
	.stat    = devfs_stat,
	.ioctl   = devfs_ioctl,
	.sync    = devfs_sync,
};

int devfs_register(const char *name, const struct devfs_driver *driver,
                   void *driver_data, enum vfs_node_type type,
                   uint32_t major, uint32_t minor, uint32_t perm)
{
	if (!the_devfs)
		return -HAVEN_ENODEV;
	if (!name || !name[0] || !driver)
		return -HAVEN_EINVAL;

	mutex_lock(&the_devfs->lock);

	for (size_t i = 0; i < DEVFS_MAX_DEVICES; i++) {
		if (the_devfs->devices[i].in_use &&
		    strcmp(the_devfs->devices[i].vnode.name, name) == 0) {
			mutex_unlock(&the_devfs->lock);
			kerr("devfs: /dev/%s is already registered", name);
			return -HAVEN_EEXIST;
		}
	}

	struct devfs_entry *e = NULL;
	for (size_t i = 0; i < DEVFS_MAX_DEVICES; i++) {
		if (!the_devfs->devices[i].in_use) {
			e = &the_devfs->devices[i];
			break;
		}
	}
	if (!e) {
		mutex_unlock(&the_devfs->lock);
		kerr("devfs: device table full (%d entries)",
		     DEVFS_MAX_DEVICES);
		return -HAVEN_ENOSPC;
	}

	memset(e, 0, sizeof(*e));
	strlcpy(e->vnode.name, name, sizeof(e->vnode.name));
	e->vnode.type      = type;
	e->vnode.perm      = perm ? perm : 0666;
	e->vnode.refcount  = 1;
	e->vnode.mtime_ns  = time_ns();
	e->vnode.dev_major = major;
	e->vnode.dev_minor = minor;
	e->vnode.fs_data   = e;
	e->vnode.mount     = the_devfs->root.mount;
	e->driver          = driver;
	e->driver_data     = driver_data;
	e->in_use          = true;

	the_devfs->count++;
	mutex_unlock(&the_devfs->lock);

	kinfo("devfs: /dev/%s (%s, %u:%u)", name,
	      type == VFS_NODE_BLOCKDEV ? "block" : "character",
	      major, minor);
	return 0;
}

int devfs_unregister(const char *name)
{
	if (!the_devfs || !name)
		return -HAVEN_EINVAL;

	mutex_lock(&the_devfs->lock);
	for (size_t i = 0; i < DEVFS_MAX_DEVICES; i++) {
		if (!the_devfs->devices[i].in_use)
			continue;
		if (strcmp(the_devfs->devices[i].vnode.name, name) != 0)
			continue;

		the_devfs->devices[i].in_use = false;
		if (the_devfs->count)
			the_devfs->count--;
		mutex_unlock(&the_devfs->lock);
		return 0;
	}
	mutex_unlock(&the_devfs->lock);
	return -HAVEN_ENOENT;
}

int devfs_mount_at(const char *path)
{
	if (the_devfs)
		return -HAVEN_EBUSY;

	struct devfs_super *sb = kzalloc(sizeof(*sb));
	if (!sb)
		return -HAVEN_ENOMEM;

	mutex_init(&sb->lock, "devfs");

	strlcpy(sb->root.name, "dev", sizeof(sb->root.name));
	sb->root.type     = VFS_NODE_DIR;
	sb->root.perm     = 0755;
	sb->root.refcount = 1;
	sb->root.fs_data  = sb;

	int rc = vfs_mount(path, &devfs_ops, sb, &sb->root);
	if (rc != 0) {
		kfree(sb);
		return rc;
	}

	the_devfs = sb;
	return 0;
}

uint32_t devfs_device_count(void)
{
	return the_devfs ? the_devfs->count : 0;
}
