/* SPDX-License-Identifier: MIT
 *
 * time.c — monotonic time and calibrated delays.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS derived all time from polling the 8253 PIT counter, and its own
 * source documents what that cost.  The conversion from PIT ticks to
 * microseconds was `elapsed * 1000000 / 1193182` evaluated in 32-bit
 * integers, which has two defects it names explicitly: one PIT tick is
 * 0.838 us so the expression truncated to zero and *destroyed* the tick,
 * making sleep_ms() sleep unboundedly longer than asked; and `elapsed`
 * could reach 11932, whose product with 1000000 does not fit in a
 * uint32_t.  The fix was to carry the remainder in 16.16 fixed point.
 *
 * HavenOS avoids the whole class of problem rather than fixing an
 * instance of it:
 *
 *   - the primary clock is the TSC, read with a single instruction, and
 *     the conversion is a 128-bit multiply-shift with no division and no
 *     truncation;
 *   - the TSC is calibrated once against the PIT, so the scale factor is
 *     measured rather than assumed;
 *   - if the CPU lacks invariant TSC the TSC is not trusted for
 *     timekeeping at all and the PIT channel-2 path is used instead,
 *     because a TSC that changes rate with CPU frequency produces times
 *     that are confidently wrong, which is worse than times that are
 *     coarse.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/arch/x86_64/io.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/acpi.h>
#include <haven/boot.h>

#define PIT_FREQ_HZ      1193182u
#define PIT_CH0_DATA     0x40
#define PIT_CH2_DATA     0x42
#define PIT_COMMAND      0x43
#define PIT_SPEAKER_PORT 0x61

/* How long to wait for the PIT before deciding it is not there. */
#define PIT_WAIT_MAX_CYCLES (1ULL << 31)

#define ACPI_PM_TIMER_HZ    3579545u

/* boot.S direct-maps this much physical memory; ACPI tables above it
   cannot be read this early. */
#define EARLY_MAP_LIMIT     0x100000000ULL

static uint64_t tsc_hz;
static uint64_t tsc_at_boot;
/* Nanoseconds per TSC tick in 32.32 fixed point: ns = (tsc * mult) >> 32.
   Using a reciprocal multiply keeps the hot path free of the 64-bit
   division that would otherwise run on every timestamp. */
static uint64_t tsc_ns_mult;
static bool     tsc_usable;

/*
 * Calibrate the TSC against PIT channel 2 in one-shot mode.
 *
 * Channel 2 is used rather than channel 0 because channel 0 drives the
 * scheduler tick: reprogramming it here would disturb timekeeping that
 * other code already depends on.  Channel 2's gate is under software
 * control through port 0x61, which makes a clean one-shot measurement
 * possible.
 */
static uint64_t calibrate_tsc_once(uint32_t ms)
{
	uint32_t ticks = (uint32_t)(((uint64_t)PIT_FREQ_HZ * ms) / 1000u);
	if (ticks == 0 || ticks > 0xFFFF)
		ticks = 0xFFFF;

	/* Disable the speaker but enable the channel-2 gate. */
	uint8_t port61 = inb(PIT_SPEAKER_PORT);
	outb(PIT_SPEAKER_PORT, (uint8_t)((port61 & ~0x02) | 0x01));

	/* Channel 2, mode 0 (interrupt on terminal count), lo/hi, binary. */
	outb(PIT_COMMAND, 0xB0);
	outb(PIT_CH2_DATA, (uint8_t)(ticks & 0xFF));
	outb(PIT_CH2_DATA, (uint8_t)(ticks >> 8));

	uint64_t start = rdtsc();

	/* Wait for the channel-2 output to go high, which mode 0 does at
	   terminal count.  Bit 5 of port 0x61 is that output.

	   The wait is bounded by the TSC itself, not by a count of port
	   reads.  It used to give up after 100 million reads, which under
	   QEMU is instant; on real hardware a port read costs about a
	   microsecond, so a machine whose PIT never fires -- and firmware
	   on many current laptops gates the 8254's clock off -- sat here
	   for over a minute and a half per attempt, three attempts in a
	   row, with nothing on the screen.

	   Whatever the TSC's rate, a 10 ms window cannot honestly take
	   2^31 cycles: that is 10 ms only on a 200 GHz processor, and on a
	   real one it is between a third of a second and two seconds. */
	while (!(inb(PIT_SPEAKER_PORT) & 0x20)) {
		if (rdtsc() - start > PIT_WAIT_MAX_CYCLES) {
			outb(PIT_SPEAKER_PORT, port61);
			return 0;
		}
	}

	uint64_t end = rdtsc();

	outb(PIT_SPEAKER_PORT, port61);

	if (end <= start)
		return 0;

	/* Scale the measured delta up to a per-second rate. */
	uint64_t elapsed = end - start;
	return (elapsed * PIT_FREQ_HZ) / ticks;
}

