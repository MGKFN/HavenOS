/* SPDX-License-Identifier: MIT
 *
 * timer.c — the tick, and the one-shot timer list.
 */
#include <haven/time.h>
#include <haven/arch.h>
#include <haven/smp.h>
#include <haven/sched.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/printf.h>
#include <haven/errno.h>
#include <haven/string.h>

static struct list_head timers = LIST_HEAD_INIT(timers);
static struct time_stats stats;

static volatile uint32_t timer_lock_word;

static irqflags_t timer_lock(void)
{
	irqflags_t f = arch_irq_save();
	while (__atomic_exchange_n(&timer_lock_word, 1, __ATOMIC_ACQUIRE))
		arch_pause();
	return f;
}

static void timer_unlock(irqflags_t f)
{
	__atomic_store_n(&timer_lock_word, 0, __ATOMIC_RELEASE);
	arch_irq_restore(f);
}

nstime_t time_ns(void)
{
	return arch_time_ns();
}

uint64_t time_ms(void)
{
	return arch_time_ns() / 1000000ULL;
}

uint64_t time_ticks(void)
{
	return this_cpu()->ticks;
}

void time_delay_us(uint64_t us)
{
	arch_delay_us(us);
}

void time_delay_ms(uint64_t ms)
{
	arch_delay_us(ms * 1000ULL);
}

void timer_init_entry(struct timer *t,
                      void (*callback)(struct timer *t, void *arg),
                      void *arg)
{
	list_init(&t->node);
	t->expires  = 0;
	t->callback = callback;
	t->arg      = arg;
	t->armed    = false;
}

void timer_arm_ns(struct timer *t, nstime_t delay_ns)
{
	if (!t || !t->callback)
		return;

	irqflags_t f = timer_lock();

	/* Re-arming an already-armed timer must remove it first, or it ends
	   up on the list twice and fires twice — and the second firing is
	   on a timer the owner believes is idle. */
	if (t->armed)
		list_del_init(&t->node);

	t->expires = arch_time_ns() + delay_ns;
	t->armed   = true;

	/* Insert in expiry order, so the tick only has to look at the head
	   of the list in the common case where nothing is due. */
	struct list_head *before = &timers;
	struct timer     *pos;
	list_for_each_entry(pos, &timers, node) {
		if (pos->expires > t->expires)
			break;
		before = &pos->node;
	}
	__list_add(&t->node, before, before->next);

	stats.timers_armed++;
	timer_unlock(f);
}

void timer_arm_ms(struct timer *t, uint64_t delay_ms)
{
	timer_arm_ns(t, delay_ms * 1000000ULL);
}

bool timer_cancel(struct timer *t)
{
	if (!t)
		return false;

	irqflags_t f = timer_lock();
	bool was_armed = t->armed;
	if (was_armed) {
		list_del_init(&t->node);
		t->armed = false;
		stats.timers_cancelled++;
	}
	timer_unlock(f);

	return was_armed;
}

/*
 * One tick.  Runs in interrupt context, on the CPU whose timer fired.
 */
void timer_tick(void)
{
	struct haven_cpu *cpu = this_cpu();
	cpu->ticks++;
	__atomic_fetch_add(&stats.ticks, 1, __ATOMIC_RELAXED);

	/* Expire what is due.  Each callback is called with the list
	   unlocked and the timer already removed, so a callback may re-arm
	   its own timer -- which is how a periodic timer is built on a
	   one-shot list -- without deadlocking or being expired twice. */
	nstime_t now = arch_time_ns();

	for (;;) {
		irqflags_t f = timer_lock();

		if (list_empty(&timers)) {
			timer_unlock(f);
			break;
		}

		struct timer *head = list_first_entry(&timers, struct timer,
		                                      node);
		if (head->expires > now) {
			timer_unlock(f);
			break;
		}

		list_del_init(&head->node);
		head->armed = false;
		stats.timers_fired++;

		void (*cb)(struct timer *, void *) = head->callback;
		void  *arg = head->arg;
		timer_unlock(f);

		if (cb)
			cb(head, arg);
	}

	/* Let the scheduler account for the slice and decide whether the
	   running thread should be replaced.  Deliberately last: a timer
	   callback that wakes a thread should be able to affect this
	   tick's scheduling decision rather than waiting for the next. */
	sched_tick();
}

void time_get_stats(struct time_stats *out)
{
	if (!out)
		return;
	irqflags_t f = timer_lock();
	*out = stats;
	timer_unlock(f);
}

void time_init(void)
{
	list_init(&timers);
	memset(&stats, 0, sizeof(stats));
	stats.clock_hz     = arch_clock_hz();
	stats.clock_is_tsc = arch_clock_reliable();

	kinfo("time: monotonic clock at %llu.%03llu MHz, %s; scheduler tick "
	      "target %d Hz",
	      (unsigned long long)(stats.clock_hz / 1000000),
	      (unsigned long long)((stats.clock_hz / 1000) % 1000),
	      stats.clock_is_tsc ? "rate is invariant"
	                         : "rate may drift with CPU frequency",
	      HZ);
}

/* ── Wall clock ───────────────────────────────────────────────────────
   Supplied by a platform driver (the CMOS RTC on a PC).  Weak here so a
   platform with no RTC still links, and reports that it has no wall clock
   rather than returning a plausible wrong date -- HavenDOS's desktop
   displayed whatever the CMOS held with no indication of whether it was
   trustworthy. */

HAVEN_WEAK int platform_read_rtc(struct haven_tm *out)
{
	(void)out;
	return -HAVEN_ENODEV;
}

int time_get_wallclock(struct haven_tm *out)
{
	if (!out)
		return -HAVEN_EINVAL;
	memset(out, 0, sizeof(*out));
	return platform_read_rtc(out);
}

static const char *const weekday_name[7] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

void time_format(const struct haven_tm *tm, char *buf, size_t bufsz)
{
	if (!buf || bufsz == 0)
		return;
	if (!tm) {
		strlcpy(buf, "(no clock)", bufsz);
		return;
	}

	snprintf(buf, bufsz, "%s %04u-%02u-%02u %02u:%02u:%02u UTC",
	         tm->weekday < 7 ? weekday_name[tm->weekday] : "???",
	         tm->year, tm->month, tm->day,
	         tm->hour, tm->minute, tm->second);
}
