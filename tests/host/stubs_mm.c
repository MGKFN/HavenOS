/* SPDX-License-Identifier: MIT
 *
 * stubs_mm.c — allocator and page-table stubs for testing layers ABOVE
 * memory management.
 *
 * test_pmm links the real kernel/mm/pmm.c because the allocator is what
 * it is testing.  A test of the VFS or the ELF loader should not: those
 * tests want to exercise path resolution and segment validation, and
 * routing every allocation through a buddy allocator over a simulated
 * machine adds a second system under test for no benefit.
 *
 * So kmalloc here is host malloc, and the page table is a flat map from
 * virtual to simulated-physical address.  The simulated physical memory
 * is the same allocation stubs.c provides, which is what lets the real
 * elf_load() copy segment data through arch_phys_to_virt() exactly as it
 * does on hardware.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <haven/mm.h>
#include <haven/arch.h>
#include <haven/errno.h>

#include "harness.h"

/* ── Object allocator ────────────────────────────────────────────────── */

static uint64_t live_allocs;

void *kmalloc(size_t size)
{
	if (size == 0)
		return NULL;
	void *p = malloc(size);
	if (p)
		live_allocs++;
	return p;
}

void *kzalloc(size_t size)
{
	if (size == 0)
		return NULL;
	void *p = calloc(1, size);
	if (p)
		live_allocs++;
	return p;
}

void *kcalloc(size_t n, size_t size)
{
	if (n && size > SIZE_MAX / n)
		return NULL;
	return kzalloc(n * size);
}

void *krealloc(void *ptr, size_t size)
{
	if (!ptr)
		return kmalloc(size);
	if (size == 0) {
		kfree(ptr);
		return NULL;
	}
	return realloc(ptr, size);
}

void kfree(void *ptr)
{
	if (ptr) {
		if (live_allocs)
			live_allocs--;
		free(ptr);
	}
}

size_t kmalloc_size(const void *ptr) { (void)ptr; return 0; }
bool   kmalloc_check(void)           { return true; }
void   kmalloc_dump(void)            { }
void   kmalloc_init(void)            { }

void kmalloc_get_stats(struct kmalloc_stats *out)
{
	if (out) {
		memset(out, 0, sizeof(*out));
		out->live_objects = live_allocs;
	}
}

uint64_t haven_test_live_allocations(void) { return live_allocs; }

/* ── Virtual memory arena ────────────────────────────────────────────── */

/* ramfs allocates its data blocks with vmm_alloc(), so the stub has to
   return memory the test can actually write through. */
vaddr_t vmm_alloc(size_t len, uint32_t prot)
{
	(void)prot;
	void *p = calloc(1, len);
	return (vaddr_t)(uintptr_t)p;
}

void vmm_free(vaddr_t va, size_t len)
{
	(void)len;
	free((void *)(uintptr_t)va);
}

int  vmm_map(vaddr_t va, paddr_t pa, size_t len, uint32_t prot)
{
	(void)va; (void)pa; (void)len; (void)prot;
	return 0;
}
int  vmm_unmap(vaddr_t va, size_t len) { (void)va; (void)len; return 0; }
int  vmm_protect(vaddr_t va, size_t len, uint32_t prot)
{
	(void)va; (void)len; (void)prot;
	return 0;
}
vaddr_t vmm_map_mmio(paddr_t pa, size_t len) { (void)pa; (void)len; return 0; }
void    vmm_unmap_mmio(vaddr_t va, size_t len) { (void)va; (void)len; }
void    vmm_protect_kernel(void) { }
void    vmm_init(const struct boot_info *bi) { (void)bi; }
void    vmm_get_stats(struct vmm_stats *out)
{
	if (out)
		memset(out, 0, sizeof(*out));
}

vaddr_t vmm_alloc_kstack(void) { return (vaddr_t)(uintptr_t)calloc(1, 16384) + 16384; }
void    vmm_free_kstack(vaddr_t stack_top) { (void)stack_top; }

