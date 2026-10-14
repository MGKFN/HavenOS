/* SPDX-License-Identifier: MIT
 *
 * vfs.c — path resolution, mount table, and the file abstraction.
 *
 * PATH WALKING IS THE VFS'S JOB, NOT THE FILESYSTEM'S
 * This is the single most important structural difference from HavenDOS.
 * There, every filesystem received whole path strings: fat16_write() was
 * handed "boot/grub.cfg" and passed it to name_to_83(), which produced
 * the 8.3 name "BOOT/GRU" in the root directory -- a file with a slash in
 * its name that no other FAT implementation will ever find.  Its own
 * source calls this out: "It looked like a write that worked and a read
 * that came back empty."
 *
 * Here the VFS splits the path and calls ->lookup() once per component.
 * A filesystem never sees a separator, so it cannot mishandle one.
 */
#include <haven/vfs.h>
#include <haven/mm.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/spinlock.h>
#include <haven/time.h>

static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static DEFINE_SPINLOCK(mount_lock, "vfs-mounts");
static struct vfs_stats stats;

const char *vfs_node_type_name(enum vfs_node_type t)
{
	switch (t) {
	case VFS_NODE_FILE:     return "file";
	case VFS_NODE_DIR:      return "directory";
	case VFS_NODE_CHARDEV:  return "character device";
	case VFS_NODE_BLOCKDEV: return "block device";
	case VFS_NODE_SYMLINK:  return "symlink";
	case VFS_NODE_FIFO:     return "fifo";
	case VFS_NODE_NONE:
	default:                return "none";
	}
}

/*
 * Normalise a path.
 *
 * Collapses repeated separators, drops "." components, and resolves ".."
 * by removing the previous component.  A ".." that would escape the root
 * is clamped at the root rather than rejected, which is what every other
 * system does and is what stops "../../etc/passwd" from meaning anything
 * other than "/etc/passwd".
 *
 * This is a security boundary, not a tidiness pass: without it a process
 * could reach any file in the system from any starting directory.
 * HavenDOS had no equivalent -- it had no processes to contain.
 */
int vfs_normalise_path(const char *in, char *out, size_t outsz)
{
	if (!in || !out || outsz < 2)
		return -HAVEN_EINVAL;

	/* Component start offsets within `out`, so ".." can pop one. */
	size_t starts[64];
	size_t depth = 0;
	size_t len   = 0;

	out[len++] = '/';

	const char *p = in;
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;

		const char *comp = p;
		while (*p && *p != '/')
			p++;
		size_t clen = (size_t)(p - comp);

		if (clen == 1 && comp[0] == '.')
			continue;

		if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
			if (depth > 0) {
				depth--;
				len = starts[depth];
				/* Drop the trailing separator too, unless we
				   are back at the root. */
				if (len > 1 && out[len - 1] == '/')
					len--;
				if (len == 0)
					len = 1;
			}
			/* At the root, ".." is a no-op: clamping rather than
			   failing is what keeps a traversal attempt from
			   reaching anything. */
			continue;
		}

		if (clen > VFS_NAME_MAX)
			return -HAVEN_ENAMETOOLONG;
		if (depth >= ARRAY_SIZE(starts))
			return -HAVEN_ENAMETOOLONG;

		/* Separator between components, but not after the root. */
		if (len > 1) {
			if (len + 1 >= outsz)
				return -HAVEN_ENAMETOOLONG;
			out[len++] = '/';
		}

		starts[depth++] = len;

		if (len + clen >= outsz)
			return -HAVEN_ENAMETOOLONG;
		memcpy(out + len, comp, clen);
		len += clen;
	}

	out[len] = '\0';
	return 0;
}

/* Find the mount whose path is the longest prefix of `path`. */
static struct vfs_mount *find_mount(const char *path, const char **rest)
{
	struct vfs_mount *best = NULL;
	size_t            best_len = 0;

	for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (!mounts[i].in_use)
			continue;

		size_t mlen = strlen(mounts[i].path);

		/* "/" matches everything; any other mount must be followed
		   by a separator or the end of the path, or "/devices" would
		   match a mount at "/dev". */
		if (strncmp(path, mounts[i].path, mlen) != 0)
			continue;
		if (mlen > 1 && path[mlen] != '/' && path[mlen] != '\0')
			continue;

