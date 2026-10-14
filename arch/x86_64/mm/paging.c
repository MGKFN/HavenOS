/* SPDX-License-Identifier: MIT
 *
 * paging.c — x86_64 four-level page tables.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS ran with paging off: a flat GDT covering 4 GiB, read-write and
 * executable, shared by everything.  Three consequences follow directly,
 * and all three are listed as limitations in its own documentation or
 * source:
 *
 *   - no user/kernel separation was possible, because nothing in the
 *     hardware distinguished them;
 *   - its PMM could and did hand out pages holding live kernel BSS,
 *     because a physical address was the only kind of address there was;
 *   - a stray write into kernel code was simply a write.
 *
 * With paging, each of those becomes a fault instead of corruption.
 *
 * GRANULARITY, DELIBERATELY MIXED
 * The direct map uses 1 GiB pages where the CPU supports them and 2 MiB
 * otherwise: it covers all of RAM, so 4 KiB entries would cost hundreds of
 * page-table pages and thousands of TLB entries for no benefit.
 *
 * The kernel image is mapped with 4 KiB pages on purpose, even though
 * 2 MiB would be cheaper.  vmm_protect_kernel() has to give .text
 * read-execute, .rodata read-only-no-execute and .data read-write-no-
 * execute, and those boundaries are 4 KiB aligned by the linker script.
 * A 2 MiB mapping would force the permissions of the whole 2 MiB region
 * to the weakest of its contents, which would quietly make .text writable
 * and .data executable — exactly the protection we are trying to gain.
 */
#include <haven/arch.h>
#include <haven/mm.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/string.h>
#include <haven/errno.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/memlayout.h>
#include <haven/arch/x86_64/paging.h>
#include <haven/arch/x86_64/boot.h>

/* Hardware page-table entry bits. */
#define PTE_P       (1ULL << 0)    /* present */
#define PTE_RW      (1ULL << 1)    /* writable */
#define PTE_US      (1ULL << 2)    /* user-accessible */
#define PTE_PWT     (1ULL << 3)    /* write-through */
#define PTE_PCD     (1ULL << 4)    /* cache disable */
#define PTE_A       (1ULL << 5)    /* accessed */
#define PTE_D       (1ULL << 6)    /* dirty */
#define PTE_PS      (1ULL << 7)    /* page size: large page at this level */
#define PTE_G       (1ULL << 8)    /* global: survives a CR3 reload */
#define PTE_NX      (1ULL << 63)   /* no execute */

/* The physical address field of an entry.  Bits 51:12, so masking with
   this both extracts the address and discards every flag — including NX
   at bit 63, which a naive `entry & ~0xFFF` would leave in place and turn
   into a wildly wrong address. */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)

struct arch_pagetable {
	paddr_t  root;        /* physical address of the PML4 */
	uint64_t pt_pages;    /* intermediate tables owned by this space */
	bool     is_kernel;
};

static struct arch_pagetable kernel_pt;
static bool                  kernel_pt_ready;
static uint64_t              pagetable_pages;

static inline uint64_t *table_at(paddr_t pa)
{
	return (uint64_t *)arch_phys_to_virt(pa & PTE_ADDR_MASK);
}

/* Translate architecture-neutral protection flags into hardware bits. */
static uint64_t prot_to_pte(uint32_t prot, bool large)
{
	uint64_t bits = PTE_P;

	if (prot & VM_PROT_WRITE)
		bits |= PTE_RW;
	if (prot & VM_PROT_USER)
		bits |= PTE_US;
	if (prot & VM_ATTR_NOCACHE)
		bits |= PTE_PCD | PTE_PWT;
	else if (prot & VM_ATTR_WRITETHRU)
		bits |= PTE_PWT;
	if (prot & VM_ATTR_GLOBAL)
		bits |= PTE_G;
	if (large)
		bits |= PTE_PS;

	/* NX is the default: a mapping is non-executable unless it asks to
	   be executable.  The opposite default is how a data page ends up
	   being a usable target for code injection. */
	if (!(prot & VM_PROT_EXEC) && arch_cpu_has_nx())
		bits |= PTE_NX;

	return bits;
}

/*
 * Walk to the table one level below `table[index]`, allocating it when
 * `create` is set.  Returns NULL when the entry is absent and we were not
 * asked to create it, or when allocation fails.
 *
 * `*split_large` is set when the walk hit a large page, which the caller
 * must handle rather than silently descending into what is actually a
 * data page.
 */
