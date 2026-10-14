/* SPDX-License-Identifier: MIT
 *
 * ramfs.c — the in-memory filesystem.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's ramfs was a fixed array of 64 slots, each with a 48-byte
 * name and a 2048-byte inline data buffer:
 *
 *     #define MAX_FILES 64
 *     #define MAX_NAME  48
 *     #define MAX_DATA  2048
 *     static ramfs_file_t files[MAX_FILES];
 *
 * Four consequences, all of them silent:
 *
 *   - 64 files, full stop.  ramfs_alloc() returned -1 and ramfs_write()
 *     returned -1, which most callers ignored.
 *   - a file larger than 2 KiB was TRUNCATED, not rejected:
 *     `if (size > MAX_DATA) size = MAX_DATA;` and then it returned the
 *     truncated size as if it had written everything.
 *   - 128 KiB of kernel BSS was reserved whether any file existed or not.
 *   - directories were faked by putting a slash in the name, so
 *     ramfs_list_dir() matched on a "dir/" string prefix and explicitly
 *     refused to recurse: "Don't recurse into deeper subdirs -- only one
 *     level."
 *
 * HavenOS allocates nodes and data on demand, grows a file by adding
 * pages, and has real directories with real parent/child links, so depth
 * is unlimited.  A short write returns the count actually written and a
 * failure returns a negative errno, so a caller can tell the difference.
 */
#include <haven/vfs.h>
#include <haven/mm.h>
#include <haven/mutex.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/time.h>
#include <haven/panic.h>
#include <haven/ramfs.h>

/* A file's data is a list of page-sized blocks rather than one
   reallocated buffer: growing a file then costs one page allocation, not
   a copy of everything written so far.  For a filesystem that holds a
   kernel log or a shell history, that difference is the whole cost
   model. */
#define RAMFS_BLOCK_SIZE 4096

struct ramfs_block {
	struct list_head node;
	uint32_t         index;    /* which block of the file this is */
	uint8_t         *data;
};

struct ramfs_inode {
	struct vfs_node  vnode;

	/* Directory contents, or file blocks. */
	struct list_head children;    /* struct ramfs_inode::sibling */
	struct list_head sibling;
	struct list_head blocks;      /* struct ramfs_block::node */

	struct ramfs_inode *parent;
	uint32_t            child_count;
	uint32_t            block_count;
};

struct ramfs_super {
	struct ramfs_inode *root;
	struct mutex        lock;
	uint64_t            total_bytes;
	uint32_t            node_count;
};

static const struct vfs_ops ramfs_ops;

/* ── Node management ─────────────────────────────────────────────────── */

static struct ramfs_inode *inode_create(struct ramfs_super *sb,
                                        const char *name,
                                        enum vfs_node_type type,
                                        uint32_t perm)
{
	struct ramfs_inode *in = kzalloc(sizeof(*in));
	if (!in)
		return NULL;

	list_init(&in->children);
	list_init(&in->sibling);
	list_init(&in->blocks);

	strlcpy(in->vnode.name, name, sizeof(in->vnode.name));
	in->vnode.type     = type;
	in->vnode.perm     = perm;
	in->vnode.size     = 0;
	in->vnode.refcount = 1;
	in->vnode.mtime_ns = time_ns();
	in->vnode.fs_data  = in;

	sb->node_count++;
	return in;
}

static void inode_free_blocks(struct ramfs_super *sb,
                              struct ramfs_inode *in)
{
	struct ramfs_block *b, *tmp;
	list_for_each_entry_safe(b, tmp, &in->blocks, node) {
		list_del(&b->node);
		if (b->data) {
			vmm_free((vaddr_t)b->data, RAMFS_BLOCK_SIZE);
			if (sb->total_bytes >= RAMFS_BLOCK_SIZE)
				sb->total_bytes -= RAMFS_BLOCK_SIZE;
		}
		kfree(b);
	}
	in->block_count = 0;
	in->vnode.size  = 0;
}

static struct ramfs_block *block_find(struct ramfs_inode *in, uint32_t index)
{
	struct ramfs_block *b;
	list_for_each_entry(b, &in->blocks, node) {
		if (b->index == index)
			return b;
	}
	return NULL;
}

