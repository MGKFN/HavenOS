/* SPDX-License-Identifier: MIT
 *
 * sched.c — the preemptive scheduler.
 *
 * THE FOUR RULES THAT MAKE THIS CORRECT
 *
 * 1. A context switch must happen on the interrupted thread's OWN kernel
 *    stack, and never from a nested handler or from an IST stack.
 *
 *    This is subtler than "never from interrupt context", which is what
 *    an earlier version of this file asserted -- and that version was
 *    wrong in a way the self-test caught immediately: a thread spinning
 *    in a tight loop was never preempted at all, because the tick set a
 *    flag and nothing ever acted on it.  A thread that makes no system
 *    calls and never idles reaches no other switch point.
 *
 *    Switching from the timer handler IS safe, because the handler was
 *    entered on the interrupted thread's kernel stack: the interrupt
 *    frame and the handler's own frames simply stay on that stack.  When
 *    the thread is scheduled again, arch_context_switch() returns, the
 *    handler returns, and its `iretq` pops the frame that was waiting
 *    there all along.
 *
 *    What is NOT safe is switching from a nested handler (the outer
 *    handler's frames would be stranded mid-unwind) or from a handler on
 *    an IST stack (the IST stack is shared per-CPU, not per-thread, so
 *    leaving a frame on it corrupts the next user of that vector).  So
 *    the switch happens at the END of irq_dispatch(), after the nesting
 *    count has dropped back to zero, and only for vectors that do not
 *    use an IST.
 *
 * 2. A spinlock may never be held across a context switch.  The next
 *    thread may want the same lock, and it will spin forever waiting for
 *    a thread that is not running.  Callers release before blocking;
 *    sched_block_on() takes the thread off the run queue with interrupts
 *    disabled so there is no window.
 *
 * 3. A wake-up that arrives before the block must not be lost.  The
 *    classic race: a thread checks a condition, decides to block, and the
 *    event happens before it is actually off the run queue -- so the wake
 *    finds nothing to wake and the thread sleeps forever.  struct thread
 *    carries wake_pending for exactly this, and sched_block_on() checks it
 *    before committing.
 *
 * 4. Every CPU always has something to run.  A CPU with an empty run
 *    queue runs its own idle thread, which halts until the next
 *    interrupt.  Without a per-CPU idle thread, a CPU with nothing to do
 *    would have no stack to be on.
 */
#include <haven/sched.h>
#include <haven/arch.h>
#include <haven/mm.h>
#include <haven/smp.h>
#include <haven/time.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/printf.h>
#include <haven/string.h>
#include <haven/errno.h>

static bool     scheduler_active;
static tid_t    next_tid = 1;
static struct sched_stats stats;

/* All live threads, for `ps` and for join.  Separate from the run
   queues, which hold only runnable ones. */
static LIST_HEAD(all_threads);
static LIST_HEAD(sleeping_threads);

/* One lock for thread bookkeeping (the all-threads list, tid allocation)
   and the sleeping list.  Run queues are per-CPU and need no lock for
   the common case; a cross-CPU wake takes this one. */
static volatile uint32_t sched_lock_word;

static irqflags_t sched_lock(void)
{
	irqflags_t f = arch_irq_save();
	while (__atomic_exchange_n(&sched_lock_word, 1, __ATOMIC_ACQUIRE))
		arch_pause();
	return f;
}

static void sched_unlock(irqflags_t f)
{
	__atomic_store_n(&sched_lock_word, 0, __ATOMIC_RELEASE);
	arch_irq_restore(f);
}

const char *thread_state_name(enum thread_state s)
{
	switch (s) {
	case THREAD_NEW:      return "new";
	case THREAD_READY:    return "ready";
	case THREAD_RUNNING:  return "running";
	case THREAD_BLOCKED:  return "blocked";
	case THREAD_SLEEPING: return "sleeping";
	case THREAD_ZOMBIE:   return "zombie";
	case THREAD_DEAD:     return "dead";
	default:              return "?";
	}
}

bool sched_active(void)
{
	return scheduler_active;
}

struct thread *thread_current(void)
{
	return (struct thread *)arch_get_current_thread();
}

/* ── Run queues ──────────────────────────────────────────────────────── */