/* ── Physical allocator ──────────────────────────────────────────────── */

/* A bump allocator over the simulated machine, starting above where a
   test's kernel image would be.  Simple on purpose: a test of the ELF
   loader should fail because of the loader, not because of this. */
static paddr_t bump_next = 0x400000;
static uint64_t pages_handed_out;

paddr_t pmm_alloc_page(void)
{
	if (bump_next + 4096 > haven_test_ram_size())
		return 0;
	paddr_t pa = bump_next;
	bump_next += 4096;
	pages_handed_out++;
	return pa;
}

paddr_t pmm_alloc_page_zeroed(void)
{
	paddr_t pa = pmm_alloc_page();
	if (pa)
		memset((void *)arch_phys_to_virt(pa), 0, 4096);
	return pa;
}

paddr_t pmm_alloc_pages(unsigned order)
{
	size_t n = (size_t)1 << order;
	paddr_t first = 0;
	for (size_t i = 0; i < n; i++) {
		paddr_t pa = pmm_alloc_page();
		if (!i)
			first = pa;
	}
	return first;
}

paddr_t pmm_alloc_pages_zone(unsigned order, enum pmm_zone zone)
{
	(void)zone;
	return pmm_alloc_pages(order);
}

paddr_t pmm_alloc_contig(size_t nr, enum pmm_zone zone)
{
	(void)zone;
	paddr_t first = 0;
	for (size_t i = 0; i < nr; i++) {
		paddr_t pa = pmm_alloc_page();
		if (!i)
			first = pa;
	}
	return first;
}

void pmm_free_page(paddr_t pa)  { (void)pa; if (pages_handed_out) pages_handed_out--; }
void pmm_free_pages(paddr_t pa, unsigned order) { (void)pa; (void)order; }
void pmm_free_contig(paddr_t pa, size_t nr) { (void)pa; (void)nr; }
void pmm_page_get(paddr_t pa)   { (void)pa; }
bool pmm_page_put(paddr_t pa)   { (void)pa; return false; }
struct page *pmm_page_for(paddr_t pa) { (void)pa; return NULL; }
paddr_t pmm_page_addr(const struct page *pg) { (void)pg; return 0; }
bool pmm_check(void) { return true; }
void pmm_dump(void)  { }
void pmm_init(const struct boot_info *bi) { (void)bi; }
void pmm_set_alloc_limit(paddr_t limit) { (void)limit; }
paddr_t pmm_alloc_limit(void) { return 0; }

void pmm_get_stats(struct pmm_stats *out)
{
	if (out)
		memset(out, 0, sizeof(*out));
}

void haven_test_reset_pmm(void)
{
	bump_next = 0x400000;
	pages_handed_out = 0;
}

uint64_t haven_test_pages_handed_out(void) { return pages_handed_out; }

/* ── Page tables ─────────────────────────────────────────────────────
   A flat open-addressed map from virtual page to physical page, which is
   everything elf_load() needs: it maps pages and then translates them
   back to copy segment contents through the direct map. */

#define PT_SLOTS 8192

struct arch_pagetable {
	struct {
		vaddr_t  va;
		paddr_t  pa;
		uint32_t prot;
		bool     used;
	} slot[PT_SLOTS];
	size_t count;
};

static struct arch_pagetable kernel_pt_stub;

static size_t pt_hash(vaddr_t va)
{
	return (size_t)((va >> 12) * 0x9E3779B97F4A7C15ULL >> 40) % PT_SLOTS;
}

struct arch_pagetable *arch_pt_create(void)
{
	return calloc(1, sizeof(struct arch_pagetable));
}

void arch_pt_destroy(struct arch_pagetable *pt)
{
	if (pt != &kernel_pt_stub)
		free(pt);
}