static struct ramfs_block *block_get_or_add(struct ramfs_super *sb,
                                            struct ramfs_inode *in,
                                            uint32_t index)
{
	struct ramfs_block *b = block_find(in, index);
	if (b)
		return b;

	b = kzalloc(sizeof(*b));
	if (!b)
		return NULL;

	b->data = (uint8_t *)vmm_alloc(RAMFS_BLOCK_SIZE,
	                               VM_PROT_READ | VM_PROT_WRITE);
	if (!b->data) {
		kfree(b);
		return NULL;
	}

	/* Zero on allocation.  A sparse file read between two written
	   blocks must return zeroes, not whatever that page held before --
	   which would be an information leak from whatever freed it. */
	memset(b->data, 0, RAMFS_BLOCK_SIZE);

	b->index = index;

	/* Keep the list in index order so a sequential read walks forward
	   rather than searching from the head each time. */
	struct list_head   *before = &in->blocks;
	struct ramfs_block *pos;
	list_for_each_entry(pos, &in->blocks, node) {
		if (pos->index > index)
			break;
		before = &pos->node;
	}
	__list_add(&b->node, before, before->next);

	in->block_count++;
	sb->total_bytes += RAMFS_BLOCK_SIZE;
	return b;
}

/* ── vfs_ops ─────────────────────────────────────────────────────────── */

static struct ramfs_super *super_of(struct vfs_node *node)
{
	return node && node->mount ? node->mount->fs_data : NULL;
}

static int ramfs_lookup(struct vfs_node *dir, const char *name,
                        struct vfs_node **out)
{
	struct ramfs_super *sb = super_of(dir);
	if (!sb)
		return -HAVEN_EIO;
	if (dir->type != VFS_NODE_DIR)
		return -HAVEN_ENOTDIR;

	struct ramfs_inode *d = dir->fs_data;
	if (!d)
		return -HAVEN_EIO;

	mutex_lock(&sb->lock);

	struct ramfs_inode *child;
	list_for_each_entry(child, &d->children, sibling) {
		if (strcmp(child->vnode.name, name) == 0) {
			vfs_node_get(&child->vnode);
			mutex_unlock(&sb->lock);
			*out = &child->vnode;
			return 0;
		}
	}

	mutex_unlock(&sb->lock);
	return -HAVEN_ENOENT;
}

static int ramfs_create(struct vfs_node *dir, const char *name, uint32_t perm,
                        enum vfs_node_type type, struct vfs_node **out)
{
	struct ramfs_super *sb = super_of(dir);
	if (!sb)
		return -HAVEN_EIO;
	if (dir->type != VFS_NODE_DIR)
		return -HAVEN_ENOTDIR;
	if (!name || !name[0])
		return -HAVEN_EINVAL;
	if (strlen(name) > VFS_NAME_MAX)
		return -HAVEN_ENAMETOOLONG;

	struct ramfs_inode *d = dir->fs_data;

	mutex_lock(&sb->lock);

	struct ramfs_inode *existing;
	list_for_each_entry(existing, &d->children, sibling) {
		if (strcmp(existing->vnode.name, name) == 0) {
			mutex_unlock(&sb->lock);
			return -HAVEN_EEXIST;
		}
	}

	struct ramfs_inode *in = inode_create(sb, name, type, perm);
	if (!in) {
		mutex_unlock(&sb->lock);
		/* -ENOMEM, not a truncated success.  HavenDOS's ramfs_write()
		   silently truncated a file larger than 2 KiB and reported
		   the truncated length as if it had written everything. */
		return -HAVEN_ENOMEM;
	}

	in->parent      = d;
	in->vnode.mount = dir->mount;
	in->vnode.uid   = dir->uid;
	in->vnode.gid   = dir->gid;

	list_add_tail(&in->sibling, &d->children);
	d->child_count++;
	d->vnode.mtime_ns = time_ns();

	mutex_unlock(&sb->lock);

	if (out) {
		*out = &in->vnode;
	} else {
		vfs_node_put(&in->vnode);
	}
	return 0;
}

