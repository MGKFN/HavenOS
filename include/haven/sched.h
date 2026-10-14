/* SPDX-License-Identifier: MIT
 *
 * sched.h — threads and the scheduler.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had no scheduler and no threads.  kernel_main() called
 * desktop_run() or shell_run(), and those functions ran until the machine
 * was reset.  Everything that needed to happen concurrently was woven into
 * one event loop by hand: the shell polled the keyboard, the network stack
 * was driven by whoever happened to call it, and a slow disk read stopped
 * the entire system -- its own heap.c said so plainly, "Thread safety: not
 * applicable (HavenDOS is single-threaded)".
 *
 * That is the single largest architectural difference in HavenOS, and
 * everything else in the system is shaped by it: the per-CPU run queues,
 * the spinlocks, the reentrant string functions, the blocking sleep.
 *
 * WHAT THE SCHEDULER IS
 * A fixed-priority round-robin scheduler with per-CPU run queues and
 * priority bands 0 (highest) to 7 (idle).  Within a band, threads run in
 * turn for one time slice.  A runnable thread in a higher band always
 * preempts a lower one.
 *
 * WHY NOT SOMETHING CLEVERER
 * A fair-share scheduler (CFS-style, with a virtual-time red-black tree)
 * is better for a general-purpose workload, and HavenOS will want one.
 * But a priority round-robin is small enough to be read in one sitting
 * and to be reasoned about when debugging something else, and getting
 * preemption, context switching, blocking and wake-up correct is the hard
 * part -- the queue discipline can be replaced later without touching any
 * of it.  See docs/DESIGN_DECISIONS.md#scheduler.
 */
#ifndef _HAVEN_SCHED_H
#define _HAVEN_SCHED_H

#include <haven/types.h>
#include <haven/list.h>
#include <haven/smp.h>

struct arch_context;
struct process;

enum thread_state {
	THREAD_NEW = 0,    /* created, never run */
	THREAD_READY,      /* on a run queue */
	THREAD_RUNNING,    /* on a CPU now */
	THREAD_BLOCKED,    /* on a wait queue, waiting for an event */
	THREAD_SLEEPING,   /* waiting for a time to arrive */
	THREAD_ZOMBIE,     /* finished; exit status not yet collected */
	THREAD_DEAD,       /* reaped */
};

#define THREAD_NAME_MAX 32

/* A thread's time slice, in ticks.  Four ticks at 100 Hz is 40 ms: long
   enough that the switch cost is lost in the noise, short enough that
   eight runnable threads all get a turn within a third of a second. */
#define SCHED_SLICE_TICKS 4

struct thread {
	/* The context must come first.  arch_context_switch() is handed a
	   pointer to it, and putting it at offset 0 means the assembly can
	   use the thread pointer directly -- one fewer address calculation
	   on the hottest path in the kernel. */
	struct arch_context *context;

	tid_t              tid;
	char               name[THREAD_NAME_MAX];
	enum thread_state  state;

	uint8_t            priority;        /* 0..SCHED_PRIO_LEVELS-1 */
	uint8_t            base_priority;   /* restored after a boost */
	int32_t            slice_remaining; /* ticks left in this turn */

	vaddr_t            kstack_top;      /* for freeing, and for the TSS */
	struct process    *proc;            /* NULL for a kernel thread */

	/* Queue membership.  A thread is on exactly one of: a run queue, a
	   wait queue, or nothing.  `proc_node` is a second list entirely --
	   its process's thread list -- which is why these are intrusive
	   lists and not an array index. */
	struct list_head   queue_node;
	struct list_head   proc_node;

	/* Threads blocked in thread_join() on this thread.  A per-thread
	   wait queue rather than a single waiter pointer, because more than
	   one thread may legitimately join the same thread. */
	struct list_head   join_waiters;

	/* Wake-up bookkeeping. */
	nstime_t           wake_at;         /* for THREAD_SLEEPING */
	struct list_head  *wait_queue;      /* for THREAD_BLOCKED */
	bool               wake_pending;    /* woken before it blocked */

	/* Accounting, for `ps`. */
	nstime_t           cpu_time_ns;
	nstime_t           last_scheduled;
	uint64_t           switch_count;
	uint32_t           home_cpu;

	int                exit_code;
};

void sched_init(void);

/* Create a kernel thread.  It starts READY, not running. */
struct thread *thread_create(const char *name, void (*entry)(void *),
                             void *arg, uint8_t priority);

/* The running thread on this CPU.  Never NULL once sched_init() has run:
   a CPU with nothing to do runs its idle thread. */
struct thread *thread_current(void);

/* Give up the rest of this slice. */
void sched_yield(void);

/* Pick and switch to the next thread.  Safe to call with a lock held
   only if that lock is released first -- holding a spinlock across a
   context switch deadlocks the moment the next thread wants it. */
void sched_switch(void);

/* Called once per tick from the timer interrupt.  It only RECORDS that a
   switch is due; the switch itself cannot happen in interrupt context
   (see rule 1 in kernel/sched/sched.c). */
void sched_tick(void);

/* Perform a switch the tick asked for, from a context where switching is
   safe.  Called from the idle loop and from the syscall return path. */
void sched_preempt_if_needed(void);

/* Make a thread runnable.  Safe from interrupt context. */
void sched_wake(struct thread *t);

/* Block the current thread on `queue` until something wakes it. */
void sched_block_on(struct list_head *queue);

/* Wake one / all threads on `queue`. */
void sched_wake_one(struct list_head *queue);
void sched_wake_all(struct list_head *queue);

/* Terminate the current thread with `code`.  Never returns. */
void thread_exit(int code) HAVEN_NORETURN;

/* Block until `t` has exited; returns its exit code. */
int  thread_join(struct thread *t);

void thread_set_priority(struct thread *t, uint8_t priority);
const char *thread_state_name(enum thread_state s);

struct sched_stats {
	uint64_t context_switches;
	uint64_t ticks;
	uint64_t preemptions;
	uint64_t voluntary_yields;
	uint64_t threads_created;
	uint64_t threads_exited;
	uint32_t threads_live;
	uint32_t threads_runnable;
};

void sched_get_stats(struct sched_stats *out);
void sched_dump(void);

/* Is the scheduler running?  Before it is, sleeping is a busy-wait and
   blocking is a bug -- so code that can run in both phases has to be
   able to ask. */
bool sched_active(void);

#endif /* _HAVEN_SCHED_H */