struct arch_pagetable *arch_pt_kernel(void) { return &kernel_pt_stub; }
void arch_pt_switch(struct arch_pagetable *pt) { (void)pt; }

int arch_pt_map(struct arch_pagetable *pt, vaddr_t va, paddr_t pa,
                size_t len, uint32_t prot)
{
	if (!pt)
		pt = &kernel_pt_stub;

	for (size_t off = 0; off < len; off += 4096) {
		vaddr_t v = va + off;
		size_t  h = pt_hash(v);

		for (size_t probe = 0; probe < PT_SLOTS; probe++) {
			size_t i = (h + probe) % PT_SLOTS;
			if (pt->slot[i].used && pt->slot[i].va == v) {
				if (pt->slot[i].pa != pa + off)
					return -HAVEN_EEXIST;
				goto next;
			}
			if (!pt->slot[i].used) {
				pt->slot[i].used = true;
				pt->slot[i].va   = v;
				pt->slot[i].pa   = pa + off;
				pt->slot[i].prot = prot;
				pt->count++;
				goto next;
			}
		}
		return -HAVEN_ENOMEM;
next:
		continue;
	}
	return 0;
}

int arch_pt_unmap(struct arch_pagetable *pt, vaddr_t va, size_t len)
{
	if (!pt)
		pt = &kernel_pt_stub;

	for (size_t off = 0; off < len; off += 4096) {
		vaddr_t v = va + off;
		size_t  h = pt_hash(v);
		for (size_t probe = 0; probe < PT_SLOTS; probe++) {
			size_t i = (h + probe) % PT_SLOTS;
			if (!pt->slot[i].used)
				break;
			if (pt->slot[i].va == v) {
				pt->slot[i].used = false;
				if (pt->count)
					pt->count--;
				break;
			}
		}
	}
	return 0;
}

int arch_pt_protect(struct arch_pagetable *pt, vaddr_t va, size_t len,
                    uint32_t prot)
{
	(void)pt; (void)va; (void)len; (void)prot;
	return 0;
}

paddr_t arch_pt_translate(struct arch_pagetable *pt, vaddr_t va)
{
	if (!pt)
		pt = &kernel_pt_stub;

	vaddr_t page   = va & ~(vaddr_t)4095;
	vaddr_t offset = va & 4095;
	size_t  h      = pt_hash(page);

	for (size_t probe = 0; probe < PT_SLOTS; probe++) {
		size_t i = (h + probe) % PT_SLOTS;
		if (!pt->slot[i].used)
			return 0;
		if (pt->slot[i].va == page)
			return pt->slot[i].pa + offset;
	}
	return 0;
}

uint32_t haven_test_pt_prot(struct arch_pagetable *pt, vaddr_t va)
{
	vaddr_t page = va & ~(vaddr_t)4095;
	size_t  h    = pt_hash(page);
	for (size_t probe = 0; probe < PT_SLOTS; probe++) {
		size_t i = (h + probe) % PT_SLOTS;
		if (!pt->slot[i].used)
			return 0;
		if (pt->slot[i].va == page)
			return pt->slot[i].prot;
	}
	return 0;
}

size_t haven_test_pt_count(struct arch_pagetable *pt)
{
	return pt ? pt->count : 0;
}

void arch_pt_init_kernel(void) { }
int  arch_pt_map_hhdm(paddr_t pa, size_t len) { (void)pa; (void)len; return 0; }
int  arch_pt_map_kernel_image(paddr_t a, paddr_t b) { (void)a; (void)b; return 0; }
void arch_pt_activate_kernel(void) { }
void arch_pt_drop_identity_map(void) { }
uint64_t arch_pt_page_count(void) { return 0; }

/* The kernel's linker-script bounds symbols are deliberately NOT defined
   here.  Only kernel/mm/vmm.c references them, vmm.c is stubbed out in
   these tests, and defining __data_start would collide with the one glibc
   already provides in Scrt1.o. */
