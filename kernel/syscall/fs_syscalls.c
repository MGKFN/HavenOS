/* SPDX-License-Identifier: MIT
 *
 * fs_syscalls.c — the filesystem system calls.
 *
 * These override the weak -ENOSYS stubs in kernel/syscall/syscall.c
 * simply by being linked in, which is why the stubs exist: a build
 * without a filesystem still runs and reports honestly that these calls
 * are unavailable, rather than failing to link.
 *
 * EVERY POINTER HERE COMES FROM USER SPACE
 * None of these functions dereferences a user pointer.  Paths are copied
 * with strncpy_from_user(), which bounds the length and survives a fault;
 * buffers are copied in bounded chunks.  The alternative -- passing the
 * user pointer straight to vfs_read() -- would let a process read kernel
 * memory by passing a kernel address, and would fault the kernel on a
 * bad one.
 */
#include <haven/syscall.h>
#include <haven/vfs.h>
#include <haven/proc.h>
#include <haven/mm.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/sched.h>

/* How much is copied across the boundary per iteration.  A fixed bounce
   buffer rather than one sized by the request, so a process cannot make
   the kernel allocate an arbitrary amount by asking for a huge read. */
#define BOUNCE_SIZE 4096

static int copy_path_from_user(const char *user_path, char *out, size_t outsz)
{
	int rc = strncpy_from_user(out, user_path, outsz);
	if (rc < 0)
		return rc;
	if (out[0] == '\0')
		return -HAVEN_EINVAL;
	return 0;
}

int64_t sys_open(const struct syscall_args *a)
{
	char path[VFS_PATH_MAX];
	int rc = copy_path_from_user((const char *)a->arg[0], path,
	                             sizeof(path));
	if (rc != 0)
		return rc;

	uint32_t flags = (uint32_t)a->arg[1];
	uint32_t perm  = (uint32_t)a->arg[2];

	struct vfs_file *file = NULL;
	rc = vfs_open(path, flags, perm, &file);
	if (rc != 0)
		return rc;

	int fd = proc_fd_install(file);
	if (fd < 0) {
		vfs_close(file);
		return fd;
	}
	return fd;
}

int64_t sys_close(const struct syscall_args *a)
{
	return proc_fd_close((int)a->arg[0]);
}

int64_t sys_read(const struct syscall_args *a)
{
	int    fd   = (int)a->arg[0];
	void  *ubuf = (void *)a->arg[1];
	size_t len  = (size_t)a->arg[2];

	if (len == 0)
		return 0;

	struct vfs_file *file = proc_fd_get(fd);
	if (!file)
		return -HAVEN_EBADF;

	void *bounce = kmalloc(BOUNCE_SIZE);
	if (!bounce) {
		proc_fd_put(file);
		return -HAVEN_ENOMEM;
	}

	ssize_t_k total = 0;
	int       rc    = 0;

	while ((size_t)total < len) {
		size_t chunk = len - (size_t)total;
		if (chunk > BOUNCE_SIZE)
			chunk = BOUNCE_SIZE;

		ssize_t_k got = vfs_read(file, bounce, chunk);
		if (got < 0) {
			/* Report the error only if nothing was read yet.
			   Otherwise report the partial success: a caller that
			   got 2 KiB and then an error must know about the
			   2 KiB, or it will read them again. */
			if (total == 0)
				rc = (int)got;
			break;
		}
		if (got == 0)
			break;   /* end of file */

		rc = copy_to_user((uint8_t *)ubuf + total, bounce,
		                  (size_t)got);
		if (rc != 0) {
			if (total == 0) {
				kfree(bounce);
				proc_fd_put(file);
				return rc;
			}
			break;
		}

		total += got;
		if ((size_t)got < chunk)
			break;   /* a short read means no more is available */
	}

	kfree(bounce);
	proc_fd_put(file);

	return total > 0 ? total : rc;
}