static int ramfs_unlink(struct vfs_node *dir, const char *name)
{
	struct ramfs_super *sb = super_of(dir);
	if (!sb)
		return -HAVEN_EIO;

	struct ramfs_inode *d = dir->fs_data;

	mutex_lock(&sb->lock);

	struct ramfs_inode *child, *tmp;
	list_for_each_entry_safe(child, tmp, &d->children, sibling) {
		if (strcmp(child->vnode.name, name) != 0)
			continue;

		/* A non-empty directory must not be removed: its children
		   would become unreachable but still allocated. */
		if (child->vnode.type == VFS_NODE_DIR &&
		    !list_empty(&child->children)) {
			mutex_unlock(&sb->lock);
			return -HAVEN_ENOTEMPTY;
		}

		list_del(&child->sibling);
		if (d->child_count)
			d->child_count--;
		d->vnode.mtime_ns = time_ns();

		inode_free_blocks(sb, child);
		if (sb->node_count)
			sb->node_count--;

		mutex_unlock(&sb->lock);

		/* The node may still be open.  Dropping our reference lets
		   the last holder free it; freeing it here would leave a
		   dangling pointer in whoever has it open -- the classic
		   unlink-while-open bug. */
		vfs_node_put(&child->vnode);
		return 0;
	}

	mutex_unlock(&sb->lock);
	return -HAVEN_ENOENT;
}

static int ramfs_rename(struct vfs_node *dir, const char *from,
                        struct vfs_node *newdir, const char *to)
{
	struct ramfs_super *sb = super_of(dir);
	if (!sb)
		return -HAVEN_EIO;
	if (super_of(newdir) != sb)
		return -HAVEN_EXDEV;

	struct ramfs_inode *d  = dir->fs_data;
	struct ramfs_inode *nd = newdir->fs_data;

	mutex_lock(&sb->lock);

	struct ramfs_inode *src = NULL, *pos;
	list_for_each_entry(pos, &d->children, sibling) {
		if (strcmp(pos->vnode.name, from) == 0) {
			src = pos;
			break;
		}
	}
	if (!src) {
		mutex_unlock(&sb->lock);
		return -HAVEN_ENOENT;
	}

	list_for_each_entry(pos, &nd->children, sibling) {
		if (pos != src && strcmp(pos->vnode.name, to) == 0) {
			mutex_unlock(&sb->lock);
			return -HAVEN_EEXIST;
		}
	}

	/* Moving a directory into its own subtree would detach it from the
	   root and create a cycle: the subtree becomes unreachable and
	   unfreeable, and a later path walk through it never terminates. */
	if (src->vnode.type == VFS_NODE_DIR) {
		for (struct ramfs_inode *a = nd; a; a = a->parent) {
			if (a == src) {
				mutex_unlock(&sb->lock);
				return -HAVEN_EINVAL;
			}
		}
	}

	if (nd != d) {
		list_del(&src->sibling);
		if (d->child_count)
			d->child_count--;
		list_add_tail(&src->sibling, &nd->children);
		nd->child_count++;
		src->parent = nd;
	}

	strlcpy(src->vnode.name, to, sizeof(src->vnode.name));
	src->vnode.mtime_ns = time_ns();

	mutex_unlock(&sb->lock);
	return 0;
}

static ssize_t_k ramfs_read(struct vfs_node *node, void *buf, size_t len,
                            haven_off_t offset)
{
	struct ramfs_super *sb = super_of(node);
	if (!sb)
		return -HAVEN_EIO;

	struct ramfs_inode *in = node->fs_data;
	if (!in)
		return -HAVEN_EIO;

	mutex_lock(&sb->lock);

	if (offset >= node->size) {
		mutex_unlock(&sb->lock);
		return 0;   /* end of file, which is not an error */
	}

	/* Never read past the end, even if the caller asked for more. */
	uint64_t available = node->size - offset;
	if (len > available)
		len = (size_t)available;

	uint8_t *dst  = buf;
	size_t   done = 0;

	while (done < len) {
		uint64_t pos      = offset + done;
		uint32_t blk      = (uint32_t)(pos / RAMFS_BLOCK_SIZE);
		uint32_t in_block = (uint32_t)(pos % RAMFS_BLOCK_SIZE);
		size_t   chunk    = RAMFS_BLOCK_SIZE - in_block;
		if (chunk > len - done)
			chunk = len - done;

		struct ramfs_block *b = block_find(in, blk);
		if (b && b->data) {
			memcpy(dst + done, b->data + in_block, chunk);
		} else {
			/* A hole in a sparse file reads as zeroes. */
			memset(dst + done, 0, chunk);
		}
		done += chunk;
	}

	mutex_unlock(&sb->lock);
	return (ssize_t_k)done;
}