/*
 * The TSC rate as the processor itself reports it, or 0.
 *
 * Leaf 0x15 gives the ratio of the TSC to the core crystal and, on some
 * processors, the crystal's frequency.  Leaf 0x16 gives the base
 * frequency in MHz, which is what the invariant TSC runs at; it is
 * rounded to the marketing figure, so it is the second choice.
 *
 * Neither leaf exists on AMD processors or under most hypervisors, which
 * is why this is the fallback and the PIT is still tried first.
 */
static uint64_t tsc_hz_from_cpuid(void)
{
	uint32_t a, b, c, d;

	cpuid_raw(0, 0, &a, &b, &c, &d);
	uint32_t max_leaf = a;

	if (max_leaf >= 0x15) {
		cpuid_raw(0x15, 0, &a, &b, &c, &d);
		/* a = denominator, b = numerator, c = crystal Hz (may be 0) */
		if (a && b && c)
			return ((uint64_t)c * b) / a;
	}

	if (max_leaf >= 0x16) {
		cpuid_raw(0x16, 0, &a, &b, &c, &d);
		if (a & 0xFFFF)
			return (uint64_t)(a & 0xFFFF) * 1000000ULL;
	}

	return 0;
}

/* A TSC rate some x86-64 processor could plausibly have. */
static bool tsc_rate_plausible(uint64_t hz)
{
	return hz >= 100ULL * 1000 * 1000 && hz <= 20ULL * 1000 * 1000 * 1000;
}

/*
 * Measure the TSC against the ACPI PM timer for about 10 ms.
 *
 * The counter is 24 or 32 bits wide and free-running; differences are
 * taken modulo its width, so one wrap inside the window is harmless (a
 * 24-bit counter wraps every 4.7 s).  Like the PIT wait, this gives up
 * by TSC cycles rather than by iteration count, and returns 0 if the
 * counter is not moving.
 */
static uint64_t calibrate_tsc_pmtimer(uint16_t port, bool is_32bit)
{
	const uint32_t mask   = is_32bit ? 0xFFFFFFFFu : 0x00FFFFFFu;
	const uint32_t target = ACPI_PM_TIMER_HZ / 100;      /* 10 ms */

	uint32_t first = inl(port) & mask;
	uint64_t start = rdtsc();
	uint32_t ticks;

	do {
		ticks = ((inl(port) & mask) - first) & mask;
		if (rdtsc() - start > PIT_WAIT_MAX_CYCLES)
			return 0;
	} while (ticks < target);

	uint64_t cycles = rdtsc() - start;
	return (cycles * ACPI_PM_TIMER_HZ) / ticks;
}

static uint64_t median3(uint64_t a, uint64_t b, uint64_t c)
{
	if ((a <= b && b <= c) || (c <= b && b <= a))
		return b;
	if ((b <= a && a <= c) || (c <= a && a <= b))
		return a;
	return c;
}