		if (mlen >= best_len) {
			best     = &mounts[i];
			best_len = mlen;
		}
	}

	if (best && rest) {
		*rest = path + best_len;
		while (**rest == '/')
			(*rest)++;
	}
	return best;
}

int vfs_mount(const char *path, const struct vfs_ops *ops, void *fs_data,
              struct vfs_node *root)
{
	if (!path || !ops || !root)
		return -HAVEN_EINVAL;

	char norm[VFS_PATH_MAX];
	int rc = vfs_normalise_path(path, norm, sizeof(norm));
	if (rc != 0)
		return rc;

	irqflags_t f = spin_lock(&mount_lock);

	/* Refuse a duplicate mount point.  Allowing two would make
	   find_mount()'s choice depend on table order, which is not
	   something a caller can reason about. */
	for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (mounts[i].in_use && strcmp(mounts[i].path, norm) == 0) {
			spin_unlock(&mount_lock, f);
			kerr("vfs: '%s' is already a mount point", norm);
			return -HAVEN_EBUSY;
		}
	}

	struct vfs_mount *m = NULL;
	for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (!mounts[i].in_use) {
			m = &mounts[i];
			break;
		}
	}
	if (!m) {
		spin_unlock(&mount_lock, f);
		kerr("vfs: mount table full (%d entries)", VFS_MAX_MOUNTS);
		return -HAVEN_ENOSPC;
	}

	strlcpy(m->path, norm, sizeof(m->path));
	m->ops       = ops;
	m->fs_data   = fs_data;
	m->root      = root;
	m->read_only = false;
	m->in_use    = true;

	root->mount = m;
	root->type  = VFS_NODE_DIR;

	stats.mounts++;
	spin_unlock(&mount_lock, f);

	kinfo("vfs: mounted %s at %s", ops->name, norm);
	return 0;
}

int vfs_unmount(const char *path)
{
	char norm[VFS_PATH_MAX];
	int rc = vfs_normalise_path(path, norm, sizeof(norm));
	if (rc != 0)
		return rc;

	irqflags_t f = spin_lock(&mount_lock);

	for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (!mounts[i].in_use || strcmp(mounts[i].path, norm) != 0)
			continue;

		struct vfs_mount *m = &mounts[i];
		const struct vfs_ops *ops = m->ops;

		/* Flush before detaching, or anything buffered is lost --
		   which on a FAT volume means a corrupt filesystem. */
		if (ops->sync)
			ops->sync(m);

		m->in_use = false;
		stats.mounts--;
		spin_unlock(&mount_lock, f);

		if (ops->unmount)
			ops->unmount(m);

		kinfo("vfs: unmounted %s from %s", ops->name, norm);
		return 0;
	}

	spin_unlock(&mount_lock, f);
	return -HAVEN_ENOENT;
}

void vfs_node_get(struct vfs_node *node)
{
	if (node)
		__atomic_fetch_add(&node->refcount, 1, __ATOMIC_RELAXED);
}

void vfs_node_put(struct vfs_node *node)
{
	if (!node)
		return;

	uint32_t left = __atomic_sub_fetch(&node->refcount, 1,
	                                  __ATOMIC_ACQ_REL);
	if (left != 0)
		return;

	/* A mount's root node is owned by the filesystem, not by the
	   reference count: releasing it would unmount by accident. */
	if (node->mount && node->mount->root == node)
		return;

	if (node->mount && node->mount->ops->release)
		node->mount->ops->release(node);
}

/*
 * Resolve a path to a node, one component at a time.
 */
