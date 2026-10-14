/* SPDX-License-Identifier: MIT
 *
 * memlayout.h — the x86_64 virtual address space map.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS ran with paging off and a flat 4 GiB segment, so virtual and
 * physical addresses were the same number and there was no layout to
 * speak of.  That is why it had no user/kernel separation to enforce and
 * why its own PMM could hand out the kernel's live BSS: nothing in the
 * hardware distinguished the two.
 *
 * HavenOS uses the canonical 4-level x86_64 split.  The low half is user
 * space, the high half is the kernel, and the hardware enforces the
 * boundary — which is what makes "secure user and kernel separation" a
 * property of the system rather than a convention.
 *
 *   0x0000000000000000 ─┐
 *                       │ user space (128 TiB)
 *   0x00007FFFFFFFFFFF ─┘
 *   ── non-canonical hole ──
 *   0xFFFF800000000000 ─┐ HHDM: direct map of all physical RAM
 *   0xFFFFBFFFFFFFFFFF ─┘ (64 TiB window)
 *   0xFFFFC00000000000 ─┐ kernel heap (vmalloc-style, 16 TiB)
 *   0xFFFFCFFFFFFFFFFF ─┘
 *   0xFFFFD00000000000 ─┐ MMIO / device mappings (16 TiB)
 *   0xFFFFDFFFFFFFFFFF ─┘
 *   0xFFFFE00000000000 ─┐ per-CPU stacks and guard pages
 *   0xFFFFEFFFFFFFFFFF ─┘
 *   0xFFFFFFFF80000000 ─┐ kernel image (2 GiB, -mcmodel=kernel window)
 *   0xFFFFFFFFFFFFFFFF ─┘
 *
 * WHY A DIRECT MAP (HHDM)
 * Every allocator and every driver needs to read or write a page it knows
 * only by physical address.  Without a direct map each of those accesses
 * needs a temporary mapping, a TLB shootdown and an unmap — which is both
 * slow and a rich source of bugs.  With one, phys_to_virt() is a single
 * addition and cannot fail.  The cost is page tables for the map, which
 * 1 GiB pages make negligible.
 */
#ifndef _HAVEN_ARCH_X86_64_MEMLAYOUT_H
#define _HAVEN_ARCH_X86_64_MEMLAYOUT_H

#include <haven/types.h>

/* Kernel image window.  Must match arch/x86_64/boot/linker.ld. */
#define KERNEL_VMA_BASE   0xFFFFFFFF80000000ULL
#define KERNEL_LMA_BASE   0x0000000000200000ULL

/* Direct map of physical memory. */
#define HHDM_BASE         0xFFFF800000000000ULL
#define HHDM_SIZE         0x0000400000000000ULL   /* 64 TiB */

/* Kernel virtual allocation arena (non-contiguous physical backing). */
#define KHEAP_BASE        0xFFFFC00000000000ULL
#define KHEAP_SIZE        0x0000100000000000ULL   /* 16 TiB */

/* Device / MMIO window. */
#define MMIO_BASE         0xFFFFD00000000000ULL
#define MMIO_SIZE         0x0000100000000000ULL   /* 16 TiB */

/* Kernel stacks, each followed by an unmapped guard page. */
#define KSTACK_BASE       0xFFFFE00000000000ULL
#define KSTACK_SIZE       0x0000100000000000ULL
#define KSTACK_PAGES      4                        /* 16 KiB per thread */
#define KSTACK_BYTES      (KSTACK_PAGES * HAVEN_PAGE_SIZE)

/* User address space. */
#define USER_MIN          0x0000000000001000ULL   /* page 0 stays unmapped
                                                     so a NULL dereference
                                                     faults */
#define USER_MAX          0x0000800000000000ULL
#define USER_STACK_TOP    0x00007FFFFFFFF000ULL
#define USER_STACK_PAGES  32                       /* 128 KiB */

/* Paging geometry. */
#define PAGE_SIZE_4K      0x1000ULL
#define PAGE_SIZE_2M      0x200000ULL
#define PAGE_SIZE_1G      0x40000000ULL

#define PT_ENTRIES        512

/* An address is canonical when bits 63:47 are all copies of bit 47.
   The CPU faults on a non-canonical access, so checking here turns a
   user-supplied garbage pointer into -EFAULT instead of a #GP. */
static inline bool is_canonical(uint64_t addr)
{
	uint64_t top = addr >> 47;
	return top == 0 || top == 0x1FFFF;
}

static inline bool is_user_addr(uint64_t addr)
{
	return addr >= USER_MIN && addr < USER_MAX;
}

static inline bool is_kernel_addr(uint64_t addr)
{
	return addr >= HHDM_BASE;
}

#endif /* _HAVEN_ARCH_X86_64_MEMLAYOUT_H */
