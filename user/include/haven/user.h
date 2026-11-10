/* SPDX-License-Identifier: MIT
 *
 * user.h -- the system-call interface as a user program sees it.
 *
 * This is deliberately not a C library.  It is the thinnest possible
 * layer over the kernel ABI: one inline function per system call, each
 * returning the kernel's result unchanged (>= 0 on success, -errno on
 * failure).  A libc with errno, stdio and malloc is built on top of this,
 * not in place of it -- see the roadmap.
 */
#ifndef _HAVEN_USER_H
#define _HAVEN_USER_H

#include <haven/abi/syscall_nr.h>

#if defined(__x86_64__)
#include "../../lib/x86_64/syscall.h"
#else
#error "no system-call sequence for this architecture yet"
#endif

typedef unsigned long  size_t;
typedef long           ssize_t;

/* Values shared with the kernel's <haven/vfs.h> and <haven/errno.h>.
   They are restated rather than included because those headers declare
   kernel-internal types; they move to include/haven/abi/ with the libc. */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

#define EBADF     9
#define EFAULT   14
#define ENOSYS   38

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define __sc(nr, a, b, c, d) \
	__syscall6((nr), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

static inline void    sys_exit(int code)        { __sc(SYS_exit, code, 0, 0, 0); for (;;) ; }
static inline ssize_t sys_write(int fd, const void *buf, size_t n) { return __sc(SYS_write, fd, buf, n, 0); }
static inline ssize_t sys_read(int fd, void *buf, size_t n)        { return __sc(SYS_read, fd, buf, n, 0); }
static inline int     sys_open(const char *path, int flags, int mode) { return (int)__sc(SYS_open, path, flags, mode, 0); }
static inline int     sys_close(int fd)         { return (int)__sc(SYS_close, fd, 0, 0, 0); }
static inline int     sys_unlink(const char *p) { return (int)__sc(SYS_unlink, p, 0, 0, 0); }
static inline int     sys_getpid(void)          { return (int)__sc(SYS_getpid, 0, 0, 0, 0); }
static inline int     sys_yield(void)           { return (int)__sc(SYS_yield, 0, 0, 0, 0); }
static inline int     sys_nanosleep(unsigned long ns) { return (int)__sc(SYS_nanosleep, ns, 0, 0, 0); }
static inline long    sys_brk(unsigned long to) { return __sc(SYS_brk, to, 0, 0, 0); }
static inline long    sys_mmap(void *hint, size_t len, int prot, int flags) { return __sc(SYS_mmap, hint, len, prot, flags); }

#endif /* _HAVEN_USER_H */