static uint64_t *next_level(uint64_t *table, unsigned index, bool create,
                            bool *hit_large, struct arch_pagetable *pt)
{
	uint64_t e = table[index];

	if (hit_large)
		*hit_large = false;

	if (e & PTE_P) {
		if (e & PTE_PS) {
			/* A large page is a leaf; there is no level below it.
			   Treating its address as a table would interpret
			   user data as page-table entries. */
			if (hit_large)
				*hit_large = true;
			return NULL;
		}
		return table_at(e);
	}

	if (!create)
		return NULL;

	paddr_t fresh = pmm_alloc_page_zeroed();
	if (!fresh)
		return NULL;

	struct page *pg = pmm_page_for(fresh);
	if (pg)
		pg->flags |= PAGE_FLAG_PGTABLE;

	pagetable_pages++;
	if (pt)
		pt->pt_pages++;

	/* Intermediate entries are permissive: the effective permission of a
	   mapping is the AND of every level, so restricting here would cap
	   what a leaf can grant.  The leaf carries the real permissions.
	   User-accessibility must be granted at every level, though, so it
	   is set when the target is a user address. */
	uint64_t bits = PTE_P | PTE_RW;
	table[index] = (fresh & PTE_ADDR_MASK) | bits;

	return table_at(fresh);
}

/* Set the US bit along the walk for a user mapping.  Called after the
   tables exist, because a table shared with a kernel mapping must not
   lose the bit if the user mapping is removed later — the AND semantics
   mean an over-permissive intermediate level is harmless, while a
   missing US bit makes the leaf unreachable from ring 3. */
static void grant_user_path(uint64_t *pml4, vaddr_t va)
{
	uint64_t *l4 = pml4;
	l4[PML4_INDEX(va)] |= PTE_US;

	uint64_t *l3 = table_at(l4[PML4_INDEX(va)]);
	l3[PDPT_INDEX(va)] |= PTE_US;
	if (l3[PDPT_INDEX(va)] & PTE_PS)
		return;

	uint64_t *l2 = table_at(l3[PDPT_INDEX(va)]);
	l2[PD_INDEX(va)] |= PTE_US;
}

/*
 * Decide what to do when a mapping request lands on an entry that is
 * already present.
 *
 * Re-mapping the SAME physical page is not an error and must not be one.
 * Firmware memory maps routinely describe adjacent regions whose ends and
 * starts round to the same page -- QEMU reports usable memory ending at
 * 0x9FC00 and reserved memory starting at 0x9FC00, which round to the same
 * 4 KiB page -- so building the direct map hits this on every boot.  The
 * first version of this file returned -EEXIST and the direct map lost
 * three regions, including the one holding the kernel image.
 *
 * Re-mapping a DIFFERENT physical page is a genuine conflict: silently
 * overwriting it would detach whatever was there, with no way for the
 * previous owner to find out.
 */
static int resolve_existing(uint64_t entry, paddr_t pa, vaddr_t va)
{
	if ((entry & PTE_ADDR_MASK) == (pa & PTE_ADDR_MASK))
		return 0;   /* idempotent: already mapped to the same page */

	kerr("paging: va 0x%llx is already mapped to physical 0x%llx; "
	     "refusing to re-point it at 0x%llx",
	     (unsigned long long)va,
	     (unsigned long long)(entry & PTE_ADDR_MASK),
	     (unsigned long long)pa);
	return -HAVEN_EEXIST;
}

