/* SPDX-License-Identifier: MIT
 *
 * irq.c — the interrupt handler registry and dispatcher.
 */
#include <haven/irq.h>
#include <haven/arch.h>
#include <haven/mm.h>
#include <haven/smp.h>
#include <haven/sched.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/errno.h>
#include <haven/string.h>

#define IRQ_LINES 256

struct irq_line {
	struct list_head actions;
	uint64_t         count;
	uint64_t         unhandled;
	bool             enabled;
};

static struct irq_line  lines[IRQ_LINES];
static struct irq_stats stats;

/* A statically allocated pool for irq_request(), so a driver can take an
   interrupt before kmalloc exists — which the timer does. */
#define STATIC_ACTIONS 32
static struct irq_action static_pool[STATIC_ACTIONS];
static uint32_t          static_used;

static volatile uint32_t irq_lock_word;

static irqflags_t irq_lock(void)
{
	irqflags_t f = arch_irq_save();
	while (__atomic_exchange_n(&irq_lock_word, 1, __ATOMIC_ACQUIRE))
		arch_pause();
	return f;
}

static void irq_unlock(irqflags_t f)
{
	__atomic_store_n(&irq_lock_word, 0, __ATOMIC_RELEASE);
	arch_irq_restore(f);
}

void irq_init(void)
{
	for (int i = 0; i < IRQ_LINES; i++) {
		list_init(&lines[i].actions);
		lines[i].count     = 0;
		lines[i].unhandled = 0;
		lines[i].enabled   = false;
	}
	memset(&stats, 0, sizeof(stats));

	arch_irq_init();
	kinfo("irq: handler registry ready for %d lines", IRQ_LINES);
}

int irq_register(uint32_t irq, struct irq_action *action)
{
	if (irq >= IRQ_LINES || !action || !action->handler)
		return -HAVEN_EINVAL;

	irqflags_t f = irq_lock();

	/* Registering the same action twice would put it on the list in a
	   cycle, so the dispatcher would never terminate. */
	struct irq_action *pos;
	list_for_each_entry(pos, &lines[irq].actions, node) {
		if (pos == action) {
			irq_unlock(f);
			kerr("irq: action '%s' is already registered on line %u",
			     action->name ? action->name : "?", irq);
			return -HAVEN_EEXIST;
		}
	}

	action->count    = 0;
	action->spurious = 0;
	list_init(&action->node);
	list_add_tail(&action->node, &lines[irq].actions);

	bool first = list_length(&lines[irq].actions) == 1;
	irq_unlock(f);

	kinfo("irq: line %u -> '%s'%s", irq,
	      action->name ? action->name : "unnamed",
	      first ? "" : " (shared)");

	/* Unmask only once the handler is in place.  Unmasking first means
	   an interrupt can arrive with an empty action list, which the
	   dispatcher correctly reports as unhandled — noise for something
	   that is really just a race in the registration order. */
	irq_enable_line(irq);
	return 0;
}

int irq_unregister(uint32_t irq, struct irq_action *action)
{
	if (irq >= IRQ_LINES || !action)
		return -HAVEN_EINVAL;

	irqflags_t f = irq_lock();

	struct irq_action *pos, *tmp;
	bool found = false;
	list_for_each_entry_safe(pos, tmp, &lines[irq].actions, node) {
		if (pos == action) {
			list_del(&pos->node);
			found = true;
			break;
		}
	}

	bool now_empty = list_empty(&lines[irq].actions);
	irq_unlock(f);

	if (!found)
		return -HAVEN_ENOENT;

	/* Mask a line that has no handlers left, or the device will keep
	   asserting it and the dispatcher will keep reporting it. */
	if (now_empty)
		irq_disable_line(irq);

	return 0;
}

int irq_request(uint32_t irq, irq_handler_fn handler, void *context,
                const char *name)
{
	if (irq >= IRQ_LINES || !handler)
		return -HAVEN_EINVAL;

	struct irq_action *action = NULL;

	irqflags_t f = irq_lock();
	if (static_used < STATIC_ACTIONS)
		action = &static_pool[static_used++];
	irq_unlock(f);

	if (!action) {
		/* The static pool is for drivers that register before
		   kmalloc exists; after that, allocate. */
		action = kzalloc(sizeof(*action));
		if (!action)
			return -HAVEN_ENOMEM;
	}

	action->handler = handler;
	action->context = context;
	action->name    = name;

	return irq_register(irq, action);
}

void irq_enable_line(uint32_t irq)
{
	if (irq >= IRQ_LINES)
		return;
	lines[irq].enabled = true;
	arch_irq_unmask(irq);
}

