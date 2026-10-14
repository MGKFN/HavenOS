/* SPDX-License-Identifier: MIT
 *
 * console.c — the kernel log: format once, fan out to every sink.
 */
#include <haven/console.h>
#include <haven/printf.h>
#include <haven/string.h>
#include <haven/arch.h>
#include <haven/panic.h>

/* Ring buffer.  64 KiB of static storage, because the log must work before
   the allocator exists and must keep working when the allocator is the
   thing that broke. */
#define RING_SIZE 65536
static char   ring[RING_SIZE];
static size_t ring_head;      /* next write position */
static bool   ring_wrapped;

static struct console *sinks;
static enum log_level  threshold = LOG_INFO;

/* The console is written from thread context, from interrupt context, and
   from panic context on any CPU.  A full spinlock would be the right
   answer, but console_register() runs before the scheduler and before SMP,
   so this uses the raw interrupt-disable form of mutual exclusion: it is
   correct on one CPU and, combined with the atomic-exchange guard in
   panic(), good enough that a second CPU can never interleave inside a
   panic.  kernel/sync/spinlock.c takes over once SMP is up — see
   console_lock() below. */
static volatile uint32_t console_locked;

static irqflags_t console_lock(void)
{
	irqflags_t flags = arch_irq_save();
	/* Re-entrancy from an interrupt that fired mid-log would interleave
	   two messages; with interrupts already off that cannot happen on
	   this CPU, and the exchange serialises other CPUs. */
	while (__atomic_exchange_n(&console_locked, 1, __ATOMIC_ACQUIRE)) {
		/* If a panic is under way, a spinning CPU must not block the
		   panic from printing: give up the lock contest and let the
		   output interleave rather than deadlock the machine. */
		if (panic_in_progress())
			break;
		arch_pause();
	}
	return flags;
}

static void console_unlock(irqflags_t flags)
{
	__atomic_store_n(&console_locked, 0, __ATOMIC_RELEASE);
	arch_irq_restore(flags);
}

static void ring_put(const char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		ring[ring_head] = buf[i];
		ring_head = (ring_head + 1) % RING_SIZE;
		if (ring_head == 0)
			ring_wrapped = true;
	}
}

static void sinks_write(enum log_level level, const char *buf, size_t len)
{
	for (struct console *c = sinks; c; c = c->next) {
		if (!c->enabled || !c->write)
			continue;
		if (level > c->min_level)
			continue;
		c->write(buf, len);
	}
}

/* Write the ring's current contents to one sink, oldest byte first, as
   at most two contiguous spans.  Takes no lock: callers hold it, or are
   console_register(), which has not yet published the sink. */
static void ring_replay(struct console *con)
{
	size_t avail = console_ring_size();
	if (!avail || !con->write)
		return;

	size_t start = (ring_head + RING_SIZE - avail) % RING_SIZE;
	size_t first = RING_SIZE - start;
	if (first > avail)
		first = avail;

	con->write(&ring[start], first);
	if (avail > first)
		con->write(&ring[0], avail - first);
}

void console_register(struct console *con)
{
	if (!con || !con->write)
		return;

	irqflags_t flags = console_lock();
	con->enabled = true;
	con->next = sinks;
	sinks = con;
	console_unlock(flags);

	/* Replay what has already been logged, so a sink that comes up late
	   still shows the whole boot.  This is the reason the framebuffer
	   console can register after memory management and still display the
	   PMM's own startup lines.

	   The replay writes straight out of the ring in at most two spans
	   rather than copying it to a local buffer first: the ring is 64 KiB
	   and the boot stack is 64 KiB, so the obvious `char tail[RING_SIZE]`
	   would overflow the stack on the very first registration.  The
	   -Wframe-larger-than=2048 in the build flags exists to catch exactly
	   this, and did. */
	ring_replay(con);
}

void console_unregister(struct console *con)
{
	irqflags_t flags = console_lock();
	for (struct console **p = &sinks; *p; p = &(*p)->next) {
		if (*p == con) {
			*p = con->next;
			con->next = NULL;
			break;
		}
	}
	console_unlock(flags);
}

void console_flush_all(void)
{
	for (struct console *c = sinks; c; c = c->next) {
		if (c->flush)
			c->flush();
	}
}

void console_set_level(enum log_level level)
{
	if (level < LOG_LEVEL_COUNT)
		threshold = level;
}

enum log_level console_get_level(void)
{
	return threshold;
}

static const char *const level_tag[LOG_LEVEL_COUNT] = {
	[LOG_PANIC] = "PANIC",
	[LOG_ERR]   = "ERROR",
	[LOG_WARN]  = " WARN",
	[LOG_INFO]  = " INFO",
	[LOG_DEBUG] = "DEBUG",
};

void kvlog(enum log_level level, const char *fmt, va_list ap)
{
	if (level >= LOG_LEVEL_COUNT)
		level = LOG_DEBUG;

	char   line[1024];
	size_t pos = 0;

	/* Timestamp every line.  arch_time_ns() is safe to call before the
	   timer is calibrated — it returns 0 — so this never needs a
	   "is time up yet" conditional at the call sites. */
	nstime_t t  = arch_time_ns();
	uint64_t s  = t / 1000000000ULL;
	uint64_t us = (t % 1000000000ULL) / 1000ULL;

	int n = snprintf(line, sizeof(line), "[%5llu.%06llu] %s  ",
	                 (unsigned long long)s, (unsigned long long)us,
	                 level_tag[level]);
	if (n > 0)
		pos = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;

	n = vsnprintf(line + pos, sizeof(line) - pos, fmt, ap);
	if (n > 0) {
		size_t want = pos + (size_t)n;
		pos = want < sizeof(line) - 1 ? want : sizeof(line) - 1;
	}

	/* One trailing newline, exactly.  Callers that supply their own
	   should not get a blank line, and callers that forget should not
	   run two messages together — the latter being how HavenDOS's log
	   lines ended up concatenated on screen. */
	while (pos > 0 && (line[pos - 1] == '\n' || line[pos - 1] == '\r'))
		pos--;
	if (pos < sizeof(line) - 1)
		line[pos++] = '\n';
	line[pos] = '\0';

	irqflags_t flags = console_lock();
	ring_put(line, pos);
	if (level <= threshold)
		sinks_write(level, line, pos);
	console_unlock(flags);
}

void klog(enum log_level level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	kvlog(level, fmt, ap);
	va_end(ap);
}

void kwrite_raw(const char *buf, size_t len)
{
	irqflags_t flags = console_lock();
	ring_put(buf, len);
	sinks_write(LOG_PANIC, buf, len);
	console_unlock(flags);
}

void kputs_raw(const char *s)
{
	kwrite_raw(s, strlen(s));
}

size_t console_ring_size(void)
{
	return ring_wrapped ? RING_SIZE : ring_head;
}

size_t console_ring_read(char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return 0;

	size_t avail = console_ring_size();
	size_t n     = avail < outsz ? avail : outsz;

	/* Copy the most recent n bytes in chronological order. */
	size_t start = (ring_head + RING_SIZE - n) % RING_SIZE;
	for (size_t i = 0; i < n; i++)
		out[i] = ring[(start + i) % RING_SIZE];

	return n;
}
