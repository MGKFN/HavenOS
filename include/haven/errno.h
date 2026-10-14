/* SPDX-License-Identifier: MIT
 *
 * errno.h — kernel error codes.
 *
 * HavenDOS carried two overlapping error vocabularies: HD_ERR_* in
 * errors.h and bare magic numbers (-1, -2, -8) returned from fs and
 * installer code, so a caller could not tell "not found" from "permission
 * denied" without reading the callee.  HavenOS uses one negative-errno
 * convention throughout: every kernel function that can fail returns 0 or
 * a positive count on success and -HAVEN_E* on failure.
 */
#ifndef _HAVEN_ERRNO_H
#define _HAVEN_ERRNO_H

#define HAVEN_EOK        0   /* success */
#define HAVEN_EPERM      1   /* operation not permitted */
#define HAVEN_ENOENT     2   /* no such file or directory */
#define HAVEN_EIO        5   /* I/O error */
#define HAVEN_ENXIO      6   /* no such device or address */
#define HAVEN_E2BIG      7   /* argument list too long */
#define HAVEN_ENOEXEC    8   /* exec format error */
#define HAVEN_EBADF      9   /* bad file descriptor */
#define HAVEN_ECHILD    10   /* no child processes */
#define HAVEN_EAGAIN    11   /* try again */
#define HAVEN_ENOMEM    12   /* out of memory */
#define HAVEN_EACCES    13   /* permission denied */
#define HAVEN_EFAULT    14   /* bad address */
#define HAVEN_EBUSY     16   /* device or resource busy */
#define HAVEN_EEXIST    17   /* file exists */
#define HAVEN_EXDEV     18   /* cross-device link */
#define HAVEN_ENODEV    19   /* no such device */
#define HAVEN_ENOTDIR   20   /* not a directory */
#define HAVEN_EISDIR    21   /* is a directory */
#define HAVEN_EINVAL    22   /* invalid argument */
#define HAVEN_ENFILE    23   /* file table overflow */
#define HAVEN_EMFILE    24   /* too many open files */
#define HAVEN_ENOSPC    28   /* no space left on device */
#define HAVEN_ESPIPE    29   /* illegal seek */
#define HAVEN_EROFS     30   /* read-only file system */
#define HAVEN_EMLINK    31   /* too many links */
#define HAVEN_ERANGE    34   /* result out of range */
#define HAVEN_ENAMETOOLONG 36 /* file name too long */
#define HAVEN_ENOSYS    38   /* function not implemented */
#define HAVEN_ENOTEMPTY 39   /* directory not empty */
#define HAVEN_ELOOP     40   /* too many symbolic links */
#define HAVEN_EOVERFLOW 75   /* value too large for data type */
#define HAVEN_ETIMEDOUT 110  /* connection/operation timed out */
#define HAVEN_ECORRUPT  117  /* structure needs cleaning / corrupt metadata */

/* Human-readable name for an error code.  Accepts either sign. */
const char *haven_strerror(int err);

#endif /* _HAVEN_ERRNO_H */
