/* SPDX-License-Identifier: MIT
 *
 * phys.c — physical/virtual translation through the direct map.
 *
 * Both directions are pure arithmetic, which is the whole point of having
 * a direct map: the PMM, every DMA-capable driver and the page-table code
 * itself need to touch a page they know only by physical address, and here
 * that costs one addition and cannot fail.
 */
#include <haven/arch.h>
#include <haven/panic.h>
#include <haven/arch/x86_64/memlayout.h>
#include <haven/arch/x86_64/cpu.h>

vaddr_t arch_phys_to_virt(paddr_t pa)
{
	/* A physical address above the direct-map window would alias into
	   the kernel heap if we just added the base, which would corrupt
	   unrelated mappings in a way that is very hard to trace back.
	   64 TiB is far beyond any machine HavenOS will see, so reaching
	   this is a bug in the caller, not a configuration to support. */
	HAVEN_ASSERT_MSG(pa < HHDM_SIZE,
	                 "phys_to_virt: 0x%llx is outside the %llu TiB direct map",
	                 (unsigned long long)pa,
	                 (unsigned long long)(HHDM_SIZE >> 40));

	return HHDM_BASE + pa;
}

paddr_t arch_virt_to_phys(vaddr_t va)
{
	/* Two kernel windows map physical memory with a fixed offset: the
	   direct map and the kernel image window.  Anything else (the heap
	   arena, MMIO, a user address) has no constant relationship to
	   physical memory and must go through arch_pt_translate(). */
	if (va >= HHDM_BASE && va < HHDM_BASE + HHDM_SIZE)
		return va - HHDM_BASE;

	if (va >= KERNEL_VMA_BASE)
		return va - KERNEL_VMA_BASE;

	PANIC("virt_to_phys: 0x%llx is not in a fixed-offset kernel window; "
	      "use arch_pt_translate() for mapped memory",
	      (unsigned long long)va);
}

size_t arch_page_size_for(vaddr_t va, paddr_t pa, size_t len)
{
	bool huge_1g = arch_cpu_has_1g_pages();

	/* A large page requires the virtual address, the physical address
	   and the remaining length all to be aligned to it.  Checking only
	   the virtual address is a classic way to produce a mapping that is
	   silently off by a fraction of a page. */
	if (huge_1g &&
	    IS_ALIGNED(va, PAGE_SIZE_1G) &&
	    IS_ALIGNED(pa, PAGE_SIZE_1G) &&
	    len >= PAGE_SIZE_1G)
		return PAGE_SIZE_1G;

	if (IS_ALIGNED(va, PAGE_SIZE_2M) &&
	    IS_ALIGNED(pa, PAGE_SIZE_2M) &&
	    len >= PAGE_SIZE_2M)
		return PAGE_SIZE_2M;

	return PAGE_SIZE_4K;
}
