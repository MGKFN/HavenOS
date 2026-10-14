/* SPDX-License-Identifier: MIT
 *
 * syscall.h — the system-call interface.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had no system calls, because it had no privilege boundary to
 * cross: its shell, its desktop and its filesystem code all ran in ring 0
 * and called each other directly.  That is why "secure user and kernel
 * separation" is a HavenOS goal rather than a HavenDOS feature.
 *
 * THE NUMBERS ARE AN ABI AND DO NOT MOVE
 * A compiled program contains the syscall number, not the name.  Renumbering
 * breaks every binary ever built for HavenOS.  So numbers are assigned once,
 * gaps left by removed calls are never reused, and new calls are appended.
 *
 * CALLING CONVENTION (x86_64)
 *   rax  syscall number
 *   rdi, rsi, rdx, r10, r8, r9   arguments 1-6
 *   rax  return value: >= 0 on success, -errno on failure
 *
 * r10 replaces rcx for the fourth argument because the SYSCALL instruction
 * itself overwrites rcx with the return address.  This matches the Linux
 * convention, which means a toolchain targeting HavenOS needs no unusual
 * calling-convention support.
 */
#ifndef _HAVEN_SYSCALL_H
#define _HAVEN_SYSCALL_H

#include <haven/types.h>

/* ── Syscall numbers.  Append only. ──────────────────────────────────── */
#define SYS_exit          0
#define SYS_write         1
#define SYS_read          2
#define SYS_open          3
#define SYS_close         4
#define SYS_lseek         5
#define SYS_stat          6
#define SYS_getpid        7
#define SYS_gettid        8
#define SYS_yield         9
#define SYS_nanosleep    10
#define SYS_uname        11
#define SYS_brk          12
#define SYS_mmap         13
#define SYS_munmap       14
#define SYS_fork         15
#define SYS_execve       16
#define SYS_waitpid      17
#define SYS_kill         18
#define SYS_getcwd       19
#define SYS_chdir        20
#define SYS_mkdir        21
#define SYS_unlink       22
#define SYS_readdir      23
#define SYS_dup          24
#define SYS_pipe         25
#define SYS_time         26
#define SYS_sysinfo      27
#define SYS_thread_create 28
#define SYS_thread_join  29

#define SYS_MAX          30

#define SYSCALL_ARG_MAX   6

/* What a handler receives.  Passing a struct rather than six loose
   arguments means the dispatcher can validate and log uniformly, and a
   handler that wants fewer arguments does not have to declare six. */
struct syscall_args {
	uint64_t nr;
	uint64_t arg[SYSCALL_ARG_MAX];
};

typedef int64_t (*syscall_fn)(const struct syscall_args *a);

void    syscall_init(void);

/* The portable half of the dispatcher: validate the number, look up the
   handler, call it, and return the result.  The architecture's entry
   path unpacks its own register layout into struct syscall_args and
   calls this. */
int64_t syscall_invoke(struct syscall_args *a);

const char *syscall_name(uint64_t nr);

struct syscall_stats {
	uint64_t total;
	uint64_t bad_number;
	uint64_t not_implemented;
	uint64_t faults;
};

void syscall_get_stats(struct syscall_stats *out);
void syscall_dump(void);

/* Copying across the boundary.  These wrap the architecture's
   fault-tolerant primitives and are what every handler must use instead
   of dereferencing a user pointer directly. */
int copy_from_user(void *dst, const void *user_src, size_t len);
int copy_to_user(void *user_dst, const void *src, size_t len);
int strncpy_from_user(char *dst, const char *user_src, size_t len);

/* What SYS_uname fills in. */
/* Every handler is declared here, so the weak -ENOSYS stub in
 * kernel/syscall/syscall.c and the real implementation in whichever
 * subsystem owns the call are both checked against ONE prototype.  Without
 * that, a real handler with a mismatched signature would override the stub
 * at link time and be called through the wrong ABI -- a bug that produces
 * garbage arguments and no diagnostic.
 */
#define SYSCALL_DECLARE(name) \
	int64_t sys_##name(const struct syscall_args *a)

SYSCALL_DECLARE(exit);
SYSCALL_DECLARE(write);
SYSCALL_DECLARE(read);
SYSCALL_DECLARE(open);
SYSCALL_DECLARE(close);
SYSCALL_DECLARE(lseek);
SYSCALL_DECLARE(stat);
SYSCALL_DECLARE(getpid);
SYSCALL_DECLARE(gettid);
SYSCALL_DECLARE(yield);
SYSCALL_DECLARE(nanosleep);
SYSCALL_DECLARE(uname);
SYSCALL_DECLARE(brk);
SYSCALL_DECLARE(mmap);
SYSCALL_DECLARE(munmap);
SYSCALL_DECLARE(fork);
SYSCALL_DECLARE(execve);
SYSCALL_DECLARE(waitpid);
SYSCALL_DECLARE(kill);
SYSCALL_DECLARE(getcwd);
SYSCALL_DECLARE(chdir);
SYSCALL_DECLARE(mkdir);
SYSCALL_DECLARE(unlink);
SYSCALL_DECLARE(readdir);
SYSCALL_DECLARE(dup);
SYSCALL_DECLARE(pipe);
SYSCALL_DECLARE(time);
SYSCALL_DECLARE(sysinfo);
SYSCALL_DECLARE(thread_create);
SYSCALL_DECLARE(thread_join);

/* proc_current_pid() is declared in <haven/proc.h>.  It is weakly
   defined in kernel/syscall/syscall.c so the dispatcher links before the
   process layer exists; the real definition in kernel/proc/proc.c wins
   when that file is present. */

struct haven_utsname {
	char sysname[32];
	char release[32];
	char version[64];
	char machine[32];
	char nodename[32];
};

#endif /* _HAVEN_SYSCALL_H */
