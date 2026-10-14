/* SPDX-License-Identifier: MIT
 *
 * vmlayout.h — the virtual address layout constants the generic VMM uses.
 *
 * kernel/mm/vmm.c needs to know where the kernel arena, the MMIO window
 * and the direct map are, but those are architecture facts.  This header
 * forwards whichever architecture is being built, so vmm.c itself stays
 * free of any #ifdef — the rule scripts/check-portability.sh enforces.
 *
 * An ARM64 port adds one branch here and defines the same five names in
 * include/haven/arch/arm64/memlayout.h.  Nothing in kernel/ changes.
 */
#ifndef _HAVEN_VMLAYOUT_H
#define _HAVEN_VMLAYOUT_H

#if defined(__x86_64__)
#  include <haven/arch/x86_64/memlayout.h>
#elif defined(__aarch64__)
#  include <haven/arch/arm64/memlayout.h>
#else
#  error "HavenOS has no memory layout for this architecture; see docs/PORTING_ARM64.md"
#endif

/* Every architecture's memlayout.h must define these. */
#if !defined(HHDM_BASE) || !defined(KHEAP_BASE) || !defined(KHEAP_SIZE) || \
    !defined(MMIO_BASE) || !defined(MMIO_SIZE) || !defined(KSTACK_BYTES)
#  error "architecture memlayout.h is missing a required address-space constant"
#endif

#endif /* _HAVEN_VMLAYOUT_H */
