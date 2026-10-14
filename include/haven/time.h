/* SPDX-License-Identifier: MIT
 *
 * time.h — timekeeping, delays and timers.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS offered sleep_ms(), which was a busy-wait: it polled the PIT
 * counter until enough time had passed.  On a single-threaded system that
 * is the only option.  With a scheduler it is a waste of a CPU, and worse,
 * a driver that busy-waits for a slow device blocks everything.
 *
 * HavenOS distinguishes the two cases explicitly, because confusing them
 * is a real bug rather than a style preference:
 *
 *   time_delay_us()  busy-wait.  For hardware that needs a settling delay
 *                    measured in microseconds, and for use before the
 *                    scheduler exists.  Never for waiting on an event.
 *   time_sleep_ms()  block.  The calling thread leaves the run queue and
 *                    another runs.  Requires the scheduler.
 */
#ifndef _HAVEN_TIME_H
#define _HAVEN_TIME_H

#include <haven/types.h>
#include <haven/list.h>

/* The scheduler tick rate.  100 Hz gives a 10 ms slice, which is long
   enough that the switch cost is negligible and short enough that
   interactive work stays responsive.  HavenDOS also used 100 Hz. */
#define HZ 100

void     time_init(void);

/* Monotonic nanoseconds since boot.  Never goes backward and never
   wraps in any realistic uptime. */
nstime_t time_ns(void);
uint64_t time_ms(void);

/* Ticks since boot, on the CPU that is asking.  Note this is NOT a
   system-wide count once there are several CPUs: each has its own timer.
   Use time_ns() for anything that must be comparable across CPUs. */
uint64_t time_ticks(void);

/* Busy-wait.  See the note above on which of these to use. */
void     time_delay_us(uint64_t us);
void     time_delay_ms(uint64_t ms);

/* Block the calling thread.  Returns 0, or -HAVEN_EINTR if woken early. */
int      time_sleep_ms(uint64_t ms);
int      time_sleep_ns(nstime_t ns);

/* One-shot timers.  The callback runs in interrupt context on the CPU
   whose tick expired it, so it must not block, allocate or take a mutex. */
struct timer {
	struct list_head node;
	nstime_t         expires;
	void           (*callback)(struct timer *t, void *arg);
	void            *arg;
	bool             armed;
};

void timer_init_entry(struct timer *t,
                      void (*callback)(struct timer *t, void *arg),
                      void *arg);
void timer_arm_ns(struct timer *t, nstime_t delay_ns);
void timer_arm_ms(struct timer *t, uint64_t delay_ms);
bool timer_cancel(struct timer *t);

/* Called once per tick by the architecture's timer interrupt. */
void timer_tick(void);

struct time_stats {
	uint64_t ticks;
	uint64_t timers_armed;
	uint64_t timers_fired;
	uint64_t timers_cancelled;
	uint64_t clock_hz;
	bool     clock_is_tsc;
};

void time_get_stats(struct time_stats *out);

/* Wall-clock date and time, read from the platform's real-time clock.
   Separate from the monotonic clock on purpose: wall-clock time can jump
   backward and must never be used to measure an interval. */
struct haven_tm {
	uint16_t year;
	uint8_t  month;     /* 1-12 */
	uint8_t  day;       /* 1-31 */
	uint8_t  hour;      /* 0-23 */
	uint8_t  minute;
	uint8_t  second;
	uint8_t  weekday;   /* 0 = Sunday */
};

int  time_get_wallclock(struct haven_tm *out);
void time_format(const struct haven_tm *tm, char *buf, size_t bufsz);

/* Supplied by a platform real-time-clock driver (drivers/char/rtc.c on a
   PC).  Weakly defined in kernel/time/timer.c, so a platform with no RTC
   links and reports that it has no wall clock rather than returning a
   plausible wrong date. */
int  platform_read_rtc(struct haven_tm *out);

/* Bring up the platform's real-time clock. */
void rtc_init(void);

#endif /* _HAVEN_TIME_H */