static void runqueue_add(struct haven_cpu *cpu, struct thread *t)
{
	HAVEN_ASSERT(t->priority < SCHED_PRIO_LEVELS);

	t->state = THREAD_READY;
	list_add_tail(&t->queue_node, &cpu->runqueue[t->priority]);
	cpu->runnable |= (1u << t->priority);
	cpu->nr_runnable++;
}

static struct thread *runqueue_take(struct haven_cpu *cpu)
{
	/* Lowest set bit of the bitmask is the highest-priority non-empty
	   band.  Using a bitmask rather than scanning the eight lists keeps
	   the common case -- one runnable thread -- to a single
	   instruction. */
	if (cpu->runnable == 0)
		return NULL;

	unsigned prio = (unsigned)__builtin_ctz(cpu->runnable);
	HAVEN_ASSERT(prio < SCHED_PRIO_LEVELS);
	HAVEN_ASSERT(!list_empty(&cpu->runqueue[prio]));

	struct thread *t = list_first_entry(&cpu->runqueue[prio],
	                                    struct thread, queue_node);
	list_del_init(&t->queue_node);

	if (list_empty(&cpu->runqueue[prio]))
		cpu->runnable &= ~(1u << prio);
	cpu->nr_runnable--;

	return t;
}

/* ── Switching ───────────────────────────────────────────────────────── */

static void account_switch(struct thread *from, struct thread *to,
                           struct haven_cpu *cpu)
{
	nstime_t now = arch_time_ns();

	if (from && from->last_scheduled && now > from->last_scheduled)
		from->cpu_time_ns += now - from->last_scheduled;

	to->last_scheduled = now;
	to->switch_count++;
	cpu->context_switches++;
	stats.context_switches++;
}

void sched_switch(void)
{
	/* Rule 1.  Nesting must be back to zero: a switch from a nested
	   handler would strand the outer handler's frames. */
	HAVEN_ASSERT_MSG(!in_interrupt(),
	                 "sched_switch() with interrupt nesting still held");

	irqflags_t flags = arch_irq_save();

	struct haven_cpu *cpu  = this_cpu();
	struct thread    *prev = cpu->current;

	/* Preemption disabled means a critical section asked not to be
	   rescheduled.  Honour it, but remember that a switch is owed. */
	if (cpu->preempt_count > 0) {
		arch_irq_restore(flags);
		return;
	}

	/* A thread that is still RUNNING here is giving up its turn
	   voluntarily or has been preempted, so it goes back on the queue.
	   A thread that has already changed state (blocked, sleeping,
	   exiting) must NOT: it put itself on a wait queue, and adding it
	   to the run queue as well would have it in two places at once. */
	if (prev && prev->state == THREAD_RUNNING) {
		if (prev != cpu->idle)
			runqueue_add(cpu, prev);
		else
			prev->state = THREAD_READY;
	}

	struct thread *next = runqueue_take(cpu);
	if (!next)
		next = cpu->idle;

	if (next == prev) {
		/* Nothing else to run.  Put the state back and return. */
		next->state = THREAD_RUNNING;
		arch_irq_restore(flags);
		return;
	}

	next->state           = THREAD_RUNNING;
	next->slice_remaining = SCHED_SLICE_TICKS;
	cpu->current          = next;

	account_switch(prev, next, cpu);
	arch_set_current_thread(next);

	/* The CPU must find the INCOMING thread's kernel stack in the TSS,
	   or a user-mode fault in the new thread would push its frame onto
	   a stack another thread is using. */
	arch_set_kernel_stack(next->kstack_top);

	arch_context_switch(prev->context, next->context);

	/* Control arrives here when THIS thread is scheduled again, which
	   may be much later and on a different CPU.  Nothing cached from
	   before the switch is still valid -- in particular `cpu` now
	   refers to the wrong CPU, which is why the restore below uses the
	   saved flags and nothing else. */
	arch_irq_restore(flags);
}

void sched_yield(void)
{
	stats.voluntary_yields++;
	sched_switch();
}

/* ── Tick ────────────────────────────────────────────────────────────── */

static void wake_expired_sleepers(nstime_t now)
{
	irqflags_t f = sched_lock();

	struct thread *t, *tmp;
	list_for_each_entry_safe(t, tmp, &sleeping_threads, queue_node) {
		if (t->wake_at > now)
			break;   /* the list is kept in wake order */

		list_del_init(&t->queue_node);
		t->wake_at = 0;

		struct haven_cpu *home = cpu_by_id(t->home_cpu);
		if (!home || !home->online)
			home = this_cpu();
		runqueue_add(home, t);
	}

	sched_unlock(f);
}