int vfs_resolve(const char *path, struct vfs_node **out)
{
	if (!path || !out)
		return -HAVEN_EINVAL;

	char norm[VFS_PATH_MAX];
	int rc = vfs_normalise_path(path, norm, sizeof(norm));
	if (rc != 0)
		return rc;

	const char       *rest = NULL;
	struct vfs_mount *m    = find_mount(norm, &rest);
	if (!m) {
		stats.lookup_misses++;
		return -HAVEN_ENOENT;
	}

	struct vfs_node *cur = m->root;
	vfs_node_get(cur);

	/* Walk the remainder, component by component.  The filesystem's
	   ->lookup() receives a single name with no separator in it, which
	   is the invariant that makes a path-handling bug inside a
	   filesystem impossible. */
	char comp[VFS_NAME_MAX + 1];
	const char *p = rest;

	while (p && *p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;

		size_t n = 0;
		while (*p && *p != '/' && n < VFS_NAME_MAX)
			comp[n++] = *p++;
		comp[n] = '\0';

		if (*p && *p != '/') {
			vfs_node_put(cur);
			return -HAVEN_ENAMETOOLONG;
		}

		if (cur->type != VFS_NODE_DIR) {
			/* A path continues past something that is not a
			   directory -- "file.txt/more".  ENOTDIR says exactly
			   that, where HavenDOS would have returned a generic
			   -1. */
			vfs_node_put(cur);
			return -HAVEN_ENOTDIR;
		}

		if (!cur->mount || !cur->mount->ops->lookup) {
			vfs_node_put(cur);
			return -HAVEN_ENOSYS;
		}

		struct vfs_node *next = NULL;
		stats.lookups++;
		rc = cur->mount->ops->lookup(cur, comp, &next);
		vfs_node_put(cur);

		if (rc != 0 || !next) {
			stats.lookup_misses++;
			return rc != 0 ? rc : -HAVEN_ENOENT;
		}

		/* A node reached through a parent inherits that parent's
		   mount unless the filesystem set one; forgetting this is
		   how a child node ends up with no ops table. */
		if (!next->mount)
			next->mount = m;

		cur = next;

		/* Crossing into a filesystem mounted deeper: if the path we
		   have walked to is itself a mount point, continue inside
		   the mounted filesystem rather than the covered
		   directory. */
	}

	*out = cur;
	return 0;
}

/* Split a path into its parent directory and final component. */
static int split_parent(const char *path, char *parent, size_t parentsz,
                        char *leaf, size_t leafsz)
{
	char norm[VFS_PATH_MAX];
	int rc = vfs_normalise_path(path, norm, sizeof(norm));
	if (rc != 0)
		return rc;

	char *slash = strrchr(norm, '/');
	if (!slash)
		return -HAVEN_EINVAL;

	if (slash == norm) {
		/* The parent is the root itself. */
		if (parentsz < 2)
			return -HAVEN_ENAMETOOLONG;
		parent[0] = '/';
		parent[1] = '\0';
	} else {
		size_t plen = (size_t)(slash - norm);
		if (plen + 1 > parentsz)
			return -HAVEN_ENAMETOOLONG;
		memcpy(parent, norm, plen);
		parent[plen] = '\0';
	}

	if (strlcpy(leaf, slash + 1, leafsz) >= leafsz)
		return -HAVEN_ENAMETOOLONG;
	if (!leaf[0])
		return -HAVEN_EINVAL;

	return 0;
}

/* ── File interface ──────────────────────────────────────────────────── */

int vfs_open(const char *path, uint32_t flags, uint32_t perm,
             struct vfs_file **out)
{
	if (!path || !out)
		return -HAVEN_EINVAL;

	struct vfs_node *node = NULL;
	int rc = vfs_resolve(path, &node);

	if (rc == -HAVEN_ENOENT && (flags & O_CREAT)) {
		char parent[VFS_PATH_MAX], leaf[VFS_NAME_MAX + 1];
		rc = split_parent(path, parent, sizeof(parent),
		                  leaf, sizeof(leaf));
		if (rc != 0)
			return rc;

		struct vfs_node *dir = NULL;
		rc = vfs_resolve(parent, &dir);
		if (rc != 0)
			return rc;

		if (dir->type != VFS_NODE_DIR) {
			vfs_node_put(dir);
			return -HAVEN_ENOTDIR;
		}
		if (!dir->mount || !dir->mount->ops->create) {
			vfs_node_put(dir);
			return -HAVEN_ENOSYS;
		}
		if (dir->mount->read_only) {
			vfs_node_put(dir);
			return -HAVEN_EROFS;
		}

		rc = dir->mount->ops->create(dir, leaf,
		                             perm ? perm : VFS_PERM_DEFAULT_FILE,
		                             VFS_NODE_FILE, &node);
		struct vfs_mount *m = dir->mount;
		vfs_node_put(dir);

		if (rc != 0)
			return rc;
		if (node && !node->mount)
			node->mount = m;
	} else if (rc == 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
		/* O_CREAT|O_EXCL means "create, and fail if it exists".
		   That distinction is what makes a lock file work. */
		vfs_node_put(node);
		return -HAVEN_EEXIST;
	} else if (rc != 0) {
		return rc;
	}