static int map_one(struct arch_pagetable *pt, vaddr_t va, paddr_t pa,
                   size_t page_size, uint32_t prot)
{
	uint64_t *l4 = table_at(pt->root);
	bool      large_hit = false;

	uint64_t *l3 = next_level(l4, PML4_INDEX(va), true, &large_hit, pt);
	if (!l3)
		return large_hit ? -HAVEN_EEXIST : -HAVEN_ENOMEM;

	if (page_size == PAGE_SIZE_1G) {
		unsigned i = PDPT_INDEX(va);
		if (l3[i] & PTE_P)
			return resolve_existing(l3[i], pa, va);
		l3[i] = (pa & PTE_ADDR_MASK) | prot_to_pte(prot, true);
		if (prot & VM_PROT_USER)
			grant_user_path(l4, va);
		return 0;
	}

	uint64_t *l2 = next_level(l3, PDPT_INDEX(va), true, &large_hit, pt);
	if (!l2) {
		/* The walk hit a 1 GiB page.  If that page already covers the
		   physical address being requested, the request is satisfied
		   and the smaller mapping is unnecessary -- which is the
		   common case when the direct map was built with 1 GiB pages
		   and a later, smaller region falls inside one. */
		if (large_hit) {
			uint64_t e = l3[PDPT_INDEX(va)];
			paddr_t covered = (e & PTE_ADDR_MASK) +
			                  (va & (PAGE_SIZE_1G - 1));
			return covered == pa ? 0 : -HAVEN_EEXIST;
		}
		return -HAVEN_ENOMEM;
	}

	if (page_size == PAGE_SIZE_2M) {
		unsigned i = PD_INDEX(va);
		if (l2[i] & PTE_P)
			return resolve_existing(l2[i], pa, va);
		l2[i] = (pa & PTE_ADDR_MASK) | prot_to_pte(prot, true);
		if (prot & VM_PROT_USER)
			grant_user_path(l4, va);
		return 0;
	}

	uint64_t *l1 = next_level(l2, PD_INDEX(va), true, &large_hit, pt);
	if (!l1) {
		/* Same reasoning as above, one level down: a 2 MiB page that
		   already covers this physical address satisfies the request. */
		if (large_hit) {
			uint64_t e = l2[PD_INDEX(va)];
			paddr_t covered = (e & PTE_ADDR_MASK) +
			                  (va & (PAGE_SIZE_2M - 1));
			return covered == pa ? 0 : -HAVEN_EEXIST;
		}
		return -HAVEN_ENOMEM;
	}

	unsigned i = PT_INDEX(va);
	if (l1[i] & PTE_P) {
		int rc = resolve_existing(l1[i], pa, va);
		if (rc != 0)
			return rc;
	}

	l1[i] = (pa & PTE_ADDR_MASK) | prot_to_pte(prot, false);
	if (prot & VM_PROT_USER)
		grant_user_path(l4, va);

	return 0;
}

int arch_pt_map(struct arch_pagetable *pt, vaddr_t va, paddr_t pa,
                size_t len, uint32_t prot)
{
	if (!pt)
		pt = arch_pt_kernel();
	if (len == 0)
		return 0;

	if (!IS_ALIGNED(va, HAVEN_PAGE_SIZE) || !IS_ALIGNED(pa, HAVEN_PAGE_SIZE)) {
		kerr("paging: map of va 0x%llx -> pa 0x%llx is not page aligned",
		     (unsigned long long)va, (unsigned long long)pa);
		return -HAVEN_EINVAL;
	}
	if (!is_canonical(va)) {
		/* A non-canonical address cannot be mapped; the CPU faults on
		   any access to one.  Rejecting it here turns a caller's
		   arithmetic error into -EINVAL instead of a #GP later. */
		kerr("paging: va 0x%llx is not canonical",
		     (unsigned long long)va);
		return -HAVEN_EINVAL;
	}

	len = ALIGN_UP(len, HAVEN_PAGE_SIZE);

	size_t done = 0;
	while (done < len) {
		size_t remaining = len - done;
		size_t psize = arch_page_size_for(va + done, pa + done,
		                                  remaining);

		int rc = map_one(pt, va + done, pa + done, psize, prot);
		if (rc != 0) {
			/* Unwind what this call mapped, so a partial failure
			   does not leave a half-built mapping behind that the
			   caller has no way to describe. */
			if (done)
				arch_pt_unmap(pt, va, done);
			kerr("paging: mapping va 0x%llx failed at offset "
			     "0x%zx: %s",
			     (unsigned long long)va, done, haven_strerror(rc));
			return rc;
		}
		done += psize;
	}

	return 0;
}

/* Find the leaf entry for `va`, reporting the level it was found at. */
static uint64_t *find_leaf(struct arch_pagetable *pt, vaddr_t va,
                           size_t *page_size)
{
	uint64_t *l4 = table_at(pt->root);
	uint64_t  e4 = l4[PML4_INDEX(va)];
	if (!(e4 & PTE_P))
		return NULL;

	uint64_t *l3 = table_at(e4);
	uint64_t  e3 = l3[PDPT_INDEX(va)];
	if (!(e3 & PTE_P))
		return NULL;
	if (e3 & PTE_PS) {
		if (page_size)
			*page_size = PAGE_SIZE_1G;
		return &l3[PDPT_INDEX(va)];
	}

	uint64_t *l2 = table_at(e3);
	uint64_t  e2 = l2[PD_INDEX(va)];
	if (!(e2 & PTE_P))
		return NULL;
	if (e2 & PTE_PS) {
		if (page_size)
			*page_size = PAGE_SIZE_2M;
		return &l2[PD_INDEX(va)];
	}

	uint64_t *l1 = table_at(e2);
	if (!(l1[PT_INDEX(va)] & PTE_P))
		return NULL;

	if (page_size)
		*page_size = PAGE_SIZE_4K;
	return &l1[PT_INDEX(va)];
}