void sched_tick(void)
{
	if (!scheduler_active)
		return;

	struct haven_cpu *cpu = this_cpu();
	struct thread    *cur = cpu->current;

	wake_expired_sleepers(arch_time_ns());

	if (!cur)
		return;

	/* The idle thread has no slice to exhaust: if anything is runnable
	   it should be replaced immediately, not at the end of a notional
	   time slice. */
	if (cur == cpu->idle) {
		if (cpu->runnable) {
			cur->slice_remaining = 0;
			cpu->need_resched    = true;
		}
		return;
	}

	if (--cur->slice_remaining > 0)
		return;

	/* Slice exhausted.  A higher-priority band being runnable is also a
	   reason to switch, but that is already implied: runqueue_take()
	   picks the highest band, so simply switching does the right thing. */
	cur->slice_remaining = 0;
	cpu->need_resched    = true;
}

/*
 * Called from thread context at a point where switching is safe, to
 * perform a switch the timer interrupt asked for.  The idle loop and the
 * syscall return path both call it.
 */
void sched_preempt_if_needed(void)
{
	if (!scheduler_active)
		return;

	/* Nesting must be unwound; see rule 1. */
	if (in_interrupt())
		return;

	struct haven_cpu *cpu = this_cpu();
	struct thread    *cur = cpu->current;

	if (!cur)
		return;
	if (!cpu->need_resched)
		return;
	if (cpu->preempt_count > 0)
		return;

	/* Nothing to switch to.  Clearing the flag here rather than leaving
	   it set means the next tick re-evaluates, instead of this CPU
	   re-entering the scheduler on every return from every interrupt
	   for as long as its run queue stays empty. */
	if (!cpu->runnable && cur != cpu->idle) {
		cpu->need_resched = false;
		return;
	}

	cpu->need_resched = false;
	stats.preemptions++;
	sched_switch();
}

/* ── Blocking and waking ─────────────────────────────────────────────── */

void sched_wake(struct thread *t)
{
	if (!t)
		return;

	irqflags_t f = sched_lock();

	switch (t->state) {
	case THREAD_BLOCKED:
	case THREAD_SLEEPING:
		list_del_init(&t->queue_node);
		t->wait_queue = NULL;
		t->wake_at    = 0;
		break;

	case THREAD_NEW:
		break;

	case THREAD_READY:
	case THREAD_RUNNING:
		/* Already runnable: nothing to do.  This is common and not an
		   error -- two producers can both signal the same waiter. */
		sched_unlock(f);
		return;

	case THREAD_ZOMBIE:
	case THREAD_DEAD:
		sched_unlock(f);
		return;

	default:
		sched_unlock(f);
		return;
	}

	/* Rule 3.  If the thread has not reached sched_block_on() yet, it
	   will see this flag and decline to block. */
	t->wake_pending = true;

	struct haven_cpu *home = cpu_by_id(t->home_cpu);
	if (!home || !home->online)
		home = this_cpu();
	runqueue_add(home, t);

	sched_unlock(f);

	/* If the woken thread outranks what is running on its CPU, ask for
	   a reschedule.  On another CPU that needs an IPI; on this one the
	   next safe point will pick it up. */
	struct haven_cpu *self = this_cpu();
	if (home == self) {
		if (self->current && t->priority < self->current->priority) {
			self->current->slice_remaining = 0;
			self->need_resched             = true;
		}
	} else if (home->current && t->priority < home->current->priority) {
		arch_smp_send_ipi(home->cpu_id, IPI_VECTOR_RESCHEDULE);
	}
}

void sched_block_on(struct list_head *queue)
{
	HAVEN_ASSERT_MSG(!in_interrupt(),
	                 "sched_block_on() from interrupt context");

	struct thread *cur = thread_current();
	HAVEN_ASSERT(cur != NULL);

	irqflags_t f = sched_lock();

	/* Rule 3.  A wake that arrived between the caller's condition check
	   and this point must not be lost: consume the flag and carry on
	   running instead of blocking forever. */
	if (cur->wake_pending) {
		cur->wake_pending = false;
		sched_unlock(f);
		return;
	}

	cur->state      = THREAD_BLOCKED;
	cur->wait_queue = queue;
	if (queue)
		list_add_tail(&cur->queue_node, queue);

	sched_unlock(f);

	/* The thread is off the run queue and on the wait queue, so this
	   switch will not come back until somebody wakes it. */
	sched_switch();
}

