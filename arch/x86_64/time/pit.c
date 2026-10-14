/* SPDX-License-Identifier: MIT
 *
 * pit.c — the 8253/8254 timer, used as the scheduler tick when there is
 * no usable Local APIC, and as the calibration reference always.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS derived every timestamp in the system from polling this
 * device, and its own source documents the two defects that produced:
 * the tick-to-microsecond conversion truncated to zero (so a tick was
 * consumed and contributed nothing, making sleep_ms() sleep much longer
 * than asked), and the intermediate product overflowed 32 bits.
 *
 * HavenOS does not derive time from the PIT at all -- arch_time_ns()
 * reads the TSC. The PIT does two jobs here:
 *
 *   * channel 2 is the calibration reference for the TSC and the LAPIC
 *     timer, where a one-shot measurement avoids the conversion entirely;
 *   * channel 0 provides the scheduler tick on a machine with no APIC.
 *
 * Neither job needs the arithmetic that went wrong.
 */
#include <haven/arch.h>
#include <haven/irq.h>
#include <haven/console.h>
#include <haven/arch/x86_64/io.h>
#include <haven/errno.h>
#include <haven/time.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/idt.h>

#define PIT_BASE_HZ   1193182u
#define PIT_CH0_DATA  0x40
#define PIT_COMMAND   0x43

/* Channel 0, access low then high byte, mode 3 (square wave), binary. */
#define PIT_MODE_RATE 0x36

static uint64_t pit_ticks;
static uint32_t pit_hz;

uint64_t pit_tick_count(void)
{
	return __atomic_load_n(&pit_ticks, __ATOMIC_RELAXED);
}

static enum irq_result pit_interrupt(uint32_t irq, void *context)
{
	(void)irq;
	(void)context;

	__atomic_fetch_add(&pit_ticks, 1, __ATOMIC_RELAXED);
	timer_tick();
	return IRQ_HANDLED;
}

void pit_start(uint32_t hz)
{
	if (hz == 0)
		hz = 100;

	uint32_t divisor = PIT_BASE_HZ / hz;

	/* The divisor is a 16-bit register where 0 means 65536.  A hz low
	   enough to produce a divisor above 65535 cannot be expressed, and
	   writing the truncated low 16 bits would run the timer at a
	   wildly different rate than requested -- which is exactly the
	   class of silent misconfiguration HavenDOS's pit_init() had before
	   it started tracking the divisor it actually programmed. */
	if (divisor > 65535) {
		kwarn("pit: %u Hz needs divisor %u, which does not fit in 16 "
		      "bits; using the slowest expressible rate (%u Hz)",
		      hz, divisor, PIT_BASE_HZ / 65535);
		divisor = 65535;
	}
	if (divisor == 0)
		divisor = 1;

	pit_hz = PIT_BASE_HZ / divisor;

	outb(PIT_COMMAND, PIT_MODE_RATE);
	outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
	outb(PIT_CH0_DATA, (uint8_t)(divisor >> 8));

	int rc = irq_request(IRQ_LINE_LEGACY(0), pit_interrupt, NULL,
	                     "pit-timer");
	if (rc != 0) {
		kerr("pit: cannot take IRQ 0: %s", haven_strerror(rc));
		return;
	}

	kinfo("pit: scheduler tick at %u Hz (requested %u, divisor %u)",
	      pit_hz, hz, divisor);
}

uint32_t pit_rate(void)
{
	return pit_hz;
}
