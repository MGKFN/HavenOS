/* SPDX-License-Identifier: MIT
 *
 * tables.c — bringing up the descriptor tables in the right order.
 *
 * The order is not interchangeable and the failure modes are severe
 * enough to be worth stating:
 *
 *   1. GDT first.  The IDT's gate descriptors name a code selector, so
 *      the GDT must already describe one.  The TSS, which the IDT's IST
 *      entries depend on, is also installed here.
 *   2. IDT second.  From this point a fault is reported rather than
 *      triple-faulting, which is what makes everything after it
 *      debuggable.
 *   3. Exceptions last, since they only announce themselves.
 *
 * This is a separate translation unit from gdt.c and idt.c so that the
 * sequence is visible in one place rather than being implied by the order
 * of calls in kernel/main.c -- the arrangement that let HavenDOS's boot
 * order bugs hide.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/sched.h>
#include <haven/mm.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/gdt.h>
#include <haven/arch/x86_64/idt.h>

void arch_cpu_init_tables(void)
{
	gdt_init();
	idt_init();
	exceptions_init();
}

void arch_cpu_init_tables_ap(uint32_t cpu)
{
	/* An application processor needs its own GDT (its TSS descriptor is
	   per-CPU) but shares the one IDT, which it only has to load. */
	gdt_init_cpu(cpu);
	idt_load();
}
