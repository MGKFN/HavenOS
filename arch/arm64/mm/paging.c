/* SPDX-License-Identifier: MIT
 *
 * paging.c — AArch64 translation tables.
 *
 * STATUS: skeleton.  The two address translations are implemented,
 * because they are pure arithmetic over the layout in memlayout.h and
 * are what the PMM needs.  The table walker is not, and panics rather
 * than returning a wrong answer.
 *
 * WHAT DIFFERS FROM x86_64, FOR WHOEVER FINISHES THIS
 *   - two roots, not one.  TTBR0_EL1 translates user addresses and
 *     TTBR1_EL1 kernel addresses, so a process switch writes TTBR0 only
 *     and never invalidates a kernel TLB entry.  x86_64 needs the global
 *     bit to approximate this.
 *   - permissions are encoded differently and some are INVERTED: AP[2]
 *     is a read-only bit (not a write bit), and UXN/PXN are
 *     never-execute bits per privilege level.  A direct translation of
 *     prot_to_pte() will be wrong in a way that grants more access than
 *     intended, which is the dangerous direction.
 *   - the access flag (AF) must be set explicitly or every access faults,
 *     which is a very confusing first symptom.
 *   - memory type comes from an index into MAIR_EL1 rather than from
 *     individual cache bits, so MAIR_EL1 must be programmed before any
 *     mapping is made.
 *   - TLB maintenance is explicit and scoped: tlbi vaae1is for one page,
 *     tlbi vmalle1is for all, each followed by dsb and isb.  Omitting the
 *     isb lets the CPU continue with a stale translation already in
 *     flight.
 */
#include <haven/arch.h>
#include <haven/mm.h>
#include <haven/panic.h>
#include <haven/arch/arm64/memlayout.h>

#define NOT_YET(what) \
	panic("arm64: " what " is not implemented; see docs/PORTING_ARM64.md")

/* ── Implemented: the direct map ─────────────────────────────────────── */

vaddr_t arch_phys_to_virt(paddr_t pa)
{
	HAVEN_ASSERT_MSG(pa < HHDM_SIZE,
	                 "phys_to_virt: 0x%llx is outside the direct map",
	                 (unsigned long long)pa);
	return HHDM_BASE + pa;
}

paddr_t arch_virt_to_phys(vaddr_t va)
{
	if (va >= HHDM_BASE && va < HHDM_BASE + HHDM_SIZE)
		return va - HHDM_BASE;
	if (va >= KERNEL_VMA_BASE)
		return va - KERNEL_VMA_BASE + KERNEL_LMA_BASE;

	PANIC("virt_to_phys: 0x%llx is not in a fixed-offset kernel window",
	      (unsigned long long)va);
}

size_t arch_page_size_for(vaddr_t va, paddr_t pa, size_t len)
{
	/* Level-1 and level-2 blocks, the AArch64 equivalents of 1 GiB and
	   2 MiB pages.  Both are mandatory with a 4 KiB granule, so unlike
	   x86_64's 1 GiB pages there is no feature test. */
	if (IS_ALIGNED(va, PAGE_SIZE_1G) && IS_ALIGNED(pa, PAGE_SIZE_1G) &&
	    len >= PAGE_SIZE_1G)
		return PAGE_SIZE_1G;
	if (IS_ALIGNED(va, PAGE_SIZE_2M) && IS_ALIGNED(pa, PAGE_SIZE_2M) &&
	    len >= PAGE_SIZE_2M)
		return PAGE_SIZE_2M;
	return PAGE_SIZE_4K;
}

/* ── Implemented: TLB maintenance ────────────────────────────────────
   Worth implementing now because getting the barriers wrong produces a
   bug that appears much later and elsewhere. */

void arch_tlb_flush_page(vaddr_t va)
{
	/* The operand is the virtual address shifted right by 12. */
	uint64_t page = va >> 12;

	__asm__ volatile(
		"dsb ishst\n\t"          /* the table write must land first */
		"tlbi vaae1is, %0\n\t"   /* all ASIDs, inner shareable */
		"dsb ish\n\t"            /* wait for the invalidate */
		"isb"                    /* discard prefetched translations */
		:: "r"(page) : "memory");
}

void arch_tlb_flush_all(void)
{
	__asm__ volatile(
		"dsb ishst\n\t"
		"tlbi vmalle1is\n\t"
		"dsb ish\n\t"
		"isb"
		::: "memory");
}

/* ── Not yet implemented ─────────────────────────────────────────────── */

struct arch_pagetable *arch_pt_create(void)  { NOT_YET("table creation"); }
void arch_pt_destroy(struct arch_pagetable *pt) { (void)pt; NOT_YET("table teardown"); }
struct arch_pagetable *arch_pt_kernel(void)  { NOT_YET("the kernel table"); }
void arch_pt_switch(struct arch_pagetable *pt) { (void)pt; NOT_YET("TTBR0 switching"); }

int arch_pt_map(struct arch_pagetable *pt, vaddr_t va, paddr_t pa,
                size_t len, uint32_t prot)
{
	(void)pt; (void)va; (void)pa; (void)len; (void)prot;
	NOT_YET("the translation-table walker");
}

int arch_pt_unmap(struct arch_pagetable *pt, vaddr_t va, size_t len)
{
	(void)pt; (void)va; (void)len;
	NOT_YET("the translation-table walker");
}

int arch_pt_protect(struct arch_pagetable *pt, vaddr_t va, size_t len,
                    uint32_t prot)
{
	(void)pt; (void)va; (void)len; (void)prot;
	NOT_YET("the translation-table walker");
}

paddr_t arch_pt_translate(struct arch_pagetable *pt, vaddr_t va)
{
	(void)pt; (void)va;
	/* Deliberately a panic and not a 0.  A caller that received 0 would
	   treat it as "not mapped" and carry on, and the resulting bug
	   would appear far from here. */
	NOT_YET("the translation-table walker");
}

void arch_pt_init_kernel(void)        { NOT_YET("kernel table construction"); }
int  arch_pt_map_hhdm(paddr_t pa, size_t len) { (void)pa; (void)len; NOT_YET("the direct map"); }
int  arch_pt_map_kernel_image(paddr_t a, paddr_t b) { (void)a; (void)b; NOT_YET("the image mapping"); }
void arch_pt_activate_kernel(void)    { NOT_YET("TTBR1 activation"); }
void arch_pt_drop_identity_map(void)  { NOT_YET("identity-map teardown"); }
uint64_t arch_pt_page_count(void)     { return 0; }