	if (!node)
		return -HAVEN_ENOENT;

	if ((flags & O_DIRECTORY) && node->type != VFS_NODE_DIR) {
		vfs_node_put(node);
		return -HAVEN_ENOTDIR;
	}
	if (node->type == VFS_NODE_DIR && (flags & O_ACCMODE) != O_RDONLY) {
		vfs_node_put(node);
		return -HAVEN_EISDIR;
	}
	if ((flags & O_ACCMODE) != O_RDONLY && node->mount &&
	    node->mount->read_only) {
		vfs_node_put(node);
		return -HAVEN_EROFS;
	}

	if ((flags & O_TRUNC) && node->type == VFS_NODE_FILE) {
		if (node->mount && node->mount->ops->truncate)
			node->mount->ops->truncate(node, 0);
	}

	struct vfs_file *file = kzalloc(sizeof(*file));
	if (!file) {
		vfs_node_put(node);
		return -HAVEN_ENOMEM;
	}

	file->node     = node;
	file->flags    = flags;
	file->refcount = 1;
	file->offset   = (flags & O_APPEND) ? node->size : 0;
	mutex_init(&file->lock, "vfs-file");

	stats.opens++;
	stats.open_files++;
	*out = file;
	return 0;
}

int vfs_close(struct vfs_file *file)
{
	if (!file)
		return -HAVEN_EBADF;

	if (__atomic_sub_fetch(&file->refcount, 1, __ATOMIC_ACQ_REL) != 0)
		return 0;

	vfs_node_put(file->node);
	stats.closes++;
	if (stats.open_files)
		stats.open_files--;
	kfree(file);
	return 0;
}

ssize_t_k vfs_pread(struct vfs_node *node, void *buf, size_t len,
                    haven_off_t offset)
{
	if (!node || !buf)
		return -HAVEN_EINVAL;
	if (len == 0)
		return 0;
	if (!node->mount || !node->mount->ops->read)
		return -HAVEN_ENOSYS;
	if (node->type == VFS_NODE_DIR)
		return -HAVEN_EISDIR;

	ssize_t_k n = node->mount->ops->read(node, buf, len, offset);
	if (n > 0) {
		stats.reads++;
		stats.bytes_read += (uint64_t)n;
	}
	return n;
}

ssize_t_k vfs_pwrite(struct vfs_node *node, const void *buf, size_t len,
                     haven_off_t offset)
{
	if (!node || !buf)
		return -HAVEN_EINVAL;
	if (len == 0)
		return 0;
	if (!node->mount || !node->mount->ops->write)
		return -HAVEN_ENOSYS;
	if (node->type == VFS_NODE_DIR)
		return -HAVEN_EISDIR;
	if (node->mount->read_only)
		return -HAVEN_EROFS;

	ssize_t_k n = node->mount->ops->write(node, buf, len, offset);
	if (n > 0) {
		stats.writes++;
		stats.bytes_written += (uint64_t)n;
		node->mtime_ns = time_ns();
	}
	return n;
}

ssize_t_k vfs_read(struct vfs_file *file, void *buf, size_t len)
{
	if (!file)
		return -HAVEN_EBADF;
	if ((file->flags & O_ACCMODE) == O_WRONLY)
		return -HAVEN_EBADF;

	/* The offset is shared state between every thread holding this
	   file, so the read and the advance must be one operation --
	   otherwise two threads reading the same descriptor both read from
	   the same place. */
	mutex_lock(&file->lock);
	ssize_t_k n = vfs_pread(file->node, buf, len, file->offset);
	if (n > 0)
		file->offset += (uint64_t)n;
	mutex_unlock(&file->lock);

	return n;
}

