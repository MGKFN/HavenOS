/* SPDX-License-Identifier: MIT
 *
 * dma.h — memory a device can reach.
 *
 * WHY THIS EXISTS (and the bug that produced it)
 * A device that does DMA is given a PHYSICAL address and writes to it
 * directly, with no page tables involved.  Kernel virtual memory from
 * vmm_alloc() or kmalloc() satisfies neither requirement a device has:
 * it is not physically contiguous, and its physical pages have no fixed
 * relationship to its virtual address, so arch_virt_to_phys() cannot
 * even be asked for one.
 *
 * The first version of the VirtIO block driver called
 * arch_virt_to_phys() on a kmalloc'd buffer.  It worked for the
 * descriptor ring, which was allocated from the PMM, and panicked the
 * moment the partition parser passed it a kmalloc'd sector buffer:
 *
 *     virt_to_phys: 0xffffc000000167c0 is not in a fixed-offset kernel
 *     window; use arch_pt_translate() for mapped memory
 *
 * That panic was a deliberate assertion rather than a fault, which is why
 * it named the problem instead of corrupting memory -- a driver on a
 * system without the check would have handed the device a physical
 * address derived from arithmetic on an arena pointer and had it DMA into
 * whatever happened to live there.
 *
 * HavenDOS had no equivalent risk because it had no virtual memory: every
 * address was physical, so every buffer was DMA-safe by accident.  That
 * is the one way in which paging made driver authorship harder, and this
 * header is the answer to it.
 *
 * THE TWO FACILITIES
 *   dma_alloc_coherent()  memory that is physically contiguous, below a
 *                         device's addressing limit, and reachable
 *                         through the direct map -- for descriptor rings
 *                         and anything a device reads continuously.
 *   dma_bounce            a driver-owned staging buffer for transfers
 *                         whose caller-supplied buffer is not DMA-safe.
 *                         Costs a copy; correct for every caller.
 */
#ifndef _HAVEN_DMA_H
#define _HAVEN_DMA_H

#include <haven/types.h>
#include <haven/mm.h>

struct dma_region {
	vaddr_t  virt;     /* reachable by the CPU, through the direct map */
	paddr_t  phys;     /* what the device is told */
	size_t   bytes;
	size_t   pages;
};

/*
 * Allocate `bytes` of physically contiguous memory a device can address.
 *
 * `addr_limit` is the highest physical address the device can reach:
 * PMM_ZONE_DMA32_LIMIT for a 32-bit PCI device, 0 for no limit.  Getting
 * this wrong does not fail -- the device simply DMAs to a truncated
 * address, corrupting whatever is there, which is why the limit is a
 * required argument rather than an optional hint.
 */
int  dma_alloc_coherent(struct dma_region *out, size_t bytes,
                        paddr_t addr_limit);
void dma_free_coherent(struct dma_region *region);

/*
 * Is `addr` already safe to hand to a device?
 *
 * True only for an address inside the direct map, which is where
 * PMM-allocated memory appears and where virt_to_phys() is meaningful.
 * A driver can use this to skip the bounce copy when a caller happens to
 * have passed DMA-safe memory.
 */
bool dma_addr_is_safe(vaddr_t addr, size_t len, paddr_t addr_limit);

#endif /* _HAVEN_DMA_H */
