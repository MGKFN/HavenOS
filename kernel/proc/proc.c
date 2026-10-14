/* SPDX-License-Identifier: MIT
 *
 * proc.c — processes, file-descriptor tables, and exec.
 */
#include <haven/proc.h>
#include <haven/elf.h>
#include <haven/sched.h>
#include <haven/mm.h>
#include <haven/arch.h>
#include <haven/vfs.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/panic.h>
#include <haven/printf.h>
#include <haven/spinlock.h>
#include <haven/time.h>
#include <haven/vmlayout.h>
#include <haven/syscall.h>

static LIST_HEAD(all_processes);
static DEFINE_SPINLOCK(proc_lock, "processes");
static pid_t_k next_pid = 1;
static struct proc_stats stats;

/* The largest executable the loader will read into memory at once.  A
   bound is necessary: without one, a 2 GiB file would be read into the
   kernel heap before a single check on its contents. */
#define EXEC_MAX_BYTES (16u * 1024 * 1024)

struct process *proc_current(void)
{
	struct thread *t = thread_current();
	return t ? t->proc : NULL;
}

pid_t_k proc_current_pid(void)
{
	struct process *p = proc_current();
	return p ? p->pid : 0;
}

struct process *proc_create(const char *name, struct process *parent)
{
	struct process *p = kzalloc(sizeof(*p));
	if (!p)
		return NULL;

	list_init(&p->threads);
	list_init(&p->children);
	list_init(&p->sibling);
	list_init(&p->wait_queue);
	list_init(&p->all_node);
	mutex_init(&p->fd_lock, "proc-fds");

	strlcpy(p->name, name ? name : "process", sizeof(p->name));
	p->state         = PROC_NEW;
	p->parent        = parent;
	p->ppid          = parent ? parent->pid : 0;
	p->uid           = parent ? parent->uid : 0;
	p->gid           = parent ? parent->gid : 0;
	p->start_time_ns = time_ns();
	p->mmap_next     = USER_MAX / 2;   /* mmap grows up from the middle,
	                                      the heap up from the image, so
	                                      they meet rather than collide
	                                      immediately */
	strlcpy(p->cwd, parent ? parent->cwd : "/", sizeof(p->cwd));

	irqflags_t f = spin_lock(&proc_lock);
	p->pid = next_pid++;
	list_add_tail(&p->all_node, &all_processes);
	if (parent)
		list_add_tail(&p->sibling, &parent->children);
	stats.created++;
	stats.live++;
	spin_unlock(&proc_lock, f);

	return p;
}

/* ── File descriptors ────────────────────────────────────────────────── */

int proc_fd_install(struct vfs_file *file)
{
	struct process *p = proc_current();
	if (!p)
		return -HAVEN_EINVAL;
	if (!file)
		return -HAVEN_EBADF;

	mutex_lock(&p->fd_lock);

	/* The lowest free descriptor, which is the documented behaviour
	   every program that does `close(1); open(...)` relies on. */
	for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
		if (!p->fds[fd]) {
			p->fds[fd] = file;
			mutex_unlock(&p->fd_lock);
			return fd;
		}
	}

	mutex_unlock(&p->fd_lock);
	return -HAVEN_EMFILE;
}

struct vfs_file *proc_fd_get(int fd)
{
	struct process *p = proc_current();
	if (!p || fd < 0 || fd >= PROC_MAX_FDS)
		return NULL;

	mutex_lock(&p->fd_lock);
	struct vfs_file *file = p->fds[fd];
	if (file) {
		/* Take a reference while the table lock is held, so a
		   concurrent close() on another thread cannot free the file
		   between the lookup and the use. */
		__atomic_fetch_add(&file->refcount, 1, __ATOMIC_RELAXED);
	}
	mutex_unlock(&p->fd_lock);

	return file;
}

void proc_fd_put(struct vfs_file *file)
{
	if (file)
		vfs_close(file);   /* drops the reference, frees at zero */
}

int proc_fd_close(int fd)
{
	struct process *p = proc_current();
	if (!p || fd < 0 || fd >= PROC_MAX_FDS)
		return -HAVEN_EBADF;

	mutex_lock(&p->fd_lock);
	struct vfs_file *file = p->fds[fd];
	p->fds[fd] = NULL;
	mutex_unlock(&p->fd_lock);

	if (!file)
		return -HAVEN_EBADF;

	return vfs_close(file);
}

