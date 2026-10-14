/* SPDX-License-Identifier: MIT
 *
 * mutex.c — sleeping locks, semaphores and condition variables.
 */
#include <haven/mutex.h>
#include <haven/sched.h>
#include <haven/smp.h>
#include <haven/panic.h>
#include <haven/console.h>

void mutex_init(struct mutex *m, const char *name)
{
	spin_lock_init(&m->guard, name);
	list_init(&m->waiters);
	m->owner     = NULL;
	m->name      = name;
	m->depth     = 0;
	m->recursive = false;
}

void mutex_init_recursive(struct mutex *m, const char *name)
{
	mutex_init(m, name);
	m->recursive = true;
}

void mutex_lock(struct mutex *m)
{
	/* A mutex blocks, and interrupt context cannot block: there is no
	   thread to put on a wait queue and nothing to switch to.  Assert
	   rather than deadlock, so the bug is reported at its cause. */
	HAVEN_ASSERT_MSG(!in_interrupt(),
	                 "mutex_lock('%s') from interrupt context; use a "
	                 "spinlock there", m->name ? m->name : "unnamed");

	struct thread *self = thread_current();

	for (;;) {
		irqflags_t f = spin_lock(&m->guard);

		if (m->owner == NULL) {
			m->owner = self;
			m->depth = 1;
			spin_unlock(&m->guard, f);
			return;
		}

		if (m->owner == self) {
			if (m->recursive) {
				m->depth++;
				spin_unlock(&m->guard, f);
				return;
			}
			spin_unlock(&m->guard, f);
			panic("mutex: thread %d re-entered non-recursive "
			      "mutex '%s'",
			      self ? self->tid : -1,
			      m->name ? m->name : "unnamed");
		}

		/* Held by somebody else.  Queue and block.  The guard is
		   released by sched_block_on()'s caller contract: we must not
		   hold a spinlock across a context switch, so release it
		   first -- and the wake_pending mechanism in the scheduler is
		   what makes that safe despite the window it opens. */
		spin_unlock(&m->guard, f);
		sched_block_on(&m->waiters);
	}
}

bool mutex_trylock(struct mutex *m)
{
	struct thread *self = thread_current();
	irqflags_t f = spin_lock(&m->guard);

	if (m->owner == NULL) {
		m->owner = self;
		m->depth = 1;
		spin_unlock(&m->guard, f);
		return true;
	}
	if (m->owner == self && m->recursive) {
		m->depth++;
		spin_unlock(&m->guard, f);
		return true;
	}

	spin_unlock(&m->guard, f);
	return false;
}

void mutex_unlock(struct mutex *m)
{
	struct thread *self = thread_current();
	irqflags_t f = spin_lock(&m->guard);

	/* Unlocking a mutex you do not own corrupts the ownership of
	   whoever does.  It is always a bug and never recoverable. */
	HAVEN_ASSERT_MSG(m->owner == self,
	                 "mutex_unlock('%s') by thread %d, which is not the "
	                 "owner", m->name ? m->name : "unnamed",
	                 self ? self->tid : -1);

	if (m->recursive && --m->depth > 0) {
		spin_unlock(&m->guard, f);
		return;
	}

	m->owner = NULL;
	m->depth = 0;
	spin_unlock(&m->guard, f);

	/* Wake one waiter, not all.  Waking all of them means every waiter
	   runs, contends for the mutex, and all but one blocks again -- the
	   thundering herd. */
	sched_wake_one(&m->waiters);
}

bool mutex_is_locked(const struct mutex *m)
{
	return __atomic_load_n(&m->owner, __ATOMIC_RELAXED) != NULL;
}

/* ── Semaphore ───────────────────────────────────────────────────────── */

void semaphore_init(struct semaphore *s, int32_t initial, const char *name)
{
	spin_lock_init(&s->guard, name);
	list_init(&s->waiters);
	s->count = initial;
	s->name  = name;
}

void semaphore_wait(struct semaphore *s)
{
	HAVEN_ASSERT_MSG(!in_interrupt(),
	                 "semaphore_wait('%s') from interrupt context",
	                 s->name ? s->name : "unnamed");

	for (;;) {
		irqflags_t f = spin_lock(&s->guard);
		if (s->count > 0) {
			s->count--;
			spin_unlock(&s->guard, f);
			return;
		}
		spin_unlock(&s->guard, f);
		sched_block_on(&s->waiters);
	}
}

bool semaphore_trywait(struct semaphore *s)
{
	irqflags_t f = spin_lock(&s->guard);
	bool got = s->count > 0;
	if (got)
		s->count--;
	spin_unlock(&s->guard, f);
	return got;
}

void semaphore_post(struct semaphore *s)
{
	/* Safe from interrupt context: this only increments and wakes, and
	   sched_wake() is interrupt-safe.  That asymmetry is deliberate --
	   a driver's interrupt handler posting to a semaphore its thread is
	   waiting on is the standard shape of a blocking driver. */
	irqflags_t f = spin_lock(&s->guard);
	s->count++;
	spin_unlock(&s->guard, f);

	sched_wake_one(&s->waiters);
}

/* ── Condition variable ──────────────────────────────────────────────── */

void condvar_init(struct condvar *cv, const char *name)
{
	spin_lock_init(&cv->guard, name);
	list_init(&cv->waiters);
	cv->name = name;
}

void condvar_wait(struct condvar *cv, struct mutex *m)
{
	HAVEN_ASSERT_MSG(!in_interrupt(),
	                 "condvar_wait('%s') from interrupt context",
	                 cv->name ? cv->name : "unnamed");
	HAVEN_ASSERT_MSG(m->owner == thread_current(),
	                 "condvar_wait('%s') without holding its mutex",
	                 cv->name ? cv->name : "unnamed");

	/* Release the mutex and block.  The scheduler's wake_pending flag
	   closes the window between the two: a signal that arrives after
	   the unlock but before the block is remembered rather than lost,
	   which is the classic lost-wakeup bug. */
	mutex_unlock(m);
	sched_block_on(&cv->waiters);
	mutex_lock(m);
}

void condvar_signal(struct condvar *cv)
{
	sched_wake_one(&cv->waiters);
}

void condvar_broadcast(struct condvar *cv)
{
	sched_wake_all(&cv->waiters);
}
