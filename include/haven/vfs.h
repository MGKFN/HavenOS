/* SPDX-License-Identifier: MIT
 *
 * vfs.h — the virtual filesystem switch.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's fs/vfs.c was a switch statement in function form: thirty
 * `extern` declarations of fat12_*, fat16_* and ramfs_* entry points, two
 * static booleans recording which was mounted, and every vfs_* function
 * dispatching with an if-else chain.  Its own source records the bugs
 * that shape produced:
 *
 *   - the path-aware FAT16 entry points existed from v0.5.9 but the VFS
 *     never routed to them, so "boot/grub.cfg" became the 8.3 name
 *     "BOOT/GRU" in the root directory -- a file with a slash in its
 *     name that no other FAT implementation would ever find.  Anything
 *     that produced a path rather than a bare filename hit it.
 *   - vfs_mkdir() returned 0 for FAT12 with the comment "FAT12 mkdir not
 *     implemented", so a caller could not tell success from a silent
 *     no-op.
 *   - vfs_is_dir() always asked ramfs, regardless of what was mounted.
 *   - permissions (chmod, chown, owner) always went to ramfs too, so a
 *     disk install had no permissions at all.
 *
 * Every one of those is a consequence of dispatching by hand rather than
 * through an interface.  HavenOS gives each filesystem a struct of
 * operations and one mount table.  A filesystem that does not implement
 * an operation leaves the pointer NULL and the VFS returns -ENOSYS --
 * which the caller can distinguish from success, unlike a 0.
 *
 * MOUNT POINTS, NOT ONE GLOBAL FILESYSTEM
 * HavenDOS had exactly one filesystem active at a time.  HavenOS mounts
 * filesystems at paths, so a RAM filesystem, a device filesystem and a
 * disk can all be visible at once -- which is required for /dev to exist
 * at all.
 */
#ifndef _HAVEN_VFS_H
#define _HAVEN_VFS_H

#include <haven/types.h>
#include <haven/list.h>
#include <haven/mutex.h>

#define VFS_NAME_MAX  255
#define VFS_PATH_MAX  1024
#define VFS_MAX_MOUNTS 8

enum vfs_node_type {
	VFS_NODE_NONE = 0,
	VFS_NODE_FILE,
	VFS_NODE_DIR,
	VFS_NODE_CHARDEV,
	VFS_NODE_BLOCKDEV,
	VFS_NODE_SYMLINK,
	VFS_NODE_FIFO,
};

/* Permission bits, in the familiar octal arrangement.  HavenDOS had a
   single `admin_only` boolean, which cannot express "readable by anyone,
   writable by its owner" -- the permission almost every file wants. */
#define VFS_PERM_OTHER_X  0001
#define VFS_PERM_OTHER_W  0002
#define VFS_PERM_OTHER_R  0004
#define VFS_PERM_GROUP_X  0010
#define VFS_PERM_GROUP_W  0020
#define VFS_PERM_GROUP_R  0040
#define VFS_PERM_OWNER_X  0100
#define VFS_PERM_OWNER_W  0200
#define VFS_PERM_OWNER_R  0400

#define VFS_PERM_DEFAULT_FILE 0644
#define VFS_PERM_DEFAULT_DIR  0755

/* Open flags. */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_ACCMODE  0x0003
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_DIRECTORY 0x10000

/* Seek origins. */
#define SEEK_SET_   0
#define SEEK_CUR_   1
#define SEEK_END_   2

struct vfs_node;
struct vfs_mount;
struct vfs_file;

struct vfs_stat {
	uint64_t size;
	uint32_t type;        /* enum vfs_node_type */
	uint32_t perm;
	uid_t_k  uid;
	gid_t_k  gid;
	uint64_t mtime_ns;
	uint32_t nlink;
	uint32_t dev_major;
	uint32_t dev_minor;
};

/* One directory entry, as readdir() yields it. */
struct vfs_dirent {
	char     name[VFS_NAME_MAX + 1];
	uint64_t size;
	uint32_t type;
	uint32_t index;
};

/*
 * The per-filesystem operation table.
 *
 * Every function returns 0 or a positive count on success and -HAVEN_E*
 * on failure.  A NULL pointer means the filesystem does not implement the
 * operation, and the VFS returns -ENOSYS -- which is the distinction a
 * bare "return 0" with a not-implemented comment could not make, and
 * which HavenDOS's vfs_mkdir() got wrong for FAT12.
 */
struct vfs_ops {
	const char *name;

	/* Look up a single path component inside `dir`.  The VFS walks
	   paths itself and calls this once per component, so a filesystem
	   never has to parse a path -- which is how HavenDOS ended up with
	   a slash inside an 8.3 filename. */
	int  (*lookup)(struct vfs_node *dir, const char *name,
	               struct vfs_node **out);

	int  (*create)(struct vfs_node *dir, const char *name, uint32_t perm,
	               enum vfs_node_type type, struct vfs_node **out);
	int  (*unlink)(struct vfs_node *dir, const char *name);
	int  (*rename)(struct vfs_node *dir, const char *from,
	               struct vfs_node *newdir, const char *to);