int proc_fd_dup(int fd)
{
	struct process *p = proc_current();
	if (!p || fd < 0 || fd >= PROC_MAX_FDS)
		return -HAVEN_EBADF;

	mutex_lock(&p->fd_lock);

	struct vfs_file *file = p->fds[fd];
	if (!file) {
		mutex_unlock(&p->fd_lock);
		return -HAVEN_EBADF;
	}

	int newfd = -HAVEN_EMFILE;
	for (int i = 0; i < PROC_MAX_FDS; i++) {
		if (!p->fds[i]) {
			__atomic_fetch_add(&file->refcount, 1,
			                   __ATOMIC_RELAXED);
			p->fds[i] = file;
			newfd = i;
			break;
		}
	}

	mutex_unlock(&p->fd_lock);
	return newfd;
}

static void close_all_fds(struct process *p)
{
	mutex_lock(&p->fd_lock);
	for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
		struct vfs_file *file = p->fds[fd];
		p->fds[fd] = NULL;
		if (file) {
			mutex_unlock(&p->fd_lock);
			vfs_close(file);
			mutex_lock(&p->fd_lock);
		}
	}
	mutex_unlock(&p->fd_lock);
}

/* ── Working directory ───────────────────────────────────────────────── */

int proc_set_cwd(const char *path)
{
	struct process *p = proc_current();
	if (!p)
		return -HAVEN_EINVAL;

	char norm[VFS_PATH_MAX];
	int rc = vfs_normalise_path(path, norm, sizeof(norm));
	if (rc != 0)
		return rc;

	strlcpy(p->cwd, norm, sizeof(p->cwd));
	return 0;
}

int proc_get_cwd(char *out, size_t outsz)
{
	struct process *p = proc_current();
	if (!out || outsz == 0)
		return -HAVEN_EINVAL;

	if (!p) {
		/* A kernel thread has no working directory; the root is the
		   only honest answer. */
		strlcpy(out, "/", outsz);
		return 0;
	}

	if (strlcpy(out, p->cwd, outsz) >= outsz)
		return -HAVEN_ERANGE;
	return 0;
}

int proc_resolve_path(const char *in, char *out, size_t outsz)
{
	if (!in || !out)
		return -HAVEN_EINVAL;

	if (in[0] == '/')
		return vfs_normalise_path(in, out, outsz);

	/* Relative: join with the working directory, then normalise -- so
	   "../x" from "/a/b" becomes "/a/x" rather than being rejected. */
	char joined[VFS_PATH_MAX];
	struct process *p = proc_current();
	const char *base = p ? p->cwd : "/";

	size_t n = strlcpy(joined, base, sizeof(joined));
	if (n >= sizeof(joined))
		return -HAVEN_ENAMETOOLONG;
	if (n > 1 && strlcat(joined, "/", sizeof(joined)) >= sizeof(joined))
		return -HAVEN_ENAMETOOLONG;
	if (strlcat(joined, in, sizeof(joined)) >= sizeof(joined))
		return -HAVEN_ENAMETOOLONG;

	return vfs_normalise_path(joined, out, outsz);
}

/* ── Loading an executable ───────────────────────────────────────────── */

/*
 * Build the user stack: the argument and environment strings, then the
 * argv and envp pointer arrays, then argc -- laid out so that a program
 * entered with RSP pointing at it sees the System V convention.
 *
 * The strings are copied into the user stack rather than referenced,
 * because the kernel buffers they came from are freed when exec returns.
 */
