/* SPDX-License-Identifier: MIT
 *
 * mutex.h — mutual exclusion that sleeps.
 *
 * WHEN TO USE WHICH
 * A spinlock burns CPU while it waits, so it is only correct for a
 * critical section shorter than the cost of a context switch, and it must
 * never be held across anything that can block. A mutex puts the waiting
 * thread on a wait queue and lets something else run, so it is the right
 * choice for anything that touches a device, allocates, or copies to user
 * space.
 *
 * Getting this backwards is a real bug in both directions: a spinlock
 * held across a disk read stalls a CPU for milliseconds, and a mutex
 * taken in an interrupt handler tries to block a context that cannot
 * block.  mutex_lock() asserts the latter rather than deadlocking.
 */
#ifndef _HAVEN_MUTEX_H
#define _HAVEN_MUTEX_H

#include <haven/types.h>
#include <haven/list.h>
#include <haven/spinlock.h>

struct thread;

struct mutex {
	struct spinlock   guard;      /* protects the fields below */
	struct thread    *owner;
	struct list_head  waiters;
	const char       *name;
	uint32_t          depth;      /* recursion depth, owner only */
	bool              recursive;
};

void mutex_init(struct mutex *m, const char *name);
void mutex_init_recursive(struct mutex *m, const char *name);

void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
bool mutex_is_locked(const struct mutex *m);

/* A counting semaphore, for producer/consumer and for bounding how many
   threads may be inside a resource at once. */
struct semaphore {
	struct spinlock  guard;
	int32_t          count;
	struct list_head waiters;
	const char      *name;
};

void semaphore_init(struct semaphore *s, int32_t initial, const char *name);
void semaphore_wait(struct semaphore *s);
bool semaphore_trywait(struct semaphore *s);
void semaphore_post(struct semaphore *s);

/* A condition variable, used with a mutex.  Required whenever a thread
   must wait for a predicate rather than for a resource -- which is most
   blocking in a filesystem or a driver. */
struct condvar {
	struct spinlock  guard;
	struct list_head waiters;
	const char      *name;
};

void condvar_init(struct condvar *cv, const char *name);

/* Atomically release `m`, block, and re-acquire `m` on wake.  The
   atomicity is the whole point: a thread that released the mutex and
   then blocked could miss a signal that arrived in between. */
void condvar_wait(struct condvar *cv, struct mutex *m);
void condvar_signal(struct condvar *cv);
void condvar_broadcast(struct condvar *cv);

#endif /* _HAVEN_MUTEX_H */
