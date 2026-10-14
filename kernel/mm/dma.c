/* SPDX-License-Identifier: MIT
 *
 * dma.c — allocating memory devices can reach.
 */
#include <haven/dma.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/vmlayout.h>

int dma_alloc_coherent(struct dma_region *out, size_t bytes,
                       paddr_t addr_limit)
{
	if (!out || bytes == 0)
		return -HAVEN_EINVAL;

	memset(out, 0, sizeof(*out));

	size_t pages = DIV_ROUND_UP(bytes, HAVEN_PAGE_SIZE);

	/* Pick the zone from the device's addressing limit rather than
	   making the caller translate a limit into a zone name: the limit
	   is what a datasheet states. */
	enum pmm_zone zone = PMM_ZONE_NORMAL;
	if (addr_limit && addr_limit <= PMM_ZONE_DMA_LIMIT)
		zone = PMM_ZONE_DMA;
	else if (addr_limit && addr_limit <= PMM_ZONE_DMA32_LIMIT)
		zone = PMM_ZONE_DMA32;

	paddr_t phys = pmm_alloc_contig(pages, zone);
	if (!phys) {
		kerr("dma: cannot allocate %zu KiB of contiguous memory below "
		     "0x%llx", bytes / 1024, (unsigned long long)addr_limit);
		return -HAVEN_ENOMEM;
	}

	/* The allocator honours the zone, but check anyway: handing a
	   device an address it cannot reach produces silent corruption
	   rather than an error, so it is worth one comparison. */
	if (addr_limit && phys + (uint64_t)pages * HAVEN_PAGE_SIZE > addr_limit) {
		kerr("dma: the allocator returned 0x%llx, which a device "
		     "limited to 0x%llx cannot reach",
		     (unsigned long long)phys,
		     (unsigned long long)addr_limit);
		pmm_free_contig(phys, pages);
		return -HAVEN_ENOMEM;
	}

	out->phys  = phys;
	out->virt  = arch_phys_to_virt(phys);
	out->bytes = pages * HAVEN_PAGE_SIZE;
	out->pages = pages;

	/* Zeroed, because a descriptor ring with stale contents is a ring
	   the device may start processing the instant it is published. */
	memset((void *)out->virt, 0, out->bytes);

	return 0;
}

void dma_free_coherent(struct dma_region *region)
{
	if (!region || !region->phys)
		return;

	pmm_free_contig(region->phys, region->pages);
	memset(region, 0, sizeof(*region));
}

bool dma_addr_is_safe(vaddr_t addr, size_t len, paddr_t addr_limit)
{
	/* Only the direct map qualifies.  An address in the kernel arena or
	   in the image window is either not physically contiguous or has no
	   stable physical address, and in both cases asking for one is the
	   bug this function exists to prevent. */
	if (addr < HHDM_BASE || addr >= HHDM_BASE + HHDM_SIZE)
		return false;
	if (len > HHDM_SIZE - (addr - HHDM_BASE))
		return false;

	if (addr_limit) {
		paddr_t phys = addr - HHDM_BASE;
		if (phys + len > addr_limit)
			return false;
	}

	return true;
}
