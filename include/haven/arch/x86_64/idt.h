/* SPDX-License-Identifier: MIT
 *
 * idt.h — interrupt vector assignments.
 */
#ifndef _HAVEN_ARCH_X86_64_IDT_H
#define _HAVEN_ARCH_X86_64_IDT_H

#include <haven/types.h>

/* 0-31 are CPU exceptions, fixed by the architecture. */
#define VEC_DIVIDE_ERROR      0
#define VEC_DEBUG             1
#define VEC_NMI               2
#define VEC_BREAKPOINT        3
#define VEC_OVERFLOW          4
#define VEC_BOUND_RANGE       5
#define VEC_INVALID_OPCODE    6
#define VEC_DEVICE_NA         7
#define VEC_DOUBLE_FAULT      8
#define VEC_INVALID_TSS      10
#define VEC_SEGMENT_NP       11
#define VEC_STACK_FAULT      12
#define VEC_GP_FAULT         13
#define VEC_PAGE_FAULT       14
#define VEC_FPU_ERROR        16
#define VEC_ALIGNMENT        17
#define VEC_MACHINE_CHECK    18
#define VEC_SIMD_FP          19
#define VEC_VIRTUALISATION   20
#define VEC_CONTROL_PROT     21

/* 32-47: the legacy PIC's 16 lines, remapped out of the exception range.
   They must be remapped: the 8259 defaults to vectors 8-15, which collide
   with #DF, #TS, #NP, #SS, #GP, #PF and the FPU error -- so a timer tick
   would arrive as a double fault. */
#define VEC_IRQ_BASE         32
#define VEC_IRQ_COUNT        16

/* 48-: APIC and IPI vectors.  Kept above the PIC range so a system can
   run both during the handover from PIC to APIC. */
#define VEC_LAPIC_TIMER      48
#define VEC_LAPIC_ERROR      49
#define VEC_LAPIC_SPURIOUS  255   /* the APIC spec fixes the low nibble at
                                     0xF; 255 is the conventional choice */
#define VEC_IPI_RESCHEDULE   50
#define VEC_IPI_TLB_FLUSH    51
#define VEC_IPI_HALT         52
#define VEC_IPI_CALL         53

/* The SYSCALL path does not go through the IDT, but it labels its
   synthetic trap frame with this so a backtrace can tell a syscall from
   an interrupt. */
#define VEC_SYSCALL        0x80

void idt_init(void);
void idt_load(void);

#endif /* _HAVEN_ARCH_X86_64_IDT_H */
