/* SPDX-License-Identifier: MIT
 *
 * time.c — AArch64 timekeeping.
 *
 * This one is genuinely easier than its x86_64 counterpart, and worth
 * noting as a case where the port is better rather than merely
 * equivalent.
 *
 * The x86_64 TSC has to be CALIBRATED against the PIT, because its rate
 * is not discoverable, and it cannot be trusted at all without the
 * invariant-TSC bit -- which is why arch/x86_64/time/time.c runs three
 * 10 ms measurements and takes the median, and why HavenDOS's PIT
 * arithmetic was such a source of bugs.
 *
 * AArch64's generic timer reports its own frequency in CNTFRQ_EL0 and is
 * architecturally guaranteed to be invariant.  So there is no
 * calibration, no median, no fallback clock and no fixed-point
 * conversion to get wrong: read the counter, scale by a frequency the
 * hardware told you.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/arch/arm64/memlayout.h>

#define read_sysreg(reg) ({                                   \
	uint64_t _v;                                          \
	__asm__ volatile("mrs %0, " #reg : "=r"(_v));         \
	_v;                                                   \
})

static uint64_t counter_hz;
static uint64_t counter_at_boot;
static uint64_t ns_mult;     /* ns per tick, 32.32 fixed point */

void arch_time_init(void)
{
	counter_hz      = read_sysreg(cntfrq_el0);
	counter_at_boot = read_sysreg(cntvct_el0);

	if (counter_hz == 0) {
		/* Firmware is required to program CNTFRQ_EL0 and some does
		   not.  Without it the counter cannot be converted to time
		   at all, so say so rather than inventing a rate. */
		kerr("time: CNTFRQ_EL0 reads zero, which means firmware did "
		     "not program it; timestamps will be unavailable");
		return;
	}

	ns_mult = (1000000000ULL << 32) / counter_hz;

	kinfo("time: generic timer at %llu.%03llu MHz (reported by the "
	      "hardware, no calibration needed)",
	      (unsigned long long)(counter_hz / 1000000),
	      (unsigned long long)((counter_hz / 1000) % 1000));
}

nstime_t arch_time_ns(void)
{
	if (!ns_mult)
		return 0;

	uint64_t delta = read_sysreg(cntvct_el0) - counter_at_boot;

	/* 128-bit multiply-shift, for the same reason as the x86_64 port:
	   delta * ns_mult overflows 64 bits within seconds. */
	return (nstime_t)(((__uint128_t)delta * ns_mult) >> 32);
}

void arch_delay_us(uint64_t us)
{
	if (!counter_hz) {
		/* No usable counter: a bounded spin is all that is left, and
		   it is honest about being approximate. */
		for (uint64_t i = 0; i < us * 100; i++)
			arch_pause();
		return;
	}

	uint64_t target = (counter_hz * us) / 1000000ULL;
	uint64_t start  = read_sysreg(cntvct_el0);

	while ((read_sysreg(cntvct_el0) - start) < target)
		arch_pause();
}

uint64_t arch_clock_hz(void) { return counter_hz; }

bool arch_clock_reliable(void)
{
	/* The AArch64 generic timer is architecturally invariant, so unlike
	   the x86_64 TSC there is no feature bit to check: if it exists, it
	   can be trusted. */
	return counter_hz != 0;
}