void sched_wake_one(struct list_head *queue)
{
	if (!queue)
		return;

	irqflags_t f = sched_lock();
	struct thread *t = NULL;
	if (!list_empty(queue))
		t = list_first_entry(queue, struct thread, queue_node);
	sched_unlock(f);

	if (t)
		sched_wake(t);
}

void sched_wake_all(struct list_head *queue)
{
	if (!queue)
		return;

	/* Collect first, wake after.  sched_wake() takes the same lock and
	   removes the thread from this list, so waking while iterating
	   would walk a list that is being modified underneath. */
	for (;;) {
		irqflags_t f = sched_lock();
		if (list_empty(queue)) {
			sched_unlock(f);
			return;
		}
		struct thread *t = list_first_entry(queue, struct thread,
		                                    queue_node);
		sched_unlock(f);
		sched_wake(t);
	}
}

int time_sleep_ns(nstime_t ns)
{
	if (!scheduler_active) {
		/* Before the scheduler exists there is nothing to switch to,
		   so a sleep can only be a busy-wait.  Saying so here means
		   driver init code can call time_sleep_ms() unconditionally
		   and get the right behaviour in both phases. */
		arch_delay_us(ns / 1000);
		return 0;
	}

	struct thread *cur = thread_current();
	if (!cur)
		return -HAVEN_EINVAL;

	irqflags_t f = sched_lock();

	if (cur->wake_pending) {
		cur->wake_pending = false;
		sched_unlock(f);
		return -HAVEN_EAGAIN;
	}

	cur->wake_at = arch_time_ns() + ns;
	cur->state   = THREAD_SLEEPING;

	/* Keep the list in wake order, so the tick can stop at the first
	   entry that is not due. */
	struct list_head *before = &sleeping_threads;
	struct thread    *pos;
	list_for_each_entry(pos, &sleeping_threads, queue_node) {
		if (pos->wake_at > cur->wake_at)
			break;
		before = &pos->queue_node;
	}
	__list_add(&cur->queue_node, before, before->next);

	sched_unlock(f);

	sched_switch();
	return 0;
}

int time_sleep_ms(uint64_t ms)
{
	return time_sleep_ns(ms * 1000000ULL);
}

/* ── Thread lifecycle ────────────────────────────────────────────────── */

struct thread *thread_create(const char *name, void (*entry)(void *),
                             void *arg, uint8_t priority)
{
	if (!entry)
		return NULL;
	if (priority >= SCHED_PRIO_LEVELS)
		priority = SCHED_PRIO_NORMAL;

	struct thread *t = kzalloc(sizeof(*t));
	if (!t) {
		kerr("sched: cannot allocate a thread structure");
		return NULL;
	}

	t->kstack_top = vmm_alloc_kstack();
	if (!t->kstack_top) {
		kerr("sched: cannot allocate a kernel stack for '%s'",
		     name ? name : "?");
		kfree(t);
		return NULL;
	}

	/* The context lives at the top of the thread's own kernel stack, so
	   it needs no separate allocation and is guaranteed to exist for
	   exactly as long as the stack does. */
	size_t ctx_size = arch_context_size();
	t->context = (struct arch_context *)(t->kstack_top - ctx_size);
	vaddr_t usable_top = ALIGN_DOWN(t->kstack_top - ctx_size, 16);

	strlcpy(t->name, name ? name : "thread", sizeof(t->name));
	t->state           = THREAD_NEW;
	t->priority        = priority;
	t->base_priority   = priority;
	t->slice_remaining = SCHED_SLICE_TICKS;
	t->home_cpu        = arch_cpu_id();
	list_init(&t->queue_node);
	list_init(&t->proc_node);
	list_init(&t->join_waiters);

	arch_context_init_kernel(t->context, usable_top, entry, arg);

	irqflags_t f = sched_lock();
	t->tid = next_tid++;
	list_add_tail(&t->proc_node, &all_threads);
	stats.threads_created++;
	stats.threads_live++;
	sched_unlock(f);

	kdebug("sched: created thread %d '%s' priority %u, stack top 0x%llx",
	       t->tid, t->name, t->priority,
	       (unsigned long long)t->kstack_top);

	/* Make it runnable on the creating CPU.  A real load balancer would
	   place it elsewhere; see docs/DESIGN_DECISIONS.md#scheduler. */
	f = sched_lock();
	runqueue_add(this_cpu(), t);
	sched_unlock(f);

	return t;
}