static int build_user_stack(struct arch_pagetable *pt, vaddr_t stack_top,
                            const char *const argv[],
                            const char *const envp[],
                            vaddr_t *out_sp)
{
	/* Count and measure first, so the whole layout is known before
	   anything is written and an overlong argument list is rejected
	   rather than truncated. */
	size_t argc = 0, envc = 0, bytes = 0;

	for (; argv && argv[argc]; argc++) {
		if (argc >= PROC_MAX_ARGS)
			return -HAVEN_E2BIG;
		bytes += strlen(argv[argc]) + 1;
	}
	for (; envp && envp[envc]; envc++) {
		if (envc >= PROC_MAX_ARGS)
			return -HAVEN_E2BIG;
		bytes += strlen(envp[envc]) + 1;
	}

	size_t ptr_bytes = (argc + 1 + envc + 1) * sizeof(uint64_t)
	                 + sizeof(uint64_t);   /* argc itself */

	if (bytes + ptr_bytes > PROC_ARG_MAX)
		return -HAVEN_E2BIG;

	/* Work in a kernel buffer and copy once, which avoids repeatedly
	   translating user addresses through the not-yet-active page
	   table. */
	uint8_t *img = kzalloc(PROC_ARG_MAX);
	if (!img)
		return -HAVEN_ENOMEM;

	/* The stack image is built at its final virtual address, so the
	   pointers written into it are correct for the process. */
	size_t  total = ALIGN_UP(bytes + ptr_bytes, 16);
	vaddr_t base  = stack_top - total;

	uint8_t  *img_base   = img + (PROC_ARG_MAX - total);
	uint64_t *ptr_area   = (uint64_t *)img_base;
	uint8_t  *str_area   = img_base + ptr_bytes;
	vaddr_t   str_vaddr  = base + ptr_bytes;

	size_t pi = 0;
	ptr_area[pi++] = argc;

	size_t so = 0;
	for (size_t i = 0; i < argc; i++) {
		size_t l = strlen(argv[i]) + 1;
		memcpy(str_area + so, argv[i], l);
		ptr_area[pi++] = str_vaddr + so;
		so += l;
	}
	ptr_area[pi++] = 0;   /* argv terminator */

	for (size_t i = 0; i < envc; i++) {
		size_t l = strlen(envp[i]) + 1;
		memcpy(str_area + so, envp[i], l);
		ptr_area[pi++] = str_vaddr + so;
		so += l;
	}
	ptr_area[pi++] = 0;   /* envp terminator */

	/* Map the stack pages and copy the image in. */
	size_t pages = USER_STACK_PAGES;
	vaddr_t stack_lo = stack_top - pages * HAVEN_PAGE_SIZE;

	for (size_t i = 0; i < pages; i++) {
		paddr_t phys = pmm_alloc_page();
		if (!phys) {
			kfree(img);
			return -HAVEN_ENOMEM;
		}
		memset((void *)arch_phys_to_virt(phys), 0, HAVEN_PAGE_SIZE);

		/* The stack is never executable.  An executable stack is how
		   a buffer overflow becomes code execution, and nothing
		   legitimate needs it. */
		int rc = arch_pt_map(pt, stack_lo + i * HAVEN_PAGE_SIZE, phys,
		                     HAVEN_PAGE_SIZE,
		                     VM_PROT_READ | VM_PROT_WRITE |
		                     VM_PROT_USER);
		if (rc != 0) {
			pmm_free_page(phys);
			kfree(img);
			return rc;
		}
	}

	for (size_t done = 0; done < total; ) {
		vaddr_t  va    = base + done;
		vaddr_t  page  = ALIGN_DOWN(va, HAVEN_PAGE_SIZE);
		uint64_t in_pg = va - page;
		uint64_t chunk = HAVEN_PAGE_SIZE - in_pg;
		if (chunk > total - done)
			chunk = total - done;

		paddr_t phys = arch_pt_translate(pt, page);
		if (!phys) {
			kfree(img);
			return -HAVEN_EIO;
		}

		memcpy((uint8_t *)arch_phys_to_virt(phys) + in_pg,
		       img_base + done, (size_t)chunk);
		done += chunk;
	}

	kfree(img);
	*out_sp = base;
	return 0;
}

static void user_thread_entry(void *arg);

struct exec_handoff {
	struct process *proc;
	vaddr_t         entry;
	vaddr_t         stack;
};

