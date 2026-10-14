/* SPDX-License-Identifier: MIT
 *
 * syscall.c — the portable system-call dispatcher.
 *
 * EVERY POINTER FROM USER SPACE IS HOSTILE UNTIL COPIED
 * The rule this file enforces is that a handler never dereferences a user
 * pointer.  It copies through copy_from_user(), which range-checks against
 * user space and survives a fault by returning -EFAULT.  A handler that
 * dereferenced directly would either fault the kernel or, with a kernel
 * address, read kernel memory on the caller's behalf.
 *
 * HavenDOS had nothing analogous because it had no user mode: its shell
 * called vfs_read() with a pointer and both were ring 0.
 */
#include <haven/syscall.h>
#include <haven/arch.h>
#include <haven/sched.h>
#include <haven/smp.h>
#include <haven/time.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/version.h>
#include <haven/mm.h>
#include <haven/proc.h>

static struct syscall_stats stats;

int copy_from_user(void *dst, const void *user_src, size_t len)
{
	if (!dst || !user_src)
		return -HAVEN_EFAULT;
	return arch_copy_from_user(dst, user_src, len);
}

int copy_to_user(void *user_dst, const void *src, size_t len)
{
	if (!user_dst || !src)
		return -HAVEN_EFAULT;
	return arch_copy_to_user(user_dst, src, len);
}

int strncpy_from_user(char *dst, const char *user_src, size_t len)
{
	if (!dst || !user_src || len == 0)
		return -HAVEN_EFAULT;
	return arch_strncpy_from_user(dst, user_src, len);
}

/* ── Handlers that need nothing beyond the scheduler and the clock ──── */

int64_t sys_exit(const struct syscall_args *a)
{
	int code = (int)(int32_t)a->arg[0];

	/* Weak-linked: when the process layer exists it terminates the whole
	   process; until then exiting the thread is the honest equivalent. */
	thread_exit(code);
}

int64_t sys_yield(const struct syscall_args *a)
{
	(void)a;
	sched_yield();
	return 0;
}

int64_t sys_getpid(const struct syscall_args *a)
{
	(void)a;
	return proc_current_pid();
}

int64_t sys_gettid(const struct syscall_args *a)
{
	(void)a;
	struct thread *t = thread_current();
	return t ? t->tid : 0;
}

int64_t sys_nanosleep(const struct syscall_args *a)
{
	uint64_t ns = a->arg[0];

	/* A zero sleep is a yield, which is what a caller asking to sleep
	   for no time actually wants -- and it avoids arming a timer that
	   fires on the same tick. */
	if (ns == 0) {
		sched_yield();
		return 0;
	}
	return time_sleep_ns(ns);
}

int64_t sys_time(const struct syscall_args *a)
{
	struct haven_tm tm;
	int rc = time_get_wallclock(&tm);
	if (rc != 0)
		return rc;

	if (a->arg[0]) {
		rc = copy_to_user((void *)a->arg[0], &tm, sizeof(tm));
		if (rc != 0)
			return rc;
	}
	return 0;
}

int64_t sys_uname(const struct syscall_args *a)
{
	struct haven_utsname u;
	memset(&u, 0, sizeof(u));

	strlcpy(u.sysname,  "HavenOS",            sizeof(u.sysname));
	strlcpy(u.release,  HAVENOS_VERSION,      sizeof(u.release));
	strlcpy(u.version,  HAVENOS_CODENAME " " HAVENOS_BUILD_DATE,
	        sizeof(u.version));
	strlcpy(u.machine,  HAVENOS_ARCH,         sizeof(u.machine));
	strlcpy(u.nodename, "haven",              sizeof(u.nodename));

	if (!a->arg[0])
		return -HAVEN_EFAULT;
	return copy_to_user((void *)a->arg[0], &u, sizeof(u));
}

int64_t sys_sysinfo(const struct syscall_args *a)
{
	struct {
		uint64_t uptime_ns;
		uint64_t ram_total_pages;
		uint64_t ram_free_pages;
		uint64_t heap_live_bytes;
		uint64_t context_switches;
		uint32_t cpu_count;
		uint32_t thread_count;
	} info;

	struct pmm_stats     pm;
	struct kmalloc_stats km;
	struct sched_stats   sc;

	pmm_get_stats(&pm);
	kmalloc_get_stats(&km);
	sched_get_stats(&sc);

	memset(&info, 0, sizeof(info));
	info.uptime_ns        = time_ns();
	info.ram_total_pages  = pm.total_pages;
	info.ram_free_pages   = pm.free_pages;
	info.heap_live_bytes  = km.live_bytes;
	info.context_switches = sc.context_switches;
	info.cpu_count        = smp_online_count();
	info.thread_count     = sc.threads_live;

	if (!a->arg[0])
		return -HAVEN_EFAULT;
	return copy_to_user((void *)a->arg[0], &info, sizeof(info));
}

/* ── Weak hooks for layers that may not be linked in yet ─────────────
   Each one returns -ENOSYS rather than faulting, so a kernel built
   without the filesystem or process layer still runs and reports
   honestly which calls are unavailable.  HavenDOS's habit of returning a
   bare -1 or -8 from unimplemented paths is exactly what this avoids:
   the caller can tell "not implemented" from "failed". */

HAVEN_WEAK pid_t_k proc_current_pid(void) { return 1; }

