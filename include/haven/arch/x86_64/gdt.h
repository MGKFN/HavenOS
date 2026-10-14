/* SPDX-License-Identifier: MIT
 *
 * gdt.h — GDT slot and selector numbering.
 *
 * The slot order is fixed by SYSCALL/SYSRET, which derive their target
 * selectors arithmetically from MSR_STAR rather than taking an operand.
 * See the comment at the top of arch/x86_64/cpu/gdt.c; in short, user
 * data must precede user code.
 */
#ifndef _HAVEN_ARCH_X86_64_GDT_H
#define _HAVEN_ARCH_X86_64_GDT_H

/* This header is included from arch/x86_64/cpu/entry.S for the selector
   constants, so everything that is not a preprocessor constant must be
   hidden from the assembler. */
#ifndef __ASSEMBLY__
#include <haven/types.h>
#endif

#define GDT_SLOT_NULL        0
#define GDT_SLOT_KERNEL_CODE 1
#define GDT_SLOT_KERNEL_DATA 2
#define GDT_SLOT_USER_DATA   3
#define GDT_SLOT_USER_CODE   4
#define GDT_SLOT_TSS         5   /* occupies slots 5 and 6: 16-byte
                                    descriptor with a 64-bit base */

#define GDT_SEL_KERNEL_CODE (GDT_SLOT_KERNEL_CODE * 8)        /* 0x08 */
#define GDT_SEL_KERNEL_DATA (GDT_SLOT_KERNEL_DATA * 8)        /* 0x10 */
#define GDT_SEL_USER_DATA   ((GDT_SLOT_USER_DATA * 8) | 3)    /* 0x1B */
#define GDT_SEL_USER_CODE   ((GDT_SLOT_USER_CODE * 8) | 3)    /* 0x23 */
#define GDT_SEL_TSS         (GDT_SLOT_TSS * 8)                /* 0x28 */

/* Interrupt Stack Table entries.  Each gets its own stack, used for traps
   that cannot rely on the interrupted context having a usable one. */
#define IST_DOUBLE_FAULT 1   /* a double fault often IS a bad stack */
#define IST_NMI          2   /* an NMI can arrive anywhere, including
                                mid-stack-switch */
#define IST_DEBUG        3
#define IST_DEPTH        4   /* entries actually backed with stacks */

#ifndef __ASSEMBLY__

void    gdt_init(void);
void    gdt_init_cpu(uint32_t cpu);
void    gdt_init_ap(uint32_t cpu);
void    tss_init(uint32_t cpu, vaddr_t kstack_top);
void    tss_set_kstack(uint32_t cpu, vaddr_t kstack_top);
vaddr_t tss_get_kstack(uint32_t cpu);

#endif /* !__ASSEMBLY__ */

#endif /* _HAVEN_ARCH_X86_64_GDT_H */