int proc_load_elf(struct process *proc, const char *path,
                  const char *const argv[], const char *const envp[])
{
	if (!proc || !path)
		return -HAVEN_EINVAL;

	/* Read the executable.  Bounded: without a limit a huge file would
	   be read into the kernel heap before any of it is validated. */
	struct vfs_stat st;
	int rc = vfs_stat_path(path, &st);
	if (rc != 0)
		return rc;

	if (st.type != VFS_NODE_FILE)
		return -HAVEN_EACCES;
	if (st.size == 0)
		return -HAVEN_ENOEXEC;
	if (st.size > EXEC_MAX_BYTES) {
		kerr("exec: '%s' is %llu bytes, over the %u MiB limit",
		     path, (unsigned long long)st.size,
		     EXEC_MAX_BYTES / (1024 * 1024));
		return -HAVEN_E2BIG;
	}

	void *image = kmalloc((size_t)st.size);
	if (!image)
		return -HAVEN_ENOMEM;

	ssize_t_k got = vfs_read_file(path, image, (size_t)st.size);
	if (got < 0) {
		kfree(image);
		return (int)got;
	}
	if ((uint64_t)got != st.size) {
		/* A short read of an executable means the file changed or the
		   filesystem is damaged.  Loading a partial image would
		   execute whatever happened to be in the tail. */
		kerr("exec: '%s' read %lld of %llu bytes", path,
		     (long long)got, (unsigned long long)st.size);
		kfree(image);
		return -HAVEN_EIO;
	}

	struct arch_pagetable *pt = arch_pt_create();
	if (!pt) {
		kfree(image);
		return -HAVEN_ENOMEM;
	}

	struct elf_image info;
	rc = elf_load(pt, image, (size_t)got, &info);
	kfree(image);

	if (rc != 0) {
		arch_pt_destroy(pt);
		return rc;
	}

	vaddr_t sp = 0;
	rc = build_user_stack(pt, USER_STACK_TOP, argv, envp, &sp);
	if (rc != 0) {
		arch_pt_destroy(pt);
		return rc;
	}

	/* Commit.  Everything that could fail has already succeeded, so the
	   process's old address space is only discarded now. */
	struct arch_pagetable *old = proc->pagetable;

	proc->pagetable   = pt;
	proc->image_start = info.load_start;
	proc->image_end   = info.load_end;
	proc->heap_start  = ALIGN_UP(info.load_end, HAVEN_PAGE_SIZE);
	proc->heap_end    = proc->heap_start;
	proc->stack_top   = USER_STACK_TOP;
	proc->state       = PROC_RUNNING;
	strlcpy(proc->name, path, sizeof(proc->name));

	if (old)
		arch_pt_destroy(old);

	struct exec_handoff *h = kzalloc(sizeof(*h));
	if (!h)
		return -HAVEN_ENOMEM;
	h->proc  = proc;
	h->entry = info.entry;
	h->stack = sp;

	char tname[THREAD_NAME_MAX];
	snprintf(tname, sizeof(tname), "%s", proc->name);

	struct thread *t = thread_create(tname, user_thread_entry, h,
	                                 SCHED_PRIO_NORMAL);
	if (!t) {
		kfree(h);
		return -HAVEN_ENOMEM;
	}

	t->proc = proc;
	list_add_tail(&t->proc_node, &proc->threads);
	proc->thread_count++;

	kinfo("exec: pid %d '%s' entry 0x%llx stack 0x%llx",
	      proc->pid, proc->name,
	      (unsigned long long)info.entry, (unsigned long long)sp);

	return 0;
}

/*
 * The first thing a new user thread runs, in kernel mode.
 *
 * It switches to the process's address space, points the architecture at
 * the user entry point and stack, and never returns: arch_enter_user()
 * leaves ring 0 for good.
 */
static void user_thread_entry(void *arg)
{
	struct exec_handoff *h = arg;
	struct process      *p = h->proc;
	vaddr_t entry = h->entry;
	vaddr_t stack = h->stack;

	kfree(h);

	/* The TSS must name THIS thread's kernel stack before user mode is
	   entered, or the first trap from ring 3 pushes its frame onto
	   whatever stack was there before. */
	struct thread *self = thread_current();
	arch_set_kernel_stack(self->kstack_top);

	arch_pt_switch(p->pagetable);

	arch_enter_user(entry, stack);
}