	ssize_t_k (*read)(struct vfs_node *node, void *buf, size_t len,
	                  haven_off_t offset);
	ssize_t_k (*write)(struct vfs_node *node, const void *buf, size_t len,
	                   haven_off_t offset);
	int  (*truncate)(struct vfs_node *node, uint64_t length);

	int  (*readdir)(struct vfs_node *dir, uint32_t index,
	                struct vfs_dirent *out);

	int  (*stat)(struct vfs_node *node, struct vfs_stat *out);
	int  (*chmod)(struct vfs_node *node, uint32_t perm);
	int  (*chown)(struct vfs_node *node, uid_t_k uid, gid_t_k gid);

	int  (*sync)(struct vfs_mount *mount);
	void (*release)(struct vfs_node *node);
	void (*unmount)(struct vfs_mount *mount);

	/* Character and block devices route through here, which is what
	   lets /dev/console be a file. */
	int  (*ioctl)(struct vfs_node *node, uint32_t cmd, void *arg);
};

struct vfs_node {
	char               name[VFS_NAME_MAX + 1];
	enum vfs_node_type type;
	uint32_t           perm;
	uid_t_k            uid;
	gid_t_k            gid;
	uint64_t           size;
	uint64_t           mtime_ns;

	struct vfs_mount  *mount;     /* the filesystem that owns it */
	void              *fs_data;   /* the filesystem's own handle */
	uint32_t           refcount;

	/* Device nodes. */
	uint32_t           dev_major;
	uint32_t           dev_minor;
};

struct vfs_mount {
	char                  path[VFS_PATH_MAX];
	const struct vfs_ops *ops;
	struct vfs_node      *root;
	void                 *fs_data;
	bool                  read_only;
	bool                  in_use;
};

/* An open file.  Separate from the node, because two processes may have
   the same file open at different offsets -- which a single node cannot
   represent. */
struct vfs_file {
	struct vfs_node *node;
	haven_off_t      offset;
	uint32_t         flags;
	uint32_t         refcount;
	struct mutex     lock;       /* serialises offset updates */
};

void vfs_init(void);

/* Mount `ops` at `path`.  The first mount must be at "/". */
int  vfs_mount(const char *path, const struct vfs_ops *ops, void *fs_data,
               struct vfs_node *root);
int  vfs_unmount(const char *path);

/* Resolve a path to a node.  The returned node holds a reference the
   caller must release with vfs_node_put(). */
int  vfs_resolve(const char *path, struct vfs_node **out);
void vfs_node_get(struct vfs_node *node);
void vfs_node_put(struct vfs_node *node);

/* File-level interface, which is what the syscall layer uses. */
int  vfs_open(const char *path, uint32_t flags, uint32_t perm,
              struct vfs_file **out);
int  vfs_close(struct vfs_file *file);
ssize_t_k vfs_read(struct vfs_file *file, void *buf, size_t len);
ssize_t_k vfs_write(struct vfs_file *file, const void *buf, size_t len);
int64_t   vfs_seek(struct vfs_file *file, int64_t offset, int whence);

/* Positional forms, which do not touch the file offset. */
ssize_t_k vfs_pread(struct vfs_node *node, void *buf, size_t len,
                    haven_off_t offset);
ssize_t_k vfs_pwrite(struct vfs_node *node, const void *buf, size_t len,
                     haven_off_t offset);

int  vfs_stat_path(const char *path, struct vfs_stat *out);
int  vfs_mkdir(const char *path, uint32_t perm);
int  vfs_unlink(const char *path);
int  vfs_rename(const char *from, const char *to);
int  vfs_readdir_path(const char *path, uint32_t index,
                      struct vfs_dirent *out);
int  vfs_chmod(const char *path, uint32_t perm);
int  vfs_chown(const char *path, uid_t_k uid, gid_t_k gid);
int  vfs_truncate(const char *path, uint64_t length);
int  vfs_sync(void);

/* Convenience: read or write a whole small file in one call.  Most
   kernel users of the filesystem want this rather than open/read/close. */
ssize_t_k vfs_read_file(const char *path, void *buf, size_t bufsz);
ssize_t_k vfs_write_file(const char *path, const void *buf, size_t len);
bool      vfs_exists(const char *path);
bool      vfs_is_dir(const char *path);

/* Normalise a path: collapse "//", resolve "." and "..", and reject an
   attempt to escape above the root.  Exposed because it is worth testing
   directly -- a path-traversal bug here is a security hole, not just a
   correctness one. */
int vfs_normalise_path(const char *in, char *out, size_t outsz);

const char *vfs_node_type_name(enum vfs_node_type t);

struct vfs_stats {
	uint64_t opens;
	uint64_t closes;
	uint64_t reads;
	uint64_t writes;
	uint64_t bytes_read;
	uint64_t bytes_written;
	uint64_t lookups;
	uint64_t lookup_misses;
	uint32_t mounts;
	uint32_t open_files;
};

void vfs_get_stats(struct vfs_stats *out);
void vfs_dump(void);

#endif /* _HAVEN_VFS_H */