void irq_disable_line(uint32_t irq)
{
	if (irq >= IRQ_LINES)
		return;
	lines[irq].enabled = false;
	arch_irq_mask(irq);
}

void irq_dispatch(uint32_t irq)
{
	if (irq >= IRQ_LINES) {
		stats.spurious++;
		return;
	}

	struct haven_cpu *cpu = this_cpu();

	/* Mark interrupt context for the duration.  The scheduler checks
	   this: a context switch taken from an interrupt handler would
	   return into a different thread's stack with the interrupt frame
	   still on the old one. */
	cpu->irq_nesting++;

	lines[irq].count++;
	stats.total++;

	bool handled = false;
	struct irq_action *pos;

	/* Walking the list without the lock is deliberate.  Taking a lock
	   here would mean a driver that registers an interrupt from inside
	   its own handler deadlocks, and more importantly the list is
	   append-only during normal operation.  Registration and dispatch
	   are serialised by the line being masked while it has no
	   handlers. */
	list_for_each_entry(pos, &lines[irq].actions, node) {
		enum irq_result r = pos->handler(irq, pos->context);
		if (r == IRQ_NONE) {
			pos->spurious++;
			continue;
		}
		pos->count++;
		handled = true;
	}

	if (!handled) {
		lines[irq].unhandled++;
		stats.unhandled++;

		/* Report, but only while it is still informative.  A line
		   that is wrongly routed fires continuously, and logging
		   every occurrence would bury the rest of the log and starve
		   the system doing serial I/O.  Masking it after enough
		   unhandled interrupts is what keeps the machine usable. */
		if (lines[irq].unhandled <= 5) {
			kwarn("irq: line %u fired with no handler claiming it "
			      "(%llu times)", irq,
			      (unsigned long long)lines[irq].unhandled);
		} else if (lines[irq].unhandled == 1000) {
			kerr("irq: line %u has fired 1000 times unclaimed; "
			     "masking it to keep the system responsive", irq);
			irq_disable_line(irq);
		}
	}

	/* Acknowledge after the handlers have run.  Acknowledging first
	   would let a level-triggered device re-assert before its handler
	   has cleared the condition, producing an interrupt storm.  It must
	   also happen BEFORE any context switch below, or the controller
	   stays blocked for as long as the next thread runs. */
	arch_irq_eoi(irq);

	cpu->irq_nesting--;

	/*
	 * Honour a reschedule the tick asked for.
	 *
	 * This is where preemption actually happens, and it has to be here.
	 * A thread spinning in a tight loop makes no system calls and never
	 * idles, so it reaches no other switch point -- the timer interrupt
	 * is the only thing that ever regains control from it.  An earlier
	 * version of this kernel set a flag in the tick and acted on it
	 * only in the idle loop and on syscall return, and the in-kernel
	 * self-test caught the consequence immediately: four spinning
	 * threads, one of which ran forever while the other three never
	 * started.
	 *
	 * Switching here is safe because we are on the interrupted thread's
	 * own kernel stack with the nesting count back to zero; the
	 * interrupt frame below us stays put and is popped when this thread
	 * runs again.  See rule 1 in kernel/sched/sched.c.
	 */
	if (cpu->irq_nesting == 0)
		sched_preempt_if_needed();
}

void irq_get_stats(struct irq_stats *out)
{
	if (out)
		*out = stats;
}

uint64_t irq_count_for(uint32_t irq)
{
	return irq < IRQ_LINES ? lines[irq].count : 0;
}

void irq_dump(void)
{
	kinfo("irq: %llu interrupts, %llu unhandled, %llu spurious",
	      (unsigned long long)stats.total,
	      (unsigned long long)stats.unhandled,
	      (unsigned long long)stats.spurious);

	for (int i = 0; i < IRQ_LINES; i++) {
		if (list_empty(&lines[i].actions) && lines[i].count == 0)
			continue;

		struct irq_action *pos;
		list_for_each_entry(pos, &lines[i].actions, node) {
			kinfo("irq: %3d  %10llu  %-20s%s", i,
			      (unsigned long long)pos->count,
			      pos->name ? pos->name : "unnamed",
			      lines[i].enabled ? "" : " (masked)");
		}
		if (list_empty(&lines[i].actions)) {
			kinfo("irq: %3d  %10llu  (no handler, %llu unclaimed)",
			      i, (unsigned long long)lines[i].count,
			      (unsigned long long)lines[i].unhandled);
		}
	}
}
