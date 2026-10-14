/* SPDX-License-Identifier: MIT
 *
 * paging.h — x86_64-private page-table facts.
 *
 * The kernel address-space construction calls (arch_pt_init_kernel,
 * arch_pt_map_hhdm, arch_pt_map_kernel_image, arch_pt_activate_kernel,
 * arch_pt_drop_identity_map, arch_pt_page_count) are declared in
 * include/haven/arch.h instead of here, because kernel/mm/vmm.c drives
 * that sequence and must not include an architecture header.  Only the
 * genuinely x86-specific additions live here.
 */
#ifndef _HAVEN_ARCH_X86_64_PAGING_H
#define _HAVEN_ARCH_X86_64_PAGING_H

#include <haven/types.h>
#include <haven/arch.h>

/* The physical address of an address space's PML4, for loading into CR3
   from assembly entry paths and for the SMP trampoline. */
paddr_t arch_pt_root(const struct arch_pagetable *pt);

#endif /* _HAVEN_ARCH_X86_64_PAGING_H */
