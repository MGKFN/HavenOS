/* SPDX-License-Identifier: MIT
 *
 * timer.c — choosing and starting the scheduler tick.
 */
#include <haven/arch.h>
#include <haven/irq.h>
#include <haven/console.h>
#include <haven/smp.h>
#include <haven/errno.h>
#include <haven/time.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/idt.h>
#include <haven/arch/x86_64/lapic.h>

static enum irq_result lapic_timer_interrupt(uint32_t irq, void *context)
{
	(void)irq;
	(void)context;

	/* Every CPU takes its own LAPIC timer interrupt, which is the whole
	   reason for preferring it over the PIT: the PIT is one global
	   device, so on a multi-processor machine only one CPU could be
	   preempted by it. */
	timer_tick();
	return IRQ_HANDLED;
}

void arch_timer_init(uint32_t hz)
{
	if (lapic_available()) {
		int rc = irq_request(VEC_LAPIC_TIMER, lapic_timer_interrupt,
		                     NULL, "lapic-timer");
		if (rc == 0) {
			lapic_timer_init(hz);
			return;
		}
		kerr("timer: cannot take the LAPIC timer vector: %s; falling "
		     "back to the PIT", haven_strerror(rc));
	}

	/* No APIC, or its vector was unavailable.  The PIT still gives a
	   usable tick on the boot CPU, which is enough to run a scheduler
	   -- just not to preempt application processors. */
	pit_start(hz);
}

void arch_timer_init_ap(uint32_t hz)
{
	/* An application processor has its own LAPIC timer and nothing
	   else: it cannot use the PIT, which is wired to the BSP. */
	if (!lapic_available()) {
		kerr("timer: cpu %u has no LAPIC timer and cannot be "
		     "preempted", arch_cpu_id());
		return;
	}
	lapic_timer_init(hz);
}