int proc_spawn(const char *path, const char *const argv[],
               const char *const envp[], pid_t_k *out_pid)
{
	struct process *parent = proc_current();
	struct process *p = proc_create(path, parent);
	if (!p)
		return -HAVEN_ENOMEM;

	/* Inherit stdin, stdout and stderr from the parent, which is what
	   makes a spawned program's output appear where the parent's does.
	   A process with no descriptors 0-2 is one whose printf goes
	   nowhere, and that is a confusing way to start. */
	if (parent) {
		for (int fd = 0; fd < 3; fd++) {
			struct vfs_file *f = parent->fds[fd];
			if (!f)
				continue;
			__atomic_fetch_add(&f->refcount, 1, __ATOMIC_RELAXED);
			p->fds[fd] = f;
		}
	}

	int rc = proc_load_elf(p, path, argv, envp);
	if (rc != 0) {
		close_all_fds(p);

		irqflags_t f = spin_lock(&proc_lock);
		list_del(&p->all_node);
		if (parent)
			list_del(&p->sibling);
		if (stats.live)
			stats.live--;
		spin_unlock(&proc_lock, f);

		if (p->pagetable)
			arch_pt_destroy(p->pagetable);
		kfree(p);
		return rc;
	}

	if (out_pid)
		*out_pid = p->pid;
	return 0;
}

/* ── Exit and wait ───────────────────────────────────────────────────── */

void proc_exit(int code)
{
	struct process *p = proc_current();
	if (!p) {
		/* A kernel thread calling exit means the thread, not a
		   process. */
		thread_exit(code);
	}

	kinfo("proc: pid %d '%s' exiting with code %d", p->pid, p->name, code);

	close_all_fds(p);

	irqflags_t f = spin_lock(&proc_lock);
	p->exit_code = code;
	p->state     = PROC_ZOMBIE;
	stats.exited++;
	stats.zombies++;
	if (stats.live)
		stats.live--;

	/* Re-parent any children to pid 1, so an orphan is still reaped by
	   somebody.  Without this, a child whose parent exits first becomes
	   a permanent zombie. */
	struct process *child, *tmp;
	list_for_each_entry_safe(child, tmp, &p->children, sibling) {
		list_del(&child->sibling);
		child->parent = NULL;
		child->ppid   = 1;
	}
	spin_unlock(&proc_lock, f);

	/* The address space cannot be freed here: this code is executing on
	   a kernel stack that is mapped in it... in fact the kernel half is
	   shared, so it could be -- but the thread still has to run until
	   the switch, so leave it to whoever reaps. */
	if (p->parent)
		sched_wake_all(&p->parent->wait_queue);

	thread_exit(code);
}

bool proc_fault_kill(const char *reason, int signal)
{
	struct process *p = proc_current();
	if (!p)
		return false;

	kerr("proc: killing pid %d '%s': %s (signal %d)",
	     p->pid, p->name, reason ? reason : "fault", signal);

	/* Encode the signal in the exit status the way wait() expects, so a
	   parent can tell a crash from a clean exit with that code. */
	proc_exit(128 + signal);
}

int proc_wait(pid_t_k pid, int *status_out)
{
	struct process *self = proc_current();
	if (!self)
		return -HAVEN_ECHILD;

	for (;;) {
		irqflags_t f = spin_lock(&proc_lock);

		struct process *child, *tmp;
		bool any = false;

		list_for_each_entry_safe(child, tmp, &self->children, sibling) {
			if (pid > 0 && child->pid != pid)
				continue;
			any = true;

			if (child->state != PROC_ZOMBIE)
				continue;

			pid_t_k reaped = child->pid;
			int     code   = child->exit_code;

			list_del(&child->sibling);
			list_del(&child->all_node);
			child->state = PROC_DEAD;
			if (stats.zombies)
				stats.zombies--;
			spin_unlock(&proc_lock, f);

			if (child->pagetable)
				arch_pt_destroy(child->pagetable);
			kfree(child);

			if (status_out)
				*status_out = code;
			return reaped;
		}

		spin_unlock(&proc_lock, f);

		if (!any)
			return -HAVEN_ECHILD;

		/* A child exists but has not exited.  Block rather than spin:
		   polling here would burn a CPU for the lifetime of the
		   child. */
		sched_block_on(&self->wait_queue);
	}
}