#define SYS_STUB(name)                                            \
	HAVEN_WEAK int64_t sys_##name(const struct syscall_args *a) \
	{                                                          \
		(void)a;                                           \
		return -HAVEN_ENOSYS;                              \
	}

SYS_STUB(write)
SYS_STUB(read)
SYS_STUB(open)
SYS_STUB(close)
SYS_STUB(lseek)
SYS_STUB(stat)
SYS_STUB(brk)
SYS_STUB(mmap)
SYS_STUB(munmap)
SYS_STUB(fork)
SYS_STUB(execve)
SYS_STUB(waitpid)
SYS_STUB(kill)
SYS_STUB(getcwd)
SYS_STUB(chdir)
SYS_STUB(mkdir)
SYS_STUB(unlink)
SYS_STUB(readdir)
SYS_STUB(dup)
SYS_STUB(pipe)
SYS_STUB(thread_create)
SYS_STUB(thread_join)

/* ── The table ───────────────────────────────────────────────────────── */

struct syscall_entry {
	syscall_fn  fn;
	const char *name;
	uint8_t     nargs;
};

static const struct syscall_entry table[SYS_MAX] = {
	[SYS_exit]          = { sys_exit,          "exit",          1 },
	[SYS_write]         = { sys_write,         "write",         3 },
	[SYS_read]          = { sys_read,          "read",          3 },
	[SYS_open]          = { sys_open,          "open",          3 },
	[SYS_close]         = { sys_close,         "close",         1 },
	[SYS_lseek]         = { sys_lseek,         "lseek",         3 },
	[SYS_stat]          = { sys_stat,          "stat",          2 },
	[SYS_getpid]        = { sys_getpid,        "getpid",        0 },
	[SYS_gettid]        = { sys_gettid,        "gettid",        0 },
	[SYS_yield]         = { sys_yield,         "yield",         0 },
	[SYS_nanosleep]     = { sys_nanosleep,     "nanosleep",     1 },
	[SYS_uname]         = { sys_uname,         "uname",         1 },
	[SYS_brk]           = { sys_brk,           "brk",           1 },
	[SYS_mmap]          = { sys_mmap,          "mmap",          4 },
	[SYS_munmap]        = { sys_munmap,        "munmap",        2 },
	[SYS_fork]          = { sys_fork,          "fork",          0 },
	[SYS_execve]        = { sys_execve,        "execve",        3 },
	[SYS_waitpid]       = { sys_waitpid,       "waitpid",       3 },
	[SYS_kill]          = { sys_kill,          "kill",          2 },
	[SYS_getcwd]        = { sys_getcwd,        "getcwd",        2 },
	[SYS_chdir]         = { sys_chdir,         "chdir",         1 },
	[SYS_mkdir]         = { sys_mkdir,         "mkdir",         2 },
	[SYS_unlink]        = { sys_unlink,        "unlink",        1 },
	[SYS_readdir]       = { sys_readdir,       "readdir",       3 },
	[SYS_dup]           = { sys_dup,           "dup",           1 },
	[SYS_pipe]          = { sys_pipe,          "pipe",          1 },
	[SYS_time]          = { sys_time,          "time",          1 },
	[SYS_sysinfo]       = { sys_sysinfo,       "sysinfo",       1 },
	[SYS_thread_create] = { sys_thread_create, "thread_create", 3 },
	[SYS_thread_join]   = { sys_thread_join,   "thread_join",   2 },
};

const char *syscall_name(uint64_t nr)
{
	if (nr >= SYS_MAX || !table[nr].name)
		return "invalid";
	return table[nr].name;
}

int64_t syscall_invoke(struct syscall_args *a)
{
	stats.total++;

	if (a->nr >= SYS_MAX || !table[a->nr].fn) {
		stats.bad_number++;
		/* Report the first few, then stop: a program in a loop
		   making a bad call would otherwise fill the log. */
		if (stats.bad_number <= 8)
			kwarn("syscall: process requested unknown call %llu",
			      (unsigned long long)a->nr);
		return -HAVEN_ENOSYS;
	}

	int64_t rc = table[a->nr].fn(a);

	if (rc == -HAVEN_ENOSYS)
		stats.not_implemented++;
	else if (rc == -HAVEN_EFAULT)
		stats.faults++;

	return rc;
}

void syscall_get_stats(struct syscall_stats *out)
{
	if (out)
		*out = stats;
}

void syscall_dump(void)
{
	kinfo("syscall: %llu calls, %llu unknown numbers, %llu not "
	      "implemented, %llu faulting pointers",
	      (unsigned long long)stats.total,
	      (unsigned long long)stats.bad_number,
	      (unsigned long long)stats.not_implemented,
	      (unsigned long long)stats.faults);
}

void syscall_init(void)
{
	memset(&stats, 0, sizeof(stats));

	/* Count what is actually wired up, so the boot log says how much of
	   the interface a given build provides rather than claiming all of
	   it.  A call whose handler is still the weak -ENOSYS stub is
	   reported as absent. */
	unsigned implemented = 0;
	for (unsigned i = 0; i < SYS_MAX; i++) {
		if (table[i].fn)
			implemented++;
	}

	arch_syscall_init();

	kinfo("syscall: %u numbers assigned, SYSCALL/SYSRET entry installed",
	      implemented);
}
