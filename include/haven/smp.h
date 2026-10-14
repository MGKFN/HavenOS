/* SPDX-License-Identifier: MIT
 *
 * smp.h — per-CPU state and multi-processor support.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS was single-threaded and single-CPU by construction: its heap
 * said so explicitly ("Thread safety: not applicable"), its strtok() kept
 * a file-static cursor, and its IRQ table was one global array.  Every one
 * of those is a data race the moment a second CPU exists.
 *
 * HavenOS puts anything a CPU owns privately into struct haven_cpu,
 * reached through this_cpu().  The rule is: if two CPUs would contend for
 * it and they do not have to, it belongs here; if they must share it, it
 * gets a spinlock.  There is no third category.
 */
#ifndef _HAVEN_SMP_H
#define _HAVEN_SMP_H

#include <haven/types.h>
#include <haven/list.h>

#define HAVEN_MAX_CPUS 64

struct thread;
struct arch_pagetable;

/* Scheduler priority bands.  See kernel/sched/sched.c. */
#define SCHED_PRIO_LEVELS 8
#define SCHED_PRIO_IDLE   (SCHED_PRIO_LEVELS - 1)
#define SCHED_PRIO_NORMAL 4
#define SCHED_PRIO_HIGH   1

struct haven_cpu {
	/* Identity.  cpu_id is a dense logical index 0..ncpus-1; the
	   hardware APIC ID is sparse and must not be used as an array
	   index, which is a mistake that costs a boot on any machine with
	   a non-contiguous APIC numbering. */
	uint32_t cpu_id;
	uint32_t hw_id;

	/* Currently running and idle threads. */
	struct thread *current;
	struct thread *idle;

	/* Ring-0 stack this CPU's interrupt and syscall entry paths use. */
	vaddr_t  kernel_stack_top;

	/* Run queues, one list per priority band.  Per-CPU so that the
	   common case — picking the next thread — touches no shared state
	   and needs no cross-CPU lock. */
	struct list_head runqueue[SCHED_PRIO_LEVELS];
	uint32_t         runnable;         /* bitmask of non-empty bands */
	uint32_t         nr_runnable;

	/* Nesting depths.  preempt_count > 0 means "do not reschedule here";
	   irq_nesting > 0 means "we are in interrupt context".  Tracking
	   both is what lets the scheduler assert it is never entered from a
	   context that cannot survive a context switch. */
	uint32_t preempt_count;
	uint32_t irq_nesting;

	/* Statistics, for `cpuinfo`. */
	uint64_t ticks;
	uint64_t context_switches;
	uint64_t idle_ns;

	bool online;
	bool is_bsp;
};

/* The per-CPU block for the running CPU.  Cheap: one segment-relative
   load on x86_64, one system-register read on AArch64. */
struct haven_cpu *this_cpu(void);

/* By logical index; NULL when out of range or not yet online. */
struct haven_cpu *cpu_by_id(uint32_t cpu_id);

uint32_t smp_cpu_count(void);
uint32_t smp_online_count(void);

/* Bring up the per-CPU area for the boot CPU.  Must run before anything
   calls this_cpu(). */
void smp_init_bsp(void);

/* Start the application processors.  Returns the number brought online,
   not counting the BSP.  A CPU that fails to start is reported and
   skipped: a machine that boots on 3 of 4 cores is far better than one
   that refuses to boot at all. */
int  smp_start_secondary(void);

/* Entry point for a freshly started application processor. */
void smp_ap_entry(uint32_t cpu_id) HAVEN_NORETURN;

/* Preemption control.  These nest. */
void preempt_disable(void);
void preempt_enable(void);
bool preempt_enabled(void);
bool in_interrupt(void);

#endif /* _HAVEN_SMP_H */
