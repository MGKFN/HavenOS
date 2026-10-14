/* SPDX-License-Identifier: MIT
 *
 * mm.h — physical and virtual memory management.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had two allocators and both were limited by construction:
 *
 *   pmm.c   a bitmap over a fixed 2 GiB ceiling, with a linear first-fit
 *           scan from a reserved boundary.  Its own comments record the
 *           boundary being wrong twice — a hard-coded 1 MiB, then a
 *           hard-coded 4 MiB against a 6.31 MiB image — each time leaving
 *           live kernel BSS inside the free pool.  The v0.7.18 fix was to
 *           derive the boundary from the linker's _kernel_end.
 *   heap.c   a 256 KiB *static array* with a first-fit free list.  Every
 *           kmalloc() in the system competed for that quarter-megabyte,
 *           and it could never grow, so the big buffers had to be moved
 *           onto pmm with pmm_alloc_contig() in v0.7.18 — which is what
 *           made the pmm boundary bug live for the first time.
 *
 * HavenOS replaces both:
 *
 *   PMM      a buddy allocator over a struct page array covering all RAM,
 *            with zones so a 32-bit DMA device can be given memory it can
 *            actually address.  Reserved regions come from the firmware
 *            memory map and from the linker's own image bounds, so the
 *            reserve is computed and cannot drift from the image size.
 *   VMM      a real virtual address space: the kernel heap is a 16 TiB
 *            arena backed on demand, so a large allocation costs address
 *            space rather than competing for a fixed pool.
 *   kmalloc  size-class caches over the VMM, falling through to whole
 *            pages for large requests.
 *
 * The magic-value corruption detection HavenDOS added to its heap in
 * v0.6.8 was a good idea and is kept — see kernel/mm/kmalloc.c.
 */
#ifndef _HAVEN_MM_H
#define _HAVEN_MM_H

#include <haven/types.h>
#include <haven/list.h>
#include <haven/boot.h>

/* ───────────────────────── Physical memory ─────────────────────────── */

/* Buddy orders: order n is 2^n contiguous pages.  MAX_ORDER 11 means the
   largest single allocation is 2^10 pages = 4 MiB, which covers every
   contiguous requirement HavenOS has (page tables, DMA rings, the
   framebuffer shadow) without the free lists growing unhelpfully long. */
#define PMM_MAX_ORDER 11

/* Zones exist because not every device can address all of memory.  A
   32-bit PCI device given a page above 4 GiB will DMA into the wrong
   place, and the failure appears as data corruption rather than as an
   allocation error — so the constraint belongs in the allocator. */
enum pmm_zone {
	PMM_ZONE_DMA = 0,    /* below 16 MiB: ISA DMA */
	PMM_ZONE_DMA32,      /* below 4 GiB:  32-bit PCI */
	PMM_ZONE_NORMAL,     /* everything else */
	PMM_ZONE_COUNT
};

#define PMM_ZONE_DMA_LIMIT    (16ULL * 1024 * 1024)
#define PMM_ZONE_DMA32_LIMIT  (4ULL * 1024 * 1024 * 1024)

/* Per-page metadata.  One of these exists for every page of RAM, which
   is what lets the allocator answer "is this page free, and if it is part
   of a larger block, how large" without a search — and what gives pages
   a reference count, so a page can be shared between address spaces. */
#define PAGE_FLAG_FREE     (1u << 0)
#define PAGE_FLAG_RESERVED (1u << 1)   /* firmware or kernel image */
#define PAGE_FLAG_BUDDY    (1u << 2)   /* head of a free buddy block */
#define PAGE_FLAG_SLAB     (1u << 3)   /* owned by a kmalloc cache */
#define PAGE_FLAG_PGTABLE  (1u << 4)   /* holds a page table */

struct page {
	struct list_head node;    /* on a buddy free list, or a slab list */
	uint32_t         flags;
	uint8_t          order;   /* valid when PAGE_FLAG_BUDDY */
	uint8_t          zone;
	uint16_t         refcount;
};

void    pmm_init(const struct boot_info *bi);

/* Allocate 2^order contiguous pages from `zone` or any lower-addressed
   zone.  Returns the physical address, or 0 on failure — 0 is never a
   valid return because the first page of RAM is always reserved. */
paddr_t pmm_alloc_pages_zone(unsigned order, enum pmm_zone zone);
paddr_t pmm_alloc_pages(unsigned order);
paddr_t pmm_alloc_page(void);

/* Allocate `nr` pages, rounding up to the next power of two.  Prefer
   pmm_alloc_pages() when the count is already a power of two: this
   wrapper can waste up to half the allocation. */
paddr_t pmm_alloc_contig(size_t nr_pages, enum pmm_zone zone);

void    pmm_free_pages(paddr_t pa, unsigned order);
void    pmm_free_page(paddr_t pa);
void    pmm_free_contig(paddr_t pa, size_t nr_pages);

