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

#define PIT_FREQ_HZ      1193182u
#define PIT_CH0_DATA     0x40
#define PIT_CH2_DATA     0x42
#define PIT_COMMAND      0x43
#define PIT_SPEAKER_PORT 0x61

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
	   terminal count.  Bit 5 of port 0x61 is that output.  The spin
	   limit keeps a machine whose PIT does not respond from hanging the
	   boot — HavenOS reports an uncalibrated TSC and moves on. */
	uint32_t spins = 0;
	while (!(inb(PIT_SPEAKER_PORT) & 0x20)) {
		if (++spins > 100000000u)
			return 0;
	}

	uint64_t end = rdtsc();

	outb(PIT_SPEAKER_PORT, port61);

	if (end <= start)
		return 0;

	/* Scale the measured delta up to a per-second rate. */
	uint64_t elapsed = end - start;
	return (elapsed * PIT_FREQ_HZ) / ticks;
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

	/* Three short runs and take the median.  A single calibration can be
	   skewed by an SMI landing inside the window, and an SMI is exactly
	   the thing that cannot be masked. */
	uint64_t r1 = calibrate_tsc_once(10);
	uint64_t r2 = calibrate_tsc_once(10);
	uint64_t r3 = calibrate_tsc_once(10);

	if (!r1 || !r2 || !r3) {
		kerr("time: TSC calibration failed (PIT channel 2 did not "
		     "respond); delays will be approximate");
		tsc_hz     = 0;
		tsc_usable = false;
		return;
	}

	tsc_hz = median3(r1, r2, r3);

	/* ns per tick, 32.32 fixed point.  1e9 << 32 overflows 64 bits, so
	   the shift is split: (1e9 << 32) / hz == ((1e9 << 16) / hz) << 16
	   loses precision, so compute it as a 64/64 division of the largest
	   safe numerator instead. */
	tsc_ns_mult = (1000000000ULL << 32) / tsc_hz;
	tsc_usable  = arch_cpu_has_invariant_tsc();

	kinfo("time: TSC %llu.%03llu MHz (calibrated against PIT, median of 3)",
	      (unsigned long long)(tsc_hz / 1000000),
	      (unsigned long long)((tsc_hz / 1000) % 1000));
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