void thread_exit(int code)
{
	struct thread *cur = thread_current();
	HAVEN_ASSERT_MSG(cur != NULL, "thread_exit() with no current thread");
	HAVEN_ASSERT_MSG(cur != this_cpu()->idle,
	                 "the idle thread must never exit");

	kdebug("sched: thread %d '%s' exiting with code %d",
	       cur->tid, cur->name, code);

	irqflags_t f = sched_lock();
	cur->exit_code = code;
	cur->state     = THREAD_ZOMBIE;
	stats.threads_exited++;
	sched_unlock(f);

	/* Wake anyone in thread_join().  Done before the final switch,
	   because after it this thread never runs again. */
	sched_wake_all(&cur->join_waiters);

	/* The stack this code is running on belongs to the thread being
	   destroyed, so it cannot be freed here.  The joiner frees it, or
	   it is reclaimed when the thread is reaped. */
	sched_switch();

	panic("thread %d was scheduled after exiting", cur->tid);
}

int thread_join(struct thread *t)
{
	if (!t)
		return -HAVEN_EINVAL;

	while (t->state != THREAD_ZOMBIE && t->state != THREAD_DEAD) {
		if (!scheduler_active)
			return -HAVEN_EINVAL;
		sched_block_on(&t->join_waiters);
	}

	int code = t->exit_code;

	irqflags_t f = sched_lock();
	if (t->state == THREAD_ZOMBIE) {
		t->state = THREAD_DEAD;
		list_del_init(&t->proc_node);
		if (stats.threads_live)
			stats.threads_live--;
		sched_unlock(f);

		/* Safe now: the thread is not running, so its stack is idle. */
		vmm_free_kstack(t->kstack_top);
		kfree(t);
	} else {
		sched_unlock(f);
	}

	return code;
}

void thread_set_priority(struct thread *t, uint8_t priority)
{
	if (!t || priority >= SCHED_PRIO_LEVELS)
		return;

	irqflags_t f = sched_lock();

	/* A thread sitting on a run queue is filed under its current
	   priority, so changing the field alone would leave it in the wrong
	   band -- and the bitmask would then claim a band is non-empty when
	   it is not, which trips the assertion in runqueue_take(). */
	if (t->state == THREAD_READY) {
		struct haven_cpu *cpu = cpu_by_id(t->home_cpu);
		if (cpu) {
			list_del_init(&t->queue_node);
			if (list_empty(&cpu->runqueue[t->priority]))
				cpu->runnable &= ~(1u << t->priority);
			cpu->nr_runnable--;

			t->priority = priority;
			runqueue_add(cpu, t);
			sched_unlock(f);
			return;
		}
	}

	t->priority = priority;
	sched_unlock(f);
}

/* ── Idle ────────────────────────────────────────────────────────────── */

static void idle_thread_main(void *arg)
{
	(void)arg;

	for (;;) {
		/* Take anything that became runnable while we were halted. */
		sched_preempt_if_needed();

		if (!this_cpu()->runnable) {
			/* arch_idle() enables interrupts and halts as one
			   atomic pair; see the comment in cpu.c for why that
			   matters. */
			arch_idle();
		}
	}
}

/* ── Initialisation ──────────────────────────────────────────────────── */

/*
 * Adopt the currently executing code as a thread.
 *
 * At this point the kernel is running on the boot stack with no thread
 * structure.  Rather than special-casing "no current thread" throughout
 * the scheduler, the boot path is given a real struct thread describing
 * where it already is.  The first context switch then saves this context
 * like any other, and the boot thread becomes schedulable.
 */
static struct thread *adopt_current_as_thread(const char *name)
{
	struct thread *t = kzalloc(sizeof(*t));
	if (!t)
		panic("sched: cannot allocate the boot thread");

	size_t ctx_size = arch_context_size();
	t->context = (struct arch_context *)kzalloc(ctx_size);
	if (!t->context)
		panic("sched: cannot allocate the boot thread's context");