int arch_pt_unmap(struct arch_pagetable *pt, vaddr_t va, size_t len)
{
	if (!pt)
		pt = arch_pt_kernel();
	if (len == 0)
		return 0;

	len = ALIGN_UP(len, HAVEN_PAGE_SIZE);
	va  = ALIGN_DOWN(va, HAVEN_PAGE_SIZE);

	size_t done = 0;
	while (done < len) {
		size_t    psize = HAVEN_PAGE_SIZE;
		uint64_t *leaf  = find_leaf(pt, va + done, &psize);

		if (!leaf) {
			/* Unmapping something that is not mapped is not an
			   error: teardown paths legitimately cover ranges
			   that were never fully populated. */
			done += HAVEN_PAGE_SIZE;
			continue;
		}

		*leaf = 0;
		arch_tlb_flush_page(va + done);
		done += psize;
	}

	/* Intermediate tables are deliberately not freed here.  Freeing an
	   empty table requires scanning all 512 entries on every unmap, and
	   an address space that mapped a region once is likely to map it
	   again.  They are reclaimed wholesale by arch_pt_destroy(). */
	return 0;
}

int arch_pt_protect(struct arch_pagetable *pt, vaddr_t va, size_t len,
                    uint32_t prot)
{
	if (!pt)
		pt = arch_pt_kernel();
	if (len == 0)
		return 0;

	len = ALIGN_UP(len, HAVEN_PAGE_SIZE);
	va  = ALIGN_DOWN(va, HAVEN_PAGE_SIZE);

	size_t done = 0;
	while (done < len) {
		size_t    psize = HAVEN_PAGE_SIZE;
		uint64_t *leaf  = find_leaf(pt, va + done, &psize);

		if (!leaf) {
			kerr("paging: protect of unmapped va 0x%llx",
			     (unsigned long long)(va + done));
			return -HAVEN_ENOENT;
		}

		/* A protect that spans only part of a large page would have
		   to split it; refuse rather than silently applying the new
		   permissions to the whole large page, which is how a
		   read-only request ends up making 2 MiB of unrelated memory
		   read-only. */
		if (psize > len - done) {
			kerr("paging: protect of 0x%llx+0x%zx would change "
			     "only part of a %zu KiB page; map this range "
			     "with 4 KiB pages instead",
			     (unsigned long long)(va + done), len - done,
			     psize / 1024);
			return -HAVEN_EINVAL;
		}

		paddr_t pa = *leaf & PTE_ADDR_MASK;
		*leaf = pa | prot_to_pte(prot, psize != PAGE_SIZE_4K);
		arch_tlb_flush_page(va + done);

		done += psize;
	}

	return 0;
}

paddr_t arch_pt_translate(struct arch_pagetable *pt, vaddr_t va)
{
	if (!pt)
		pt = arch_pt_kernel();

	size_t    psize = HAVEN_PAGE_SIZE;
	uint64_t *leaf  = find_leaf(pt, va, &psize);
	if (!leaf)
		return 0;

	/* The offset within the page comes from the page's own size, not
	   from 4 KiB: a 2 MiB mapping's low 21 bits are all offset. */
	paddr_t base   = *leaf & PTE_ADDR_MASK;
	paddr_t offset = va & (psize - 1);

	return base + offset;
}

/* ── Address-space lifecycle ─────────────────────────────────────────── */

struct arch_pagetable *arch_pt_kernel(void)
{
	HAVEN_ASSERT_MSG(kernel_pt_ready,
	                 "arch_pt_kernel() before arch_pt_init_kernel()");
	return &kernel_pt;
}

struct arch_pagetable *arch_pt_create(void)
{
	struct arch_pagetable *pt = kzalloc(sizeof(*pt));
	if (!pt)
		return NULL;

	pt->root = pmm_alloc_page_zeroed();
	if (!pt->root) {
		kfree(pt);
		return NULL;
	}
	pagetable_pages++;
	pt->pt_pages = 1;

	/* Share the kernel half.  Copying the top 256 PML4 entries means
	   every address space sees the same kernel mappings, so a syscall
	   or an interrupt taken in a user process can reach kernel code
	   without a CR3 switch.  Only the top half is copied, so the user
	   half starts empty and one process cannot see another's memory. */
	uint64_t *dst = table_at(pt->root);
	uint64_t *src = table_at(kernel_pt.root);
	for (unsigned i = 256; i < PT_ENTRIES; i++)
		dst[i] = src[i];

