/* SPDX-License-Identifier: MIT
 *
 * arch_irq.c — routing between the generic IRQ layer and the x86
 * interrupt controllers.
 *
 * LINE NUMBERING
 * The generic layer's "IRQ line" is the interrupt vector, not a
 * controller-relative index.  That choice removes a translation table and
 * the class of bug that comes with one: an interrupt arrives as a vector,
 * is dispatched as that vector, and is acknowledged as that vector.  A
 * driver that wants legacy IRQ 1 writes IRQ_LINE_LEGACY(1) and the
 * mapping is visible at the call site.
 *
 * HavenDOS indexed its handler table by legacy IRQ number 0-15, which is
 * why it could not express an APIC-based system at all: there was no
 * numbering available for a line above 15.
 *
 * WHICH CONTROLLER OWNS WHAT
 *   vectors 32-47   the 8259 pair: legacy device interrupts
 *   vectors 48+     the Local APIC: the scheduler tick, IPIs, errors
 *
 * Both are live at once, in the configuration the Intel MP specification
 * calls virtual wire: the LAPIC is enabled for its own interrupts, and
 * LINT0 passes the PIC's through.  See the LINT0 comment in lapic.c —
 * omitting that step makes every legacy device interrupt vanish the
 * moment the LAPIC is enabled.
 */
#include <haven/arch.h>
#include <haven/irq.h>
#include <haven/console.h>
#include <haven/smp.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/idt.h>
#include <haven/arch/x86_64/lapic.h>

void arch_irq_init(void)
{
	/* The PIC must be remapped before the LAPIC is touched: while it
	   still delivers on vectors 8-15, any device interrupt arrives as
	   a CPU exception. */
	pic_init();

	if (!lapic_init()) {
		kwarn("irq: no local APIC; the scheduler tick will come from "
		      "the PIT and SMP is unavailable");
	}
}

void arch_irq_mask(uint32_t irq)
{
	if (irq >= VEC_IRQ_BASE && irq < VEC_IRQ_BASE + VEC_IRQ_COUNT) {
		pic_set_mask(irq - VEC_IRQ_BASE, true);
		return;
	}
	/* LAPIC-sourced vectors are masked at their LVT entry by the code
	   that owns them (lapic_timer_stop(), for instance); there is no
	   generic per-vector mask. */
}

void arch_irq_unmask(uint32_t irq)
{
	if (irq >= VEC_IRQ_BASE && irq < VEC_IRQ_BASE + VEC_IRQ_COUNT) {
		pic_set_mask(irq - VEC_IRQ_BASE, false);
		return;
	}
}

void arch_irq_eoi(uint32_t irq)
{
	if (irq >= VEC_IRQ_BASE && irq < VEC_IRQ_BASE + VEC_IRQ_COUNT) {
		/* A PIC interrupt delivered through LINT0 in ExtINT mode is
		   acknowledged at the PIC only: the LAPIC passed it through
		   without entering it into its own in-service register, so a
		   LAPIC EOI here would clear an unrelated interrupt. */
		pic_eoi(irq - VEC_IRQ_BASE);
		return;
	}

	/* Everything else came from the LAPIC. */
	lapic_eoi();
}

void arch_smp_send_ipi(uint32_t cpu, uint32_t vector)
{
	struct haven_cpu *target = cpu_by_id(cpu);
	if (!target)
		return;
	lapic_send_ipi(target->hw_id, vector);
}