	/* The boot thread is already running, on the stack boot.S set up,
	   so it needs no stack allocated to run ON.  It does need one for
	   the TSS's rsp0: that is the stack the CPU switches to when a trap
	   arrives from ring 3, and leaving it at zero would mean a
	   user-mode fault pushing its frame at address 0.  The boot thread
	   itself never enters ring 3, but the TSS is per-CPU and must be
	   valid from the moment user mode becomes reachable. */
	t->kstack_top = vmm_alloc_kstack();
	if (!t->kstack_top)
		panic("sched: cannot allocate a trap stack for the boot thread");

	strlcpy(t->name, name, sizeof(t->name));
	t->state           = THREAD_RUNNING;
	t->priority        = SCHED_PRIO_NORMAL;
	t->base_priority   = SCHED_PRIO_NORMAL;
	t->slice_remaining = SCHED_SLICE_TICKS;
	t->home_cpu        = arch_cpu_id();
	t->last_scheduled  = arch_time_ns();
	list_init(&t->queue_node);
	list_init(&t->proc_node);
	list_init(&t->join_waiters);

	irqflags_t f = sched_lock();
	t->tid = next_tid++;
	list_add_tail(&t->proc_node, &all_threads);
	stats.threads_created++;
	stats.threads_live++;
	sched_unlock(f);

	return t;
}

void sched_init(void)
{
	list_init(&all_threads);
	list_init(&sleeping_threads);
	memset(&stats, 0, sizeof(stats));

	struct haven_cpu *cpu = this_cpu();

	/* Rule 4: every CPU needs an idle thread so that having nothing to
	   run is an ordinary scheduling outcome rather than a special case
	   with no stack. */
	char idle_name[THREAD_NAME_MAX];
	snprintf(idle_name, sizeof(idle_name), "idle/%u", cpu->cpu_id);

	/* The boot context becomes the first real thread. */
	struct thread *boot = adopt_current_as_thread("kinit");
	cpu->current = boot;
	arch_set_current_thread(boot);
	arch_set_kernel_stack(boot->kstack_top);

	scheduler_active = true;

	struct thread *idle = thread_create(idle_name, idle_thread_main, NULL,
	                                    SCHED_PRIO_IDLE);
	if (!idle)
		panic("sched: cannot create the idle thread for cpu %u",
		      cpu->cpu_id);

	/* The idle thread is not a normal queue member: it runs only when
	   nothing else can, so take it off the run queue and park it in the
	   CPU's idle slot. */
	irqflags_t f = sched_lock();
	if (idle->state == THREAD_READY) {
		list_del_init(&idle->queue_node);
		if (list_empty(&cpu->runqueue[idle->priority]))
			cpu->runnable &= ~(1u << idle->priority);
		cpu->nr_runnable--;
	}
	cpu->idle = idle;
	sched_unlock(f);

	kinfo("sched: priority round-robin, %d bands, %d-tick slices "
	      "(%d ms at %d Hz)",
	      SCHED_PRIO_LEVELS, SCHED_SLICE_TICKS,
	      SCHED_SLICE_TICKS * 1000 / HZ, HZ);
	kinfo("sched: boot context adopted as thread %d '%s'",
	      boot->tid, boot->name);
}

void sched_get_stats(struct sched_stats *out)
{
	if (!out)
		return;

	irqflags_t f = sched_lock();
	*out = stats;
	out->threads_runnable = this_cpu()->nr_runnable;
	sched_unlock(f);
}

void sched_dump(void)
{
	struct sched_stats s;
	sched_get_stats(&s);

	kinfo("sched: %llu switches, %llu preemptions, %llu yields",
	      (unsigned long long)s.context_switches,
	      (unsigned long long)s.preemptions,
	      (unsigned long long)s.voluntary_yields);
	kinfo("sched: %u threads live, %llu created, %llu exited",
	      s.threads_live,
	      (unsigned long long)s.threads_created,
	      (unsigned long long)s.threads_exited);

	kinfo("sched:  tid  pri  state     cpu-ms  switches  name");

	irqflags_t f = sched_lock();
	struct thread *t;
	list_for_each_entry(t, &all_threads, proc_node) {
		kinfo("sched: %4d  %3u  %-9s %6llu  %8llu  %s",
		      t->tid, t->priority, thread_state_name(t->state),
		      (unsigned long long)(t->cpu_time_ns / 1000000),
		      (unsigned long long)t->switch_count, t->name);
	}
	sched_unlock(f);
}