int proc_kill(pid_t_k pid, int signal)
{
	irqflags_t f = spin_lock(&proc_lock);

	struct process *p;
	list_for_each_entry(p, &all_processes, all_node) {
		if (p->pid != pid)
			continue;
		if (p->state != PROC_RUNNING) {
			spin_unlock(&proc_lock, f);
			return -HAVEN_ESPIPE;
		}
		spin_unlock(&proc_lock, f);

		/* Signals are not yet delivered asynchronously; a kill
		   terminates.  Saying so is better than accepting the call
		   and doing nothing, which is what a caller would otherwise
		   have to discover. */
		kwarn("proc: signal %d to pid %d terminates it; asynchronous "
		      "signal delivery is not implemented", signal, pid);

		struct thread *t;
		list_for_each_entry(t, &p->threads, proc_node)
			sched_wake(t);

		return 0;
	}

	spin_unlock(&proc_lock, f);
	return -HAVEN_ENOENT;
}

/* ── Statistics ──────────────────────────────────────────────────────── */

void proc_get_stats(struct proc_stats *out)
{
	if (!out)
		return;
	irqflags_t f = spin_lock(&proc_lock);
	*out = stats;
	spin_unlock(&proc_lock, f);
}

void proc_dump(void)
{
	struct proc_stats s;
	proc_get_stats(&s);

	kinfo("proc: %u live, %u zombies, %llu created, %llu exited",
	      s.live, s.zombies,
	      (unsigned long long)s.created, (unsigned long long)s.exited);
	kinfo("proc:  pid ppid  threads  state     name");

	irqflags_t f = spin_lock(&proc_lock);
	struct process *p;
	list_for_each_entry(p, &all_processes, all_node) {
		const char *st = p->state == PROC_RUNNING ? "running"
		               : p->state == PROC_ZOMBIE  ? "zombie"
		               : p->state == PROC_NEW     ? "new"
		                                          : "dead";
		kinfo("proc: %4d %4d  %7u  %-9s %s",
		      p->pid, p->ppid, p->thread_count, st, p->name);
	}
	spin_unlock(&proc_lock, f);
}

void proc_init(void)
{
	list_init(&all_processes);
	spin_lock_init(&proc_lock, "processes");
	memset(&stats, 0, sizeof(stats));

	kinfo("proc: process table ready, %d descriptors per process",
	      PROC_MAX_FDS);
}

/* ── The process-related system calls ────────────────────────────────── */

int64_t sys_fork(const struct syscall_args *a)
{
	(void)a;
	/* fork() needs copy-on-write page tables to be worth having: a
	   version that eagerly copied every page of the parent would make
	   fork+exec -- the only thing most programs do with it -- copy an
	   address space it immediately discards.  proc_spawn() covers that
	   case today.  Saying -ENOSYS is honest; returning 0 or a fake pid
	   would make a caller believe it had a child. */
	return -HAVEN_ENOSYS;
}

int64_t sys_execve(const struct syscall_args *a)
{
	char path[VFS_PATH_MAX];
	int rc = strncpy_from_user(path, (const char *)a->arg[0],
	                           sizeof(path));
	if (rc < 0)
		return rc;

	struct process *p = proc_current();
	if (!p)
		return -HAVEN_EINVAL;

	/* argv/envp marshalling from user space is not yet implemented, so
	   the program is started with just its own name. */
	const char *argv[2] = { path, NULL };
	return proc_load_elf(p, path, argv, NULL);
}

int64_t sys_waitpid(const struct syscall_args *a)
{
	int status = 0;
	int rc = proc_wait((pid_t_k)a->arg[0], &status);
	if (rc < 0)
		return rc;

	if (a->arg[1]) {
		int crc = copy_to_user((void *)a->arg[1], &status,
		                       sizeof(status));
		if (crc != 0)
			return crc;
	}
	return rc;
}

int64_t sys_kill(const struct syscall_args *a)
{
	return proc_kill((pid_t_k)a->arg[0], (int)a->arg[1]);
}

