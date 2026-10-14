/* SPDX-License-Identifier: MIT
 *
 * memlayout.h — the AArch64 virtual address layout.
 *
 * AArch64 splits the address space by hardware rather than by convention:
 * TTBR0_EL1 translates the low half and TTBR1_EL1 the high half, with the
 * boundary set by TCR_EL1.T0SZ/T1SZ.  That is cleaner than x86_64's
 * single root with a canonical hole, because a user address and a kernel
 * address go through DIFFERENT page-table roots -- so a process switch
 * changes TTBR0 only and the kernel's TLB entries are never invalidated.
 *
 * The constants below mirror the x86_64 layout so that kernel/mm/vmm.c,
 * which is architecture-neutral, needs no change.  That symmetry is the
 * point: these five names are the entire memory-layout contract between
 * the generic VMM and an architecture.
 *
 *   0x0000000000000000 ─┐ user space, via TTBR0_EL1 (256 TiB)
 *   0x0000FFFFFFFFFFFF ─┘
 *   0xFFFF000000000000 ─┐ HHDM: direct map of all physical RAM
 *   0xFFFF3FFFFFFFFFFF ─┘
 *   0xFFFF400000000000 ─┐ kernel heap arena
 *   0xFFFF4FFFFFFFFFFF ─┘
 *   0xFFFF500000000000 ─┐ device / MMIO mappings
 *   0xFFFF5FFFFFFFFFFF ─┘
 *   0xFFFF600000000000 ─┐ per-CPU stacks and guard pages
 *   0xFFFF6FFFFFFFFFFF ─┘
 *   0xFFFFFFFF80000000 ─┐ kernel image
 *   0xFFFFFFFFFFFFFFFF ─┘
 */
#ifndef _HAVEN_ARCH_ARM64_MEMLAYOUT_H
#define _HAVEN_ARCH_ARM64_MEMLAYOUT_H

#include <haven/types.h>

/* Kernel image window.  The same value as x86_64 deliberately: it keeps
   the kernel's own addresses recognisable across both ports, and AArch64
   has no equivalent of -mcmodel=kernel's 2 GiB constraint to satisfy. */
#define KERNEL_VMA_BASE   0xFFFFFFFF80000000ULL
#define KERNEL_LMA_BASE   0x0000000040200000ULL   /* typical AArch64 RAM base */

#define HHDM_BASE         0xFFFF000000000000ULL
#define HHDM_SIZE         0x0000400000000000ULL   /* 64 TiB */

#define KHEAP_BASE        0xFFFF400000000000ULL
#define KHEAP_SIZE        0x0000100000000000ULL   /* 16 TiB */

#define MMIO_BASE         0xFFFF500000000000ULL
#define MMIO_SIZE         0x0000100000000000ULL   /* 16 TiB */

#define KSTACK_BASE       0xFFFF600000000000ULL
#define KSTACK_SIZE       0x0000100000000000ULL
#define KSTACK_PAGES      4
#define KSTACK_BYTES      (KSTACK_PAGES * HAVEN_PAGE_SIZE)

#define USER_MIN          0x0000000000001000ULL
#define USER_MAX          0x0001000000000000ULL   /* 48-bit TTBR0 range */
#define USER_STACK_TOP    0x0000FFFFFFFFF000ULL
#define USER_STACK_PAGES  32

/* AArch64 page granules.  4 KiB is the one every implementation supports;
   16 KiB and 64 KiB are optional and change the translation-table shape
   entirely, so the port starts with 4 KiB. */
#define PAGE_SIZE_4K      0x1000ULL
#define PAGE_SIZE_2M      0x200000ULL     /* a level-2 block */
#define PAGE_SIZE_1G      0x40000000ULL   /* a level-1 block */

#define PT_ENTRIES        512

/* With TTBR0 and TTBR1 both configured for a 48-bit range, an address is
   valid when its top 16 bits are all zeros (user, TTBR0) or all ones
   (kernel, TTBR1).  Anything else raises a translation fault, so the same
   check the x86_64 port calls "canonical" applies here too. */
static inline bool is_canonical(uint64_t addr)
{
	uint64_t top = addr >> 48;
	return top == 0 || top == 0xFFFF;
}

static inline bool is_user_addr(uint64_t addr)
{
	return addr >= USER_MIN && addr < USER_MAX;
}

static inline bool is_kernel_addr(uint64_t addr)
{
	return addr >= HHDM_BASE;
}

#endif /* _HAVEN_ARCH_ARM64_MEMLAYOUT_H */
