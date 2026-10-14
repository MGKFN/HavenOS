/* SPDX-License-Identifier: MIT
 *
 * proc.h — processes: an address space, a file-descriptor table, and the
 * threads that run inside them.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had no processes.  Its "users" were entries in a login table
 * with djb2-hashed passwords, and a logged-in user's programs were
 * functions compiled into the kernel: shell.c, desktop.c, tetris.c all
 * ran in ring 0 with full access to everything.  Its own setup_user_home()
 * created HOME/<username>/ in the RAM filesystem, which was the extent of
 * the isolation -- a convention, not a boundary.
 *
 * A HavenOS process is an enforced boundary:
 *   - its own page tables, so it cannot see another process's memory;
 *   - ring 3, so it cannot execute a privileged instruction;
 *   - its own file-descriptor table, so it cannot reach another
 *     process's open files;
 *   - a uid and gid the kernel checks, rather than an admin_only flag
 *     the filesystem consults.
 *
 * THREADS BELONG TO PROCESSES
 * A process is the unit of isolation; a thread is the unit of scheduling.
 * They are separate structures because several threads share one address
 * space, which is what makes a multi-threaded program possible without
 * duplicating page tables.
 */
#ifndef _HAVEN_PROC_H
#define _HAVEN_PROC_H

#include <haven/types.h>
#include <haven/list.h>
#include <haven/mutex.h>
#include <haven/vfs.h>

#define PROC_NAME_MAX    64
#define PROC_MAX_FDS     64
#define PROC_MAX_ARGS    32
#define PROC_ARG_MAX     4096

struct arch_pagetable;
struct thread;

enum proc_state {
	PROC_NEW = 0,
	PROC_RUNNING,
	PROC_ZOMBIE,     /* exited; status not yet collected by its parent */
	PROC_DEAD,
};

struct process {
	pid_t_k          pid;
	pid_t_k          ppid;
	char             name[PROC_NAME_MAX];
	enum proc_state  state;
	int              exit_code;

	/* The address space.  NULL for a kernel-only process, which is how
	   the init process starts before it execs anything. */
	struct arch_pagetable *pagetable;

	/* The user half of the address space, tracked so exit can free it
	   and so brk/mmap know where they may place things. */
	vaddr_t          image_start;
	vaddr_t          image_end;
	vaddr_t          heap_start;
	vaddr_t          heap_end;     /* the brk */
	vaddr_t          mmap_next;    /* bump pointer for mmap */
	vaddr_t          stack_top;

	/* Credentials.  Checked by the kernel, not by the filesystem. */
	uid_t_k          uid;
	gid_t_k          gid;

	/* Open files.  Per process, so one process closing a descriptor
	   cannot affect another -- which a global table could not
	   guarantee. */
	struct vfs_file *fds[PROC_MAX_FDS];
	struct mutex     fd_lock;

	char             cwd[VFS_PATH_MAX];

	/* Threads in this process, and the process tree. */
	struct list_head threads;
	struct list_head children;
	struct list_head sibling;
	struct process  *parent;

	/* Threads waiting in waitpid() for a child of this process. */
	struct list_head wait_queue;

	struct list_head all_node;     /* the global process list */
	uint32_t         thread_count;
	nstime_t         start_time_ns;
};

void proc_init(void);

/* The process the running thread belongs to, or NULL for a pure kernel
   thread. */
struct process *proc_current(void);
pid_t_k         proc_current_pid(void);

/* Create a process with an empty address space.  It has no threads yet. */
struct process *proc_create(const char *name, struct process *parent);

/* Load an ELF64 executable from `path` into `proc` and create its first
   thread.  This is execve() without the argument marshalling. */
int proc_load_elf(struct process *proc, const char *path,
                  const char *const argv[], const char *const envp[]);

/* Spawn a process from an executable in one call: create, load, start. */
int proc_spawn(const char *path, const char *const argv[],
               const char *const envp[], pid_t_k *out_pid);

void proc_exit(int code) HAVEN_NORETURN;
int  proc_wait(pid_t_k pid, int *status_out);
int  proc_kill(pid_t_k pid, int signal);

/* Terminate the current process because of an unrecoverable fault.
   Returns false when there is no current process, which tells the
   architecture's fault handler to treat the fault as fatal. */
bool proc_fault_kill(const char *reason, int signal);

/* ── File descriptors ────────────────────────────────────────────────── */

/* Install `file` in the lowest free descriptor.  Returns the descriptor
   or -HAVEN_EMFILE.  Takes ownership on success. */
int  proc_fd_install(struct vfs_file *file);

/* Look up a descriptor, taking a reference.  Must be released with
   proc_fd_put(), which is what stops a close() on another thread from
   freeing the file mid-read. */
struct vfs_file *proc_fd_get(int fd);
void             proc_fd_put(struct vfs_file *file);

int  proc_fd_close(int fd);
int  proc_fd_dup(int fd);

int  proc_set_cwd(const char *path);
int  proc_get_cwd(char *out, size_t outsz);

/* Resolve a possibly-relative path against the process's cwd. */
int  proc_resolve_path(const char *in, char *out, size_t outsz);

struct proc_stats {
	uint32_t live;
	uint32_t zombies;
	uint64_t created;
	uint64_t exited;
};

void proc_get_stats(struct proc_stats *out);
void proc_dump(void);

#endif /* _HAVEN_PROC_H */