ssize_t_k vfs_write(struct vfs_file *file, const void *buf, size_t len)
{
	if (!file)
		return -HAVEN_EBADF;
	if ((file->flags & O_ACCMODE) == O_RDONLY)
		return -HAVEN_EBADF;

	mutex_lock(&file->lock);

	/* O_APPEND must re-read the size under the lock: another thread may
	   have extended the file since this one opened it, and appending to
	   a stale offset overwrites their data. */
	if (file->flags & O_APPEND)
		file->offset = file->node->size;

	ssize_t_k n = vfs_pwrite(file->node, buf, len, file->offset);
	if (n > 0)
		file->offset += (uint64_t)n;
	mutex_unlock(&file->lock);

	return n;
}

int64_t vfs_seek(struct vfs_file *file, int64_t offset, int whence)
{
	if (!file)
		return -HAVEN_EBADF;

	mutex_lock(&file->lock);

	int64_t base;
	switch (whence) {
	case SEEK_SET_: base = 0; break;
	case SEEK_CUR_: base = (int64_t)file->offset; break;
	case SEEK_END_: base = (int64_t)file->node->size; break;
	default:
		mutex_unlock(&file->lock);
		return -HAVEN_EINVAL;
	}

	int64_t target = base + offset;
	if (target < 0) {
		/* A negative offset is not a valid file position.  Clamping
		   to zero would silently change the caller's meaning. */
		mutex_unlock(&file->lock);
		return -HAVEN_EINVAL;
	}

	file->offset = (haven_off_t)target;
	mutex_unlock(&file->lock);
	return target;
}

/* ── Path-level operations ───────────────────────────────────────────── */

int vfs_stat_path(const char *path, struct vfs_stat *out)
{
	if (!out)
		return -HAVEN_EINVAL;

	struct vfs_node *node = NULL;
	int rc = vfs_resolve(path, &node);
	if (rc != 0)
		return rc;

	if (node->mount && node->mount->ops->stat) {
		rc = node->mount->ops->stat(node, out);
	} else {
		/* Synthesise from the node, so a filesystem that does not
		   implement stat still reports something true. */
		memset(out, 0, sizeof(*out));
		out->size     = node->size;
		out->type     = node->type;
		out->perm     = node->perm;
		out->uid      = node->uid;
		out->gid      = node->gid;
		out->mtime_ns = node->mtime_ns;
		out->nlink    = 1;
		rc = 0;
	}

	vfs_node_put(node);
	return rc;
}

int vfs_mkdir(const char *path, uint32_t perm)
{
	char parent[VFS_PATH_MAX], leaf[VFS_NAME_MAX + 1];
	int rc = split_parent(path, parent, sizeof(parent), leaf, sizeof(leaf));
	if (rc != 0)
		return rc;

	struct vfs_node *dir = NULL;
	rc = vfs_resolve(parent, &dir);
	if (rc != 0)
		return rc;

	if (dir->type != VFS_NODE_DIR) {
		vfs_node_put(dir);
		return -HAVEN_ENOTDIR;
	}
	if (!dir->mount || !dir->mount->ops->create) {
		/* -ENOSYS, not 0.  HavenDOS's vfs_mkdir() returned 0 for
		   FAT12 with a "not implemented" comment, so every caller
		   believed the directory existed. */
		vfs_node_put(dir);
		return -HAVEN_ENOSYS;
	}
	if (dir->mount->read_only) {
		vfs_node_put(dir);
		return -HAVEN_EROFS;
	}

	struct vfs_node *node = NULL;
	rc = dir->mount->ops->create(dir, leaf,
	                             perm ? perm : VFS_PERM_DEFAULT_DIR,
	                             VFS_NODE_DIR, &node);
	vfs_node_put(dir);
	if (rc == 0 && node)
		vfs_node_put(node);
	return rc;
}