static ssize_t_k ramfs_write(struct vfs_node *node, const void *buf,
                             size_t len, haven_off_t offset)
{
	struct ramfs_super *sb = super_of(node);
	if (!sb)
		return -HAVEN_EIO;

	struct ramfs_inode *in = node->fs_data;
	if (!in)
		return -HAVEN_EIO;
	if (node->type != VFS_NODE_FILE)
		return -HAVEN_EISDIR;

	mutex_lock(&sb->lock);

	const uint8_t *src  = buf;
	size_t         done = 0;

	while (done < len) {
		uint64_t pos      = offset + done;
		uint32_t blk      = (uint32_t)(pos / RAMFS_BLOCK_SIZE);
		uint32_t in_block = (uint32_t)(pos % RAMFS_BLOCK_SIZE);
		size_t   chunk    = RAMFS_BLOCK_SIZE - in_block;
		if (chunk > len - done)
			chunk = len - done;

		struct ramfs_block *b = block_get_or_add(sb, in, blk);
		if (!b) {
			/* Out of memory part way through.  Report the bytes
			   that DID land, so the caller knows exactly how much
			   of its buffer was written.  Reporting the full
			   length -- which is what a truncating write does --
			   makes the caller believe data it can no longer
			   recover was stored. */
			break;
		}

		memcpy(b->data + in_block, src + done, chunk);
		done += chunk;

		if (pos + chunk > node->size)
			node->size = pos + chunk;
	}

	in->vnode.mtime_ns = time_ns();
	mutex_unlock(&sb->lock);

	if (done == 0 && len > 0)
		return -HAVEN_ENOSPC;
	return (ssize_t_k)done;
}

static int ramfs_truncate(struct vfs_node *node, uint64_t length)
{
	struct ramfs_super *sb = super_of(node);
	if (!sb)
		return -HAVEN_EIO;

	struct ramfs_inode *in = node->fs_data;
	mutex_lock(&sb->lock);

	if (length == 0) {
		inode_free_blocks(sb, in);
	} else if (length < node->size) {
		/* Drop whole blocks past the new end, and zero the tail of
		   the block that straddles it: a later extend must not expose
		   the data that used to be there. */
		uint32_t last_blk = (uint32_t)((length - 1) / RAMFS_BLOCK_SIZE);
		uint32_t tail_off = (uint32_t)(length % RAMFS_BLOCK_SIZE);

		struct ramfs_block *b, *tmp;
		list_for_each_entry_safe(b, tmp, &in->blocks, node) {
			if (b->index > last_blk) {
				list_del(&b->node);
				if (b->data) {
					vmm_free((vaddr_t)b->data,
					         RAMFS_BLOCK_SIZE);
					if (sb->total_bytes >= RAMFS_BLOCK_SIZE)
						sb->total_bytes -= RAMFS_BLOCK_SIZE;
				}
				kfree(b);
				if (in->block_count)
					in->block_count--;
			} else if (b->index == last_blk && tail_off &&
			           b->data) {
				memset(b->data + tail_off, 0,
				       RAMFS_BLOCK_SIZE - tail_off);
			}
		}
		node->size = length;
	} else {
		/* Extending leaves holes, which read as zeroes. */
		node->size = length;
	}

	in->vnode.mtime_ns = time_ns();
	mutex_unlock(&sb->lock);
	return 0;
}