void arch_time_init(void)
{
	tsc_at_boot = rdtsc();

	if (!arch_cpu_has_invariant_tsc()) {
		/* Calibrate anyway so delays work, but do not promote the
		   TSC to the system clock: on a CPU without invariant TSC
		   the rate tracks the core frequency. */
		kwarn("time: CPU reports no invariant TSC; timestamps will be "
		      "PIT-derived and coarse");
	}

	/* Three references, in order of how much each is trusted.
	 *
	 * 1. The PIT.  Three short runs and the median: a single run can be
	 *    skewed by an SMI landing inside the window, and an SMI is
	 *    exactly the thing that cannot be masked.
	 *
	 *    The result is only believed if it is a rate a processor could
	 *    have.  When the PIT is absent the gate bit this polls can read
	 *    as already set, so the "10 ms" window closes in microseconds
	 *    and the arithmetic yields a TSC of a few kilohertz.  That used
	 *    to be accepted: with the PIT removed in QEMU the kernel
	 *    reported "TSC 0.030 MHz" and every delay, timeout and
	 *    timestamp after it was wrong by five orders of magnitude.
	 *
	 * 2. The ACPI PM timer, which firmware cannot switch off.
	 *
	 * 3. What CPUID says, on processors that say. */
	const char *source = NULL;

	uint64_t r1 = calibrate_tsc_once(10);
	/* If the first attempt timed out the PIT is not ticking, and two
	   more timeouts would only make the machine look hung for longer. */
	uint64_t r2 = r1 ? calibrate_tsc_once(10) : 0;
	uint64_t r3 = r2 ? calibrate_tsc_once(10) : 0;

	if (r1 && r2 && r3 && tsc_rate_plausible(median3(r1, r2, r3))) {
		tsc_hz = median3(r1, r2, r3);
		source = "calibrated against PIT, median of 3";
	} else {
		kwarn("time: the PIT is absent or not counting (firmware on "
		      "many laptops gates it off); trying other clocks");

		uint16_t port  = 0;
		bool     ext   = false;
		uint64_t pm_hz = 0;

		if (acpi_early_pm_timer(&haven_boot_info, EARLY_MAP_LIMIT,
		                        &port, &ext))
			pm_hz = calibrate_tsc_pmtimer(port, ext);

		if (tsc_rate_plausible(pm_hz)) {
			tsc_hz = pm_hz;
			source = "calibrated against the ACPI PM timer";
		} else if (tsc_rate_plausible(tsc_hz_from_cpuid())) {
			tsc_hz = tsc_hz_from_cpuid();
			source = "as reported by CPUID";
		}
	}

	if (!source) {
		kerr("time: no working clock to calibrate against (no PIT, no "
		     "ACPI PM timer, no rate in CPUID); delays and the timer "
		     "tick will be approximate");
		tsc_hz     = 0;
		tsc_usable = false;
		return;
	}

	/* ns per tick, 32.32 fixed point.  1e9 << 32 overflows 64 bits, so
	   the shift is split: (1e9 << 32) / hz == ((1e9 << 16) / hz) << 16
	   loses precision, so compute it as a 64/64 division of the largest
	   safe numerator instead. */
	tsc_ns_mult = (1000000000ULL << 32) / tsc_hz;
	tsc_usable  = arch_cpu_has_invariant_tsc();

	kinfo("time: TSC %llu.%03llu MHz (%s)",
	      (unsigned long long)(tsc_hz / 1000000),
	      (unsigned long long)((tsc_hz / 1000) % 1000), source);
}

nstime_t arch_time_ns(void)
{
	/* Called from klog() before calibration, so this must be safe and
	   quiet when the clock is not up yet rather than requiring every
	   caller to check. */
	if (!tsc_ns_mult)
		return 0;

	uint64_t delta = rdtsc() - tsc_at_boot;

	/* (delta * tsc_ns_mult) >> 32, computed in 128 bits so a machine
	   that has been up for hours does not wrap.  At 3 GHz, delta alone
	   overflows 64 bits after ~195 years, but delta * mult overflows in
	   about 4 seconds — which is precisely the overflow bug HavenDOS
	   documented in its own PIT conversion, in 32-bit form. */
	return (nstime_t)(((__uint128_t)delta * tsc_ns_mult) >> 32);
}

uint64_t arch_tsc_hz(void)
{
	return tsc_hz;
}

/* The portable view of the same facts, for kernel/time/timer.c. */
uint64_t arch_clock_hz(void)
{
	return tsc_hz;
}

bool arch_clock_reliable(void)
{
	return tsc_usable;
}

bool arch_tsc_usable(void)
{
	return tsc_usable;
}

void arch_delay_us(uint64_t us)
{
	if (tsc_hz) {
		uint64_t target_ticks = (tsc_hz * us) / 1000000ULL;
		uint64_t start = rdtsc();
		while ((rdtsc() - start) < target_ticks)
			arch_pause();
		return;
	}

	/* No calibrated clock: fall back to port-0x80 reads, each of which
	   costs roughly one microsecond on an ISA-speed I/O bus.  Coarse,
	   but it is only used before calibration and on machines where
	   calibration failed. */
	for (uint64_t i = 0; i < us; i++)
		io_wait();
}
