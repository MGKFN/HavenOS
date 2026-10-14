/* SPDX-License-Identifier: MIT
 *
 * spinlock.c — ticket spinlock.
 */
#include <haven/spinlock.h>
#include <haven/smp.h>
#include <haven/panic.h>
#include <haven/console.h>

/* How long to spin before declaring a deadlock.  A lock held longer than
   this is not contention, it is a bug -- and a kernel that spins forever
   is a kernel with no diagnostic at all.  The bound is deliberately
   generous: a legitimate critical section is measured in microseconds. */
#define SPIN_DEADLOCK_LIMIT 100000000u

void spin_lock_init(struct spinlock *lock, const char *name)
{
	lock->next_ticket = 0;
	lock->now_serving = 0;
	lock->name        = name;
	lock->holder_cpu  = ~0u;
	lock->held        = false;
}

irqflags_t spin_lock(struct spinlock *lock)
{
	/* Interrupts off BEFORE taking a ticket.  If an interrupt arrived
	   between taking the ticket and being served, and its handler
	   wanted this lock, the handler would wait for a ticket that cannot
	   advance until the handler returns. */
	irqflags_t flags = arch_irq_save();

	uint32_t ticket = __atomic_fetch_add(&lock->next_ticket, 1,
	                                     __ATOMIC_RELAXED);

	uint32_t spins = 0;
	while (__atomic_load_n(&lock->now_serving, __ATOMIC_ACQUIRE) != ticket) {
		arch_pause();

		if (++spins > SPIN_DEADLOCK_LIMIT) {
			/* Name the lock and both CPUs: with that, a deadlock
			   is a five-minute diagnosis instead of an afternoon. */
			panic("spinlock: cpu %u waited too long for '%s' "
			      "(held by cpu %u, ticket %u, serving %u) -- "
			      "this is a deadlock, not contention",
			      arch_cpu_id(),
			      lock->name ? lock->name : "unnamed",
			      lock->holder_cpu, ticket,
			      __atomic_load_n(&lock->now_serving,
			                      __ATOMIC_RELAXED));
		}
	}

	lock->held       = true;
	lock->holder_cpu = arch_cpu_id();

	return flags;
}

bool spin_trylock(struct spinlock *lock, irqflags_t *out_flags)
{
	irqflags_t flags = arch_irq_save();

	uint32_t serving = __atomic_load_n(&lock->now_serving,
	                                   __ATOMIC_ACQUIRE);
	uint32_t next    = __atomic_load_n(&lock->next_ticket,
	                                   __ATOMIC_RELAXED);

	/* Free exactly when nobody is queued.  Taking a ticket and then
	   giving up would leave a gap in the sequence and wedge every later
	   waiter, which is why this uses a compare-exchange on next_ticket
	   rather than a fetch-add. */
	if (serving != next ||
	    !__atomic_compare_exchange_n(&lock->next_ticket, &next, next + 1,
	                                 false, __ATOMIC_ACQUIRE,
	                                 __ATOMIC_RELAXED)) {
		arch_irq_restore(flags);
		return false;
	}

	lock->held       = true;
	lock->holder_cpu = arch_cpu_id();
	if (out_flags)
		*out_flags = flags;
	return true;
}

void spin_unlock(struct spinlock *lock, irqflags_t flags)
{
	HAVEN_ASSERT_MSG(lock->held,
	                 "spin_unlock() of '%s', which is not held",
	                 lock->name ? lock->name : "unnamed");

	lock->held       = false;
	lock->holder_cpu = ~0u;

	/* Release ordering: everything written inside the critical section
	   must be visible before the next waiter is admitted. */
	__atomic_fetch_add(&lock->now_serving, 1, __ATOMIC_RELEASE);

	arch_irq_restore(flags);
}

bool spin_is_held(const struct spinlock *lock)
{
	return __atomic_load_n(&lock->held, __ATOMIC_RELAXED);
}