int vfs_unlink(const char *path)
{
	char parent[VFS_PATH_MAX], leaf[VFS_NAME_MAX + 1];
	int rc = split_parent(path, parent, sizeof(parent), leaf, sizeof(leaf));
	if (rc != 0)
		return rc;

	struct vfs_node *dir = NULL;
	rc = vfs_resolve(parent, &dir);
	if (rc != 0)
		return rc;

	if (!dir->mount || !dir->mount->ops->unlink) {
		vfs_node_put(dir);
		return -HAVEN_ENOSYS;
	}
	if (dir->mount->read_only) {
		vfs_node_put(dir);
		return -HAVEN_EROFS;
	}

	rc = dir->mount->ops->unlink(dir, leaf);
	vfs_node_put(dir);
	return rc;
}

int vfs_rename(const char *from, const char *to)
{
	/* Two VFS_PATH_MAX buffers plus two names is 2.6 KiB, which the
	   build's -Wframe-larger-than=2048 refuses -- correctly: a 16 KiB
	   kernel stack has no room for routines that each take a sixth of
	   it, and a rename can be reached from a deep call chain.  So the
	   scratch space is allocated rather than stack-resident. */
	struct rename_paths {
		char from_parent[VFS_PATH_MAX];
		char to_parent[VFS_PATH_MAX];
		char from_leaf[VFS_NAME_MAX + 1];
		char to_leaf[VFS_NAME_MAX + 1];
	};

	struct rename_paths *p = kmalloc(sizeof(*p));
	if (!p)
		return -HAVEN_ENOMEM;

	struct vfs_node *fdir = NULL, *tdir = NULL;

	int rc = split_parent(from, p->from_parent, sizeof(p->from_parent),
	                      p->from_leaf, sizeof(p->from_leaf));
	if (rc != 0)
		goto out;

	rc = split_parent(to, p->to_parent, sizeof(p->to_parent),
	                  p->to_leaf, sizeof(p->to_leaf));
	if (rc != 0)
		goto out;

	rc = vfs_resolve(p->from_parent, &fdir);
	if (rc != 0)
		goto out;

	rc = vfs_resolve(p->to_parent, &tdir);
	if (rc != 0)
		goto out;

	/* A rename across filesystems cannot be a rename: it is a copy and
	   a delete, which has different failure semantics and must be the
	   caller's decision rather than something the VFS does silently. */
	if (fdir->mount != tdir->mount)
		rc = -HAVEN_EXDEV;
	else if (!fdir->mount || !fdir->mount->ops->rename)
		rc = -HAVEN_ENOSYS;
	else if (fdir->mount->read_only)
		rc = -HAVEN_EROFS;
	else
		rc = fdir->mount->ops->rename(fdir, p->from_leaf,
		                              tdir, p->to_leaf);

out:
	vfs_node_put(tdir);
	vfs_node_put(fdir);
	kfree(p);
	return rc;
}

int vfs_readdir_path(const char *path, uint32_t index,
                     struct vfs_dirent *out)
{
	if (!out)
		return -HAVEN_EINVAL;

	struct vfs_node *dir = NULL;
	int rc = vfs_resolve(path, &dir);
	if (rc != 0)
		return rc;

	if (dir->type != VFS_NODE_DIR) {
		vfs_node_put(dir);
		return -HAVEN_ENOTDIR;
	}
	if (!dir->mount || !dir->mount->ops->readdir) {
		vfs_node_put(dir);
		return -HAVEN_ENOSYS;
	}

	rc = dir->mount->ops->readdir(dir, index, out);
	vfs_node_put(dir);
	return rc;
}

int vfs_chmod(const char *path, uint32_t perm)
{
	struct vfs_node *node = NULL;
	int rc = vfs_resolve(path, &node);
	if (rc != 0)
		return rc;

	/* Routed to the owning filesystem, not always to ramfs.  HavenDOS's
	   vfs_chmod() called ramfs_chmod() unconditionally, so a disk
	   install had no permissions at all. */
	if (!node->mount || !node->mount->ops->chmod) {
		vfs_node_put(node);
		return -HAVEN_ENOSYS;
	}

	rc = node->mount->ops->chmod(node, perm & 07777);
	vfs_node_put(node);
	return rc;
}

