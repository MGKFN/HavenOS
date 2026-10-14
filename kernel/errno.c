/* SPDX-License-Identifier: MIT
 *
 * errno.c — error-code names, so logs say ENOENT and not -2.
 */
#include <haven/errno.h>
#include <haven/types.h>

struct err_name {
	int         code;
	const char *name;
};

static const struct err_name names[] = {
	{ HAVEN_EOK,          "EOK (success)" },
	{ HAVEN_EPERM,        "EPERM (operation not permitted)" },
	{ HAVEN_ENOENT,       "ENOENT (no such file or directory)" },
	{ HAVEN_EIO,          "EIO (I/O error)" },
	{ HAVEN_ENXIO,        "ENXIO (no such device or address)" },
	{ HAVEN_E2BIG,        "E2BIG (argument list too long)" },
	{ HAVEN_ENOEXEC,      "ENOEXEC (exec format error)" },
	{ HAVEN_EBADF,        "EBADF (bad file descriptor)" },
	{ HAVEN_ECHILD,       "ECHILD (no child processes)" },
	{ HAVEN_EAGAIN,       "EAGAIN (try again)" },
	{ HAVEN_ENOMEM,       "ENOMEM (out of memory)" },
	{ HAVEN_EACCES,       "EACCES (permission denied)" },
	{ HAVEN_EFAULT,       "EFAULT (bad address)" },
	{ HAVEN_EBUSY,        "EBUSY (device or resource busy)" },
	{ HAVEN_EEXIST,       "EEXIST (file exists)" },
	{ HAVEN_EXDEV,        "EXDEV (cross-device link)" },
	{ HAVEN_ENODEV,       "ENODEV (no such device)" },
	{ HAVEN_ENOTDIR,      "ENOTDIR (not a directory)" },
	{ HAVEN_EISDIR,       "EISDIR (is a directory)" },
	{ HAVEN_EINVAL,       "EINVAL (invalid argument)" },
	{ HAVEN_ENFILE,       "ENFILE (file table overflow)" },
	{ HAVEN_EMFILE,       "EMFILE (too many open files)" },
	{ HAVEN_ENOSPC,       "ENOSPC (no space left on device)" },
	{ HAVEN_ESPIPE,       "ESPIPE (illegal seek)" },
	{ HAVEN_EROFS,        "EROFS (read-only file system)" },
	{ HAVEN_EMLINK,       "EMLINK (too many links)" },
	{ HAVEN_ERANGE,       "ERANGE (result out of range)" },
	{ HAVEN_ENAMETOOLONG, "ENAMETOOLONG (file name too long)" },
	{ HAVEN_ENOSYS,       "ENOSYS (function not implemented)" },
	{ HAVEN_ENOTEMPTY,    "ENOTEMPTY (directory not empty)" },
	{ HAVEN_ELOOP,        "ELOOP (too many symbolic links)" },
	{ HAVEN_EOVERFLOW,    "EOVERFLOW (value too large)" },
	{ HAVEN_ETIMEDOUT,    "ETIMEDOUT (operation timed out)" },
	{ HAVEN_ECORRUPT,     "ECORRUPT (corrupt metadata)" },
};

const char *haven_strerror(int err)
{
	/* Accept either sign: callers hold -HAVEN_ENOENT, humans type 2. */
	int code = err < 0 ? -err : err;

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if (names[i].code == code)
			return names[i].name;
	}
	return "unknown error";
}
