/* SPDX-License-Identifier: MIT
 *
 * cpu.h — x86_64-private CPU facts and control-register helpers.
 *
 * These are deliberately NOT in include/haven/arch.h: nothing in the
 * generic kernel may ask whether the CPU supports 1 GiB pages, because
 * that question has no meaning on AArch64.  The generic layer asks
 * arch_page_size_for() instead and lets the architecture answer.  Only
 * code under arch/x86_64/ (and x86-only drivers) includes this header.
 */
#ifndef _HAVEN_ARCH_X86_64_CPU_H
#define _HAVEN_ARCH_X86_64_CPU_H

#include <haven/types.h>

/* Feature predicates, valid after arch_early_init(). */
bool arch_cpu_has_nx(void);
bool arch_cpu_has_1g_pages(void);
bool arch_cpu_has_apic(void);
bool arch_cpu_has_x2apic(void);
bool arch_cpu_has_invariant_tsc(void);
bool arch_cpu_has_tsc_deadline(void);
bool arch_cpu_has_smap(void);
bool arch_cpu_has_smep(void);

/* Redirect a kernel-mode fault that landed inside a user-copy routine to
   that routine's recovery path, so a bad user pointer returns -EFAULT
   instead of panicking.  Returns false when the fault was somewhere else,
   in which case it is a real kernel bug. */
struct trapframe;
bool uaccess_fixup(struct trapframe *tf);

/* Timekeeping internals shared between arch/x86_64 translation units. */
void     arch_time_init(void);
uint64_t arch_tsc_hz(void);
bool     arch_tsc_usable(void);

/* The 8253/8254 timer: calibration reference always, scheduler tick when
   there is no usable Local APIC. */
void     pit_start(uint32_t hz);
uint32_t pit_rate(void);
uint64_t pit_tick_count(void);

/* Start the scheduler tick on an application processor. */
void     arch_timer_init_ap(uint32_t hz);

/* Descriptor tables and the exception/IRQ plumbing.
   The GDT and TSS interface is in <haven/arch/x86_64/gdt.h>, and the IDT
   interface in <haven/arch/x86_64/idt.h>. */
void exceptions_init(void);

/* arch_cpu_init_tables() is declared in <haven/arch.h>, since
   kernel/main.c drives the boot sequence and must stay portable.  The
   application-processor form is x86-private. */
void arch_cpu_init_tables_ap(uint32_t cpu);

/* The 8259 PIC.  The Local APIC interface is in
   <haven/arch/x86_64/lapic.h>. */
void pic_init(void);
void pic_disable(void);
void pic_mask_all(void);
void pic_eoi(uint32_t irq);
void pic_set_mask(uint32_t irq, bool masked);

#endif /* _HAVEN_ARCH_X86_64_CPU_H */