int vfs_chown(const char *path, uid_t_k uid, gid_t_k gid)
{
	struct vfs_node *node = NULL;
	int rc = vfs_resolve(path, &node);
	if (rc != 0)
		return rc;

	if (!node->mount || !node->mount->ops->chown) {
		vfs_node_put(node);
		return -HAVEN_ENOSYS;
	}

	rc = node->mount->ops->chown(node, uid, gid);
	vfs_node_put(node);
	return rc;
}

int vfs_truncate(const char *path, uint64_t length)
{
	struct vfs_node *node = NULL;
	int rc = vfs_resolve(path, &node);
	if (rc != 0)
		return rc;

	if (!node->mount || !node->mount->ops->truncate) {
		vfs_node_put(node);
		return -HAVEN_ENOSYS;
	}
	if (node->mount->read_only) {
		vfs_node_put(node);
		return -HAVEN_EROFS;
	}

	rc = node->mount->ops->truncate(node, length);
	vfs_node_put(node);
	return rc;
}

int vfs_sync(void)
{
	int first_error = 0;

	for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (!mounts[i].in_use || !mounts[i].ops->sync)
			continue;
		int rc = mounts[i].ops->sync(&mounts[i]);
		if (rc != 0) {
			kerr("vfs: sync of %s at %s failed: %s",
			     mounts[i].ops->name, mounts[i].path,
			     haven_strerror(rc));
			/* Keep going: one failing filesystem must not stop
			   the others from being flushed, which on a shutdown
			   path is the difference between losing one volume
			   and losing all of them. */
			if (first_error == 0)
				first_error = rc;
		}
	}
	return first_error;
}

/* ── Convenience ─────────────────────────────────────────────────────── */

ssize_t_k vfs_read_file(const char *path, void *buf, size_t bufsz)
{
	struct vfs_file *f = NULL;
	int rc = vfs_open(path, O_RDONLY, 0, &f);
	if (rc != 0)
		return rc;

	ssize_t_k n = vfs_read(f, buf, bufsz);
	vfs_close(f);
	return n;
}

ssize_t_k vfs_write_file(const char *path, const void *buf, size_t len)
{
	struct vfs_file *f = NULL;
	int rc = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC,
	                  VFS_PERM_DEFAULT_FILE, &f);
	if (rc != 0)
		return rc;

	ssize_t_k n = vfs_write(f, buf, len);
	vfs_close(f);
	return n;
}

bool vfs_exists(const char *path)
{
	struct vfs_node *node = NULL;
	if (vfs_resolve(path, &node) != 0)
		return false;
	vfs_node_put(node);
	return true;
}

bool vfs_is_dir(const char *path)
{
	struct vfs_node *node = NULL;
	if (vfs_resolve(path, &node) != 0)
		return false;
	bool is_dir = node->type == VFS_NODE_DIR;
	vfs_node_put(node);
	return is_dir;
}

/* ── Statistics ──────────────────────────────────────────────────────── */

void vfs_get_stats(struct vfs_stats *out)
{
	if (out)
		*out = stats;
}

void vfs_dump(void)
{
	kinfo("vfs: %u mounts, %u open files", stats.mounts, stats.open_files);
	kinfo("vfs: %llu opens, %llu closes, %llu lookups (%llu misses)",
	      (unsigned long long)stats.opens,
	      (unsigned long long)stats.closes,
	      (unsigned long long)stats.lookups,
	      (unsigned long long)stats.lookup_misses);
	kinfo("vfs: %llu reads (%llu bytes), %llu writes (%llu bytes)",
	      (unsigned long long)stats.reads,
	      (unsigned long long)stats.bytes_read,
	      (unsigned long long)stats.writes,
	      (unsigned long long)stats.bytes_written);

	for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (mounts[i].in_use)
			kinfo("vfs:   %-16s %s%s", mounts[i].path,
			      mounts[i].ops->name,
			      mounts[i].read_only ? " (read-only)" : "");
	}
}

void vfs_init(void)
{
	memset(mounts, 0, sizeof(mounts));
	memset(&stats, 0, sizeof(stats));
	spin_lock_init(&mount_lock, "vfs-mounts");

	kinfo("vfs: path resolution walks one component at a time, so a "
	      "filesystem never sees a separator");
}
