/* SPDX-License-Identifier: MIT
 *
 * spinlock.h — mutual exclusion that does not sleep.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS needed no locks: one CPU, no threads, and its heap said so.
 * Every data structure in HavenOS that two CPUs or two threads can reach
 * needs one, and the earlier subsystems in this tree open-coded the same
 * pattern (an atomic exchange plus arch_irq_save) four separate times --
 * in the console, the PMM, the VMM and the allocator. This header is that
 * pattern, written once.
 *
 * WHY INTERRUPTS MUST BE DISABLED WHILE HELD
 * If a thread takes a lock and an interrupt handler on the same CPU then
 * tries to take it, the handler spins waiting for a thread that cannot
 * run until the handler returns. That is a guaranteed deadlock on one
 * CPU, and it is why spin_lock() disables interrupts rather than merely
 * spinning.
 *
 * HavenDOS's bare `cli`/`sti` pairs would have been unsafe here for a
 * related reason: nesting. An inner region ending in `sti` re-enables
 * interrupts even though an outer region had disabled them. spin_lock()
 * saves and restores the previous state, so nesting works.
 *
 * WHY A TICKET LOCK
 * A plain test-and-set lock has no fairness guarantee: on a contended
 * lock a CPU can be starved indefinitely while others take it in turn.
 * A ticket lock serves waiters in arrival order, which bounds the wait
 * and makes a lock-contention problem show up as uniform slowness rather
 * than as one thread mysteriously never making progress.
 */
#ifndef _HAVEN_SPINLOCK_H
#define _HAVEN_SPINLOCK_H

#include <haven/types.h>
#include <haven/arch.h>

struct spinlock {
	uint32_t    next_ticket;   /* the number the next arrival takes */
	uint32_t    now_serving;   /* the number currently admitted */
	const char *name;
	uint32_t    holder_cpu;    /* for diagnostics; valid while held */
	bool        held;
};

#define SPINLOCK_INIT(nm) { .next_ticket = 0, .now_serving = 0, \
                            .name = (nm), .holder_cpu = ~0u, .held = false }

#define DEFINE_SPINLOCK(var, nm) \
	struct spinlock var = SPINLOCK_INIT(nm)

void spin_lock_init(struct spinlock *lock, const char *name);

/* Acquire, returning the interrupt state to pass back to spin_unlock().
   Interrupts are disabled for the duration. */
irqflags_t spin_lock(struct spinlock *lock);
void       spin_unlock(struct spinlock *lock, irqflags_t flags);

/* Try once.  Returns true and fills `flags` on success. */
bool spin_trylock(struct spinlock *lock, irqflags_t *flags);

bool spin_is_held(const struct spinlock *lock);

#endif /* _HAVEN_SPINLOCK_H */