static int ramfs_readdir(struct vfs_node *dir, uint32_t index,
                         struct vfs_dirent *out)
{
	struct ramfs_super *sb = super_of(dir);
	if (!sb)
		return -HAVEN_EIO;
	if (dir->type != VFS_NODE_DIR)
		return -HAVEN_ENOTDIR;

	struct ramfs_inode *d = dir->fs_data;

	mutex_lock(&sb->lock);

	uint32_t i = 0;
	struct ramfs_inode *child;
	list_for_each_entry(child, &d->children, sibling) {
		if (i++ != index)
			continue;

		strlcpy(out->name, child->vnode.name, sizeof(out->name));
		out->size  = child->vnode.size;
		out->type  = child->vnode.type;
		out->index = index;

		mutex_unlock(&sb->lock);
		return 0;
	}

	mutex_unlock(&sb->lock);
	/* Past the last entry.  -ENOENT is how the caller's loop knows to
	   stop, which is why readdir takes an index rather than a callback:
	   HavenDOS's ramfs_list() took a callback and could not be stopped
	   part way or resumed. */
	return -HAVEN_ENOENT;
}

static int ramfs_stat(struct vfs_node *node, struct vfs_stat *out)
{
	struct ramfs_inode *in = node->fs_data;

	memset(out, 0, sizeof(*out));
	out->size     = node->size;
	out->type     = node->type;
	out->perm     = node->perm;
	out->uid      = node->uid;
	out->gid      = node->gid;
	out->mtime_ns = node->mtime_ns;
	out->nlink    = (node->type == VFS_NODE_DIR && in)
	              ? in->child_count + 2 : 1;
	return 0;
}

static int ramfs_chmod(struct vfs_node *node, uint32_t perm)
{
	node->perm = perm & 07777;
	return 0;
}

static int ramfs_chown(struct vfs_node *node, uid_t_k uid, gid_t_k gid)
{
	node->uid = uid;
	node->gid = gid;
	return 0;
}

static void ramfs_release(struct vfs_node *node)
{
	struct ramfs_inode *in = node->fs_data;
	if (!in)
		return;

	struct ramfs_super *sb = super_of(node);

	/* Only free a node that has been unlinked: one still in its
	   parent's list is reachable and must stay. */
	if (in->sibling.next != NULL)
		return;

	if (sb)
		inode_free_blocks(sb, in);
	kfree(in);
}

static int ramfs_sync(struct vfs_mount *mount)
{
	/* A RAM filesystem has nowhere to flush to, and that is not a
	   failure -- returning an error here would make vfs_sync() report a
	   problem on every shutdown. */
	(void)mount;
	return 0;
}

static const struct vfs_ops ramfs_ops = {
	.name     = "ramfs",
	.lookup   = ramfs_lookup,
	.create   = ramfs_create,
	.unlink   = ramfs_unlink,
	.rename   = ramfs_rename,
	.read     = ramfs_read,
	.write    = ramfs_write,
	.truncate = ramfs_truncate,
	.readdir  = ramfs_readdir,
	.stat     = ramfs_stat,
	.chmod    = ramfs_chmod,
	.chown    = ramfs_chown,
	.sync     = ramfs_sync,
	.release  = ramfs_release,
};

/* ── Mount ───────────────────────────────────────────────────────────── */

int ramfs_mount_at(const char *path)
{
	struct ramfs_super *sb = kzalloc(sizeof(*sb));
	if (!sb)
		return -HAVEN_ENOMEM;

	mutex_init(&sb->lock, "ramfs");

	sb->root = inode_create(sb, "/", VFS_NODE_DIR, VFS_PERM_DEFAULT_DIR);
	if (!sb->root) {
		kfree(sb);
		return -HAVEN_ENOMEM;
	}

	int rc = vfs_mount(path, &ramfs_ops, sb, &sb->root->vnode);
	if (rc != 0) {
		kfree(sb->root);
		kfree(sb);
		return rc;
	}

	return 0;
}

uint64_t ramfs_bytes_used(struct vfs_mount *mount)
{
	struct ramfs_super *sb = mount ? mount->fs_data : NULL;
	return sb ? sb->total_bytes : 0;
}