	return pt;
}

/* Recursively free the intermediate tables of the user half.  The kernel
   half is shared and must never be freed from a process teardown — doing
   so would unmap the kernel out from under every other address space. */
static void free_table(paddr_t table_pa, int level)
{
	uint64_t *t = table_at(table_pa);

	if (level > 1) {
		for (unsigned i = 0; i < PT_ENTRIES; i++) {
			uint64_t e = t[i];
			if (!(e & PTE_P) || (e & PTE_PS))
				continue;
			free_table(e & PTE_ADDR_MASK, level - 1);
		}
	}

	pmm_free_page(table_pa);
	if (pagetable_pages)
		pagetable_pages--;
}

void arch_pt_destroy(struct arch_pagetable *pt)
{
	if (!pt || pt->is_kernel)
		return;

	uint64_t *l4 = table_at(pt->root);

	/* User half only: indices 0..255. */
	for (unsigned i = 0; i < 256; i++) {
		uint64_t e = l4[i];
		if (!(e & PTE_P) || (e & PTE_PS))
			continue;
		free_table(e & PTE_ADDR_MASK, 3);
	}

	pmm_free_page(pt->root);
	if (pagetable_pages)
		pagetable_pages--;

	kfree(pt);
}

void arch_pt_switch(struct arch_pagetable *pt)
{
	if (!pt)
		pt = arch_pt_kernel();
	__asm__ volatile("mov %0, %%cr3" :: "r"(pt->root) : "memory");
}

paddr_t arch_pt_root(const struct arch_pagetable *pt)
{
	return pt ? pt->root : 0;
}

uint64_t arch_pt_page_count(void)
{
	return pagetable_pages;
}

/* ── TLB ─────────────────────────────────────────────────────────────── */

void arch_tlb_flush_page(vaddr_t va)
{
	__asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

void arch_tlb_flush_all(void)
{
	/* Reloading CR3 flushes every non-global entry.  Global entries
	   (the kernel's, which carry PTE_G) survive, which is the point of
	   marking them global — they do not need reloading on a process
	   switch. */
	uint64_t cr3;
	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	__asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* ── Kernel address space construction ───────────────────────────────
   The boot tables from boot.S cover only the first 4 GiB and are built
   with a fixed layout.  This replaces them with tables sized to the
   actual machine: the whole of RAM in the direct map, the kernel image at
   4 KiB granularity, and no identity map at all. */

void arch_pt_init_kernel(void)
{
	kernel_pt.root = pmm_alloc_page_zeroed();
	if (!kernel_pt.root)
		panic("paging: cannot allocate the kernel PML4");

	kernel_pt.is_kernel = true;
	kernel_pt.pt_pages  = 1;
	pagetable_pages++;
	kernel_pt_ready = true;
}

int arch_pt_map_hhdm(paddr_t pa, size_t len)
{
	/* The direct map is global (it never changes on a process switch)
	   and non-executable (nothing is ever executed through it; code is
	   reached through the kernel image window). */
	return arch_pt_map(&kernel_pt, HHDM_BASE + pa, pa, len,
	                   VM_PROT_READ | VM_PROT_WRITE | VM_ATTR_GLOBAL);
}

int arch_pt_map_kernel_image(paddr_t phys_start, paddr_t phys_end)
{
	/* 4 KiB granularity, deliberately: see the header comment. */
	size_t len = phys_end - phys_start;

	for (size_t off = 0; off < len; off += HAVEN_PAGE_SIZE) {
		int rc = arch_pt_map(&kernel_pt,
		                     KERNEL_VMA_BASE + phys_start + off,
		                     phys_start + off,
		                     HAVEN_PAGE_SIZE,
		                     VM_PROT_READ | VM_PROT_WRITE |
		                     VM_PROT_EXEC | VM_ATTR_GLOBAL);
		if (rc != 0)
			return rc;
	}
	return 0;
}

void arch_pt_activate_kernel(void)
{
	arch_pt_switch(&kernel_pt);
}

void arch_pt_drop_identity_map(void)
{
	/* With the new tables active, the low half holds nothing the kernel
	   needs.  Clearing it means a kernel pointer bug that produces a
	   small address faults immediately instead of reading physical
	   memory through a stale identity mapping — which, with the boot
	   tables, is exactly what would have happened. */
	uint64_t *l4 = table_at(kernel_pt.root);
	for (unsigned i = 0; i < 256; i++)
		l4[i] = 0;
	arch_tlb_flush_all();
}
