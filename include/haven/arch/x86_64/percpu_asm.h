/* SPDX-License-Identifier: MIT
 *
 * percpu_asm.h — byte offsets into the per-CPU block, for assembly.
 *
 * arch/x86_64/cpu/entry.S has to reach three fields of struct haven_cpu
 * from assembly, where the C struct layout is not available.  Hard-coded
 * numbers in the assembly would silently go wrong the moment a field is
 * added to the struct — and the symptom would be the syscall path loading
 * a garbage kernel stack pointer, which is about as bad as it gets.
 *
 * So the offsets are named here and checked against the real struct by
 * _Static_assert in arch/x86_64/cpu/percpu.c.  A struct change that
 * invalidates one of them is a compile error.
 */
#ifndef _HAVEN_ARCH_X86_64_PERCPU_ASM_H
#define _HAVEN_ARCH_X86_64_PERCPU_ASM_H

/* GS base points at struct percpu_anchor: { self, block }. */
#define PERCPU_ANCHOR_SELF   0
#define PERCPU_ANCHOR_BLOCK  8

/* Offsets within struct haven_cpu. */
#define PERCPU_OFF_KSTACK    24   /* kernel_stack_top */
#define PERCPU_OFF_USER_RSP  32   /* user_rsp         */

#endif /* _HAVEN_ARCH_X86_64_PERCPU_ASM_H */
