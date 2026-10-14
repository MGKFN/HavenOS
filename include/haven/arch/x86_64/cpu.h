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

/* Timekeeping internals shared between arch/x86_64 translation units. */
void     arch_time_init(void);
uint64_t arch_tsc_hz(void);
bool     arch_tsc_usable(void);

/* Descriptor tables and the exception/IRQ plumbing. */
void gdt_init(void);
void gdt_init_ap(uint32_t cpu);
void idt_init(void);
void idt_load(void);
void exceptions_init(void);

/* Interrupt controllers. */
void pic_init(void);
void pic_disable(void);
void pic_mask_all(void);
void pic_eoi(uint32_t irq);
void pic_set_mask(uint32_t irq, bool masked);

bool lapic_init(void);
void lapic_eoi(void);
uint32_t lapic_id(void);
void lapic_timer_init(uint32_t hz);
void lapic_send_ipi(uint32_t apic_id, uint32_t vector);
void lapic_send_init_sipi(uint32_t apic_id, paddr_t trampoline);

/* TSS, one per CPU, holding the ring-0 stack the syscall and interrupt
   entry paths switch to. */
void tss_init(uint32_t cpu, vaddr_t kstack_top);
void tss_set_kstack(uint32_t cpu, vaddr_t kstack_top);

/* Page-table internals, exposed for vmm bring-up only. */
void arch_pt_init_kernel(void);
void arch_pt_drop_identity_map(void);
int  arch_pt_map_hhdm(paddr_t pa, size_t len);

#endif /* _HAVEN_ARCH_X86_64_CPU_H */
