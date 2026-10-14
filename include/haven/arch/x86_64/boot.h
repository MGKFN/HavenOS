/* SPDX-License-Identifier: MIT
 *
 * boot.h — x86_64 boot entry points shared between boot.S and C.
 */
#ifndef _HAVEN_ARCH_X86_64_BOOT_H
#define _HAVEN_ARCH_X86_64_BOOT_H

#include <haven/types.h>

/* Called from boot.S once the kernel is executing 64-bit code in the
   higher half.  Never returns: it ends in haven_kmain(). */
void x86_boot_main(uint32_t magic, uint32_t mbinfo_pa) HAVEN_NORETURN;

/* Multiboot2 handover values, captured in 32-bit mode by boot.S. */
extern uint32_t boot_magic;
extern uint32_t boot_mbinfo;

/* Boot page tables and stack, defined in boot.S. */
extern uint64_t boot_pml4[];
extern uint8_t  boot_stack_bottom[];
extern uint8_t  boot_stack_top[];

#endif /* _HAVEN_ARCH_X86_64_BOOT_H */