int64_t sys_brk(const struct syscall_args *a)
{
	struct process *p = proc_current();
	if (!p)
		return -HAVEN_EINVAL;

	vaddr_t want = (vaddr_t)a->arg[0];

	/* brk(0) queries the current break, which is how a C library finds
	   out where the heap starts. */
	if (want == 0)
		return (int64_t)p->heap_end;

	if (want < p->heap_start)
		return -HAVEN_EINVAL;
	if (want >= p->stack_top - USER_STACK_PAGES * HAVEN_PAGE_SIZE) {
		/* Growing the heap into the stack would let one overwrite the
		   other with no fault in between. */
		return -HAVEN_ENOMEM;
	}

	vaddr_t old = ALIGN_UP(p->heap_end, HAVEN_PAGE_SIZE);
	vaddr_t new = ALIGN_UP(want, HAVEN_PAGE_SIZE);

	if (new > old) {
		for (vaddr_t va = old; va < new; va += HAVEN_PAGE_SIZE) {
			paddr_t phys = pmm_alloc_page();
			if (!phys) {
				p->heap_end = va;
				return -HAVEN_ENOMEM;
			}
			memset((void *)arch_phys_to_virt(phys), 0,
			       HAVEN_PAGE_SIZE);

			int rc = arch_pt_map(p->pagetable, va, phys,
			                     HAVEN_PAGE_SIZE,
			                     VM_PROT_READ | VM_PROT_WRITE |
			                     VM_PROT_USER);
			if (rc != 0) {
				pmm_free_page(phys);
				p->heap_end = va;
				return rc;
			}
		}
	} else if (new < old) {
		for (vaddr_t va = new; va < old; va += HAVEN_PAGE_SIZE) {
			paddr_t phys = arch_pt_translate(p->pagetable, va);
			arch_pt_unmap(p->pagetable, va, HAVEN_PAGE_SIZE);
			if (phys)
				pmm_free_page(phys);
		}
	}

	p->heap_end = want;
	return (int64_t)want;
}

int64_t sys_mmap(const struct syscall_args *a)
{
	struct process *p = proc_current();
	if (!p)
		return -HAVEN_EINVAL;

	size_t   len  = (size_t)a->arg[1];
	uint32_t prot = (uint32_t)a->arg[2];

	if (len == 0)
		return -HAVEN_EINVAL;
	len = ALIGN_UP(len, HAVEN_PAGE_SIZE);

	vaddr_t va = p->mmap_next;
	if (va + len >= p->stack_top - USER_STACK_PAGES * HAVEN_PAGE_SIZE)
		return -HAVEN_ENOMEM;

	uint32_t vmprot = VM_PROT_USER | VM_PROT_READ;
	if (prot & 0x2)
		vmprot |= VM_PROT_WRITE;
	if (prot & 0x4)
		vmprot |= VM_PROT_EXEC;

	for (size_t off = 0; off < len; off += HAVEN_PAGE_SIZE) {
		paddr_t phys = pmm_alloc_page();
		if (!phys) {
			/* Unwind what was mapped: leaving a partial mapping
			   behind would hand the caller an address range that
			   faults part way through. */
			for (size_t u = 0; u < off; u += HAVEN_PAGE_SIZE) {
				paddr_t q = arch_pt_translate(p->pagetable,
				                              va + u);
				arch_pt_unmap(p->pagetable, va + u,
				              HAVEN_PAGE_SIZE);
				if (q)
					pmm_free_page(q);
			}
			return -HAVEN_ENOMEM;
		}
		memset((void *)arch_phys_to_virt(phys), 0, HAVEN_PAGE_SIZE);

		int rc = arch_pt_map(p->pagetable, va + off, phys,
		                     HAVEN_PAGE_SIZE, vmprot);
		if (rc != 0) {
			pmm_free_page(phys);
			return rc;
		}
	}

	p->mmap_next = va + len + HAVEN_PAGE_SIZE;   /* guard page */
	return (int64_t)va;
}

int64_t sys_munmap(const struct syscall_args *a)
{
	struct process *p = proc_current();
	if (!p)
		return -HAVEN_EINVAL;

	vaddr_t va  = (vaddr_t)a->arg[0];
	size_t  len = ALIGN_UP((size_t)a->arg[1], HAVEN_PAGE_SIZE);

	if (!is_user_addr(va) || len == 0)
		return -HAVEN_EINVAL;

	for (size_t off = 0; off < len; off += HAVEN_PAGE_SIZE) {
		paddr_t phys = arch_pt_translate(p->pagetable, va + off);
		arch_pt_unmap(p->pagetable, va + off, HAVEN_PAGE_SIZE);
		if (phys)
			pmm_free_page(phys);
	}
	return 0;
}