/* Allocate a page and zero it.  Page tables must be zeroed before use and
   forgetting to is a fault with no useful diagnostic, so the operation is
   offered as one call. */
paddr_t pmm_alloc_page_zeroed(void);

struct page *pmm_page_for(paddr_t pa);
paddr_t      pmm_page_addr(const struct page *pg);

/* Reference counting, for pages mapped into more than one address space. */
void pmm_page_get(paddr_t pa);
bool pmm_page_put(paddr_t pa);   /* true when the page was freed */

struct pmm_stats {
	uint64_t total_pages;
	uint64_t free_pages;
	uint64_t reserved_pages;
	uint64_t allocated_pages;
	uint64_t zone_free[PMM_ZONE_COUNT];
	uint64_t zone_total[PMM_ZONE_COUNT];
	uint64_t alloc_calls;
	uint64_t free_calls;
	uint64_t alloc_failures;
};

void pmm_get_stats(struct pmm_stats *out);
void pmm_dump(void);

/* Bound what the allocator may return, or 0 to lift the bound.
 *
 * Needed for exactly one window during boot: pmm_init() runs while the
 * only direct map is the fixed one the boot trampoline built, so until
 * vmm_init() extends it, any page above that window is unreachable
 * through arch_phys_to_virt() and must not be handed out.  vmm_init()
 * lifts the bound as its last act.  Nothing else should call this. */
void    pmm_set_alloc_limit(paddr_t limit);
paddr_t pmm_alloc_limit(void);

/* Walk every buddy free list and verify the invariants: every block is
   marked free, aligned to its order, inside RAM, and not overlapping its
   neighbours.  Returns true when the allocator is consistent.  This is
   the physical-memory counterpart of HavenDOS's heap_check(), which was
   one of the better ideas in its heap rewrite. */
bool pmm_check(void);

/* ───────────────────────── Virtual memory ──────────────────────────── */

struct arch_pagetable;

void vmm_init(const struct boot_info *bi);

/* Map / unmap in the kernel address space. */
int  vmm_map(vaddr_t va, paddr_t pa, size_t len, uint32_t prot);
int  vmm_unmap(vaddr_t va, size_t len);
int  vmm_protect(vaddr_t va, size_t len, uint32_t prot);

/* Reserve `len` bytes of kernel virtual address space and back it with
   freshly allocated physical pages.  The pages need not be contiguous,
   which is the entire point: a 4 MiB allocation does not need 4 MiB of
   adjacent physical memory, and requiring it is why HavenDOS's large
   buffers had to be contiguous runs from the PMM. */
vaddr_t vmm_alloc(size_t len, uint32_t prot);
void    vmm_free(vaddr_t va, size_t len);

/* Map a device's MMIO aperture into the kernel's device window,
   uncached.  Returns 0 on failure. */
vaddr_t vmm_map_mmio(paddr_t pa, size_t len);
void    vmm_unmap_mmio(vaddr_t va, size_t len);

/* Apply final permissions to the kernel image: .text read-execute,
   .rodata read-only and no-execute, .data/.bss read-write and
   no-execute.  Called once the boot-time mappings are no longer needed.
   HavenDOS ran entirely with a flat read-write-execute segment, so a
   stray write into its own code was simply a write. */
void vmm_protect_kernel(void);

/* A kernel stack with an unmapped guard page below it, so a stack
   overflow faults at a known address instead of silently overwriting
   whatever is adjacent.  HavenDOS's 256 KiB boot stack had no guard, and
   the console replay bug caught during this project's own bring-up — a
   64 KiB buffer on a 64 KiB stack — is exactly what a guard page turns
   from corruption into a diagnosable fault. */
vaddr_t vmm_alloc_kstack(void);
void    vmm_free_kstack(vaddr_t stack_top);

struct vmm_stats {
	uint64_t mapped_pages;
	uint64_t arena_used_bytes;
	uint64_t arena_total_bytes;
	uint64_t mmio_used_bytes;
	uint64_t pagetable_pages;
};

void vmm_get_stats(struct vmm_stats *out);

/* ───────────────────────── Kernel allocator ────────────────────────── */

void  kmalloc_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

/* Allocation size as the allocator recorded it, for realloc and for
   leak accounting.  Returns 0 for a pointer the allocator does not own. */
size_t kmalloc_size(const void *ptr);

struct kmalloc_stats {
	uint64_t live_bytes;
	uint64_t live_objects;
	uint64_t total_allocs;
	uint64_t total_frees;
	uint64_t failed_allocs;
	uint64_t slab_pages;
};

void kmalloc_get_stats(struct kmalloc_stats *out);

/* Validate every slab cache and the large-allocation list.  Keeps the
   corruption detection HavenDOS added to its heap in v0.6.8 — magic
   values on every header, with double-free and use-after-free detection
   — but over an allocator that can grow. */
bool kmalloc_check(void);
void kmalloc_dump(void);

#endif /* _HAVEN_MM_H */