int64_t sys_write(const struct syscall_args *a)
{
	int         fd   = (int)a->arg[0];
	const void *ubuf = (const void *)a->arg[1];
	size_t      len  = (size_t)a->arg[2];

	if (len == 0)
		return 0;

	struct vfs_file *file = proc_fd_get(fd);
	if (!file)
		return -HAVEN_EBADF;

	void *bounce = kmalloc(BOUNCE_SIZE);
	if (!bounce) {
		proc_fd_put(file);
		return -HAVEN_ENOMEM;
	}

	ssize_t_k total = 0;
	int       rc    = 0;

	while ((size_t)total < len) {
		size_t chunk = len - (size_t)total;
		if (chunk > BOUNCE_SIZE)
			chunk = BOUNCE_SIZE;

		rc = copy_from_user(bounce, (const uint8_t *)ubuf + total,
		                    chunk);
		if (rc != 0) {
			if (total == 0) {
				kfree(bounce);
				proc_fd_put(file);
				return rc;
			}
			break;
		}

		ssize_t_k put = vfs_write(file, bounce, chunk);
		if (put < 0) {
			if (total == 0)
				rc = (int)put;
			break;
		}

		total += put;
		if ((size_t)put < chunk)
			break;   /* the device or filesystem is full */
	}

	kfree(bounce);
	proc_fd_put(file);

	return total > 0 ? total : rc;
}

int64_t sys_lseek(const struct syscall_args *a)
{
	struct vfs_file *file = proc_fd_get((int)a->arg[0]);
	if (!file)
		return -HAVEN_EBADF;

	int64_t rc = vfs_seek(file, (int64_t)a->arg[1], (int)a->arg[2]);
	proc_fd_put(file);
	return rc;
}

int64_t sys_stat(const struct syscall_args *a)
{
	char path[VFS_PATH_MAX];
	int rc = copy_path_from_user((const char *)a->arg[0], path,
	                             sizeof(path));
	if (rc != 0)
		return rc;

	struct vfs_stat st;
	rc = vfs_stat_path(path, &st);
	if (rc != 0)
		return rc;

	if (!a->arg[1])
		return -HAVEN_EFAULT;
	return copy_to_user((void *)a->arg[1], &st, sizeof(st));
}

int64_t sys_mkdir(const struct syscall_args *a)
{
	char path[VFS_PATH_MAX];
	int rc = copy_path_from_user((const char *)a->arg[0], path,
	                             sizeof(path));
	if (rc != 0)
		return rc;

	return vfs_mkdir(path, (uint32_t)a->arg[1]);
}

int64_t sys_unlink(const struct syscall_args *a)
{
	char path[VFS_PATH_MAX];
	int rc = copy_path_from_user((const char *)a->arg[0], path,
	                             sizeof(path));
	if (rc != 0)
		return rc;

	return vfs_unlink(path);
}

int64_t sys_readdir(const struct syscall_args *a)
{
	char path[VFS_PATH_MAX];
	int rc = copy_path_from_user((const char *)a->arg[0], path,
	                             sizeof(path));
	if (rc != 0)
		return rc;

	struct vfs_dirent de;
	rc = vfs_readdir_path(path, (uint32_t)a->arg[1], &de);
	if (rc != 0)
		return rc;

	if (!a->arg[2])
		return -HAVEN_EFAULT;
	return copy_to_user((void *)a->arg[2], &de, sizeof(de));
}

int64_t sys_dup(const struct syscall_args *a)
{
	return proc_fd_dup((int)a->arg[0]);
}

int64_t sys_chdir(const struct syscall_args *a)
{
	char path[VFS_PATH_MAX];
	int rc = copy_path_from_user((const char *)a->arg[0], path,
	                             sizeof(path));
	if (rc != 0)
		return rc;

	/* The directory must exist and be a directory before the process's
	   working directory changes; setting it first and validating later
	   would leave a process with a cwd that cannot be resolved. */
	if (!vfs_exists(path))
		return -HAVEN_ENOENT;
	if (!vfs_is_dir(path))
		return -HAVEN_ENOTDIR;

	return proc_set_cwd(path);
}

int64_t sys_getcwd(const struct syscall_args *a)
{
	char   path[VFS_PATH_MAX];
	size_t ulen = (size_t)a->arg[1];

	int rc = proc_get_cwd(path, sizeof(path));
	if (rc != 0)
		return rc;

	size_t need = strlen(path) + 1;
	if (ulen < need)
		return -HAVEN_ERANGE;

	if (!a->arg[0])
		return -HAVEN_EFAULT;
	rc = copy_to_user((void *)a->arg[0], path, need);
	if (rc != 0)
		return rc;

	return (int64_t)need;
}
