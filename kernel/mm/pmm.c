/* SPDX-License-Identifier: MIT
 *
 * pmm.c — buddy physical page allocator.
 *
 * WHY A BUDDY ALLOCATOR
 * HavenDOS's PMM was a bitmap with a linear first-fit scan, and its
 * contiguous-run allocator scanned the whole bitmap looking for a run of
 * free bits.  Its own comment conceded the cost: "A first-fit scan is
 * plenty: these are allocated a handful of times per boot, never in a
 * loop."  That is true of a system with no processes.  Once page faults,
 * thread stacks, page tables and slab growth all allocate, a linear scan
 * over a million bits per allocation is the wrong shape entirely.
 *
 * A buddy allocator gives O(log n) allocation and free, coalesces
 * automatically so fragmentation does not accumulate, and makes
 * contiguous multi-page allocation the same operation as single-page
 * allocation rather than a separate scan.
 *
 * THE RESERVE BOUNDARY
 * HavenDOS got this wrong twice, with a hard-coded constant that was
 * smaller than the kernel image both times, so the allocator's free pool
 * included the kernel's own live BSS.  HavenOS never states a reserve
 * size: every reserved page is marked from the firmware memory map and
 * from the linker's own __kernel_phys_start/__kernel_phys_end, and
 * pmm_check() asserts that no free block overlaps a reserved page.
 */
#include <haven/mm.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/string.h>
#include <haven/printf.h>

/* One free list per order, per zone.  Keeping the lists per-zone means a
   DMA allocation does not have to walk past pages it cannot use. */
static struct list_head free_lists[PMM_ZONE_COUNT][PMM_MAX_ORDER];

/* The page metadata array, covering PFN 0..max_pfn.  Allocated during
   init by a bump allocator over the firmware memory map, because the
   buddy allocator cannot allocate its own metadata. */
static struct page *mem_map;
static uint64_t     max_pfn;

static struct pmm_stats stats;

/*
 * Upper bound on what the allocator may hand out, or 0 for "no bound".
 *
 * This exists because of a strict ordering dependency that is easy to get
 * wrong and fatal when it is.  Every page the PMM touches, it touches
 * through arch_phys_to_virt() -- to zero a page, to write page metadata,
 * to build a page table.  But at the moment pmm_init() runs, the only
 * direct map that exists is the one boot.S built, which covers the first
 * 4 GiB, because the trampoline cannot know how much RAM the machine has
 * before the firmware memory map has been parsed.
 *
 * On a machine with more than 4 GiB, the NORMAL zone is entirely above
 * that window.  Without this bound the very first page-table allocation
 * in vmm_init() would return a page above 4 GiB, arch_phys_to_virt()
 * would hand back an address with nothing mapped at it, and the kernel
 * would fault while building the page tables that would have mapped it.
 *
 * So pmm_init() sets the bound to the boot direct map's extent, and
 * vmm_init() clears it with pmm_set_alloc_limit(0) once the real direct
 * map covers all of RAM.
 */
static paddr_t early_limit;

/* The PMM is called from every CPU and from interrupt context (a page
   fault allocating a page).  One lock for the whole allocator is
   adequate: allocation is not the bottleneck at HavenOS's scale, and a
   single lock is far easier to reason about than per-zone locking with a
   cross-zone fallback path. */
static volatile uint32_t pmm_lock_word;

static irqflags_t pmm_lock(void)
{
	irqflags_t f = arch_irq_save();
	while (__atomic_exchange_n(&pmm_lock_word, 1, __ATOMIC_ACQUIRE))
		arch_pause();
	return f;
}

static void pmm_unlock(irqflags_t f)
{
	__atomic_store_n(&pmm_lock_word, 0, __ATOMIC_RELEASE);
	arch_irq_restore(f);
}

static inline uint64_t pa_to_pfn(paddr_t pa)
{
	return pa >> HAVEN_PAGE_SHIFT;
}

static inline paddr_t pfn_to_pa(uint64_t pfn)
{
	return (paddr_t)pfn << HAVEN_PAGE_SHIFT;
}

static enum pmm_zone zone_for_pa(paddr_t pa)
{
	if (pa < PMM_ZONE_DMA_LIMIT)
		return PMM_ZONE_DMA;
	if (pa < PMM_ZONE_DMA32_LIMIT)
		return PMM_ZONE_DMA32;
	return PMM_ZONE_NORMAL;
}

static const char *zone_name(enum pmm_zone z)
{
	switch (z) {
	case PMM_ZONE_DMA:    return "DMA";
	case PMM_ZONE_DMA32:  return "DMA32";
	case PMM_ZONE_NORMAL: return "NORMAL";
	default:              return "?";
	}
}

struct page *pmm_page_for(paddr_t pa)
{
	uint64_t pfn = pa_to_pfn(pa);
	if (pfn >= max_pfn)
		return NULL;
	return &mem_map[pfn];
}

paddr_t pmm_page_addr(const struct page *pg)
{
	HAVEN_ASSERT(pg >= mem_map && pg < mem_map + max_pfn);
	return pfn_to_pa((uint64_t)(pg - mem_map));
}

/* ── Buddy mechanics ─────────────────────────────────────────────────
   The buddy of a block of order n starting at PFN p is at PFN p ^ (1<<n).
   Two blocks may only merge when both are free, both are the same order,
   and the merged block is still aligned to the next order — which the XOR
   guarantees, provided the whole arena is treated as starting at PFN 0.
   That is why reserved pages are marked rather than excluded: the PFN
   arithmetic stays uniform. */

static inline uint64_t buddy_pfn(uint64_t pfn, unsigned order)
{
	return pfn ^ (1ULL << order);
}

static void block_insert(uint64_t pfn, unsigned order, enum pmm_zone zone)
{
	struct page *pg = &mem_map[pfn];
	pg->flags |= PAGE_FLAG_FREE | PAGE_FLAG_BUDDY;
	pg->order  = (uint8_t)order;
	pg->zone   = (uint8_t)zone;
	list_add(&pg->node, &free_lists[zone][order]);
}

static void block_remove(struct page *pg, unsigned order, enum pmm_zone zone)
{
	(void)order;
	(void)zone;
	list_del(&pg->node);
	pg->flags &= (uint32_t)~(PAGE_FLAG_FREE | PAGE_FLAG_BUDDY);
}

/* Is the block of `order` pages starting at `pfn` entirely free, same
   order, and a legitimate merge partner? */
static bool block_is_free_buddy(uint64_t pfn, unsigned order)
{
	if (pfn >= max_pfn)
		return false;

	struct page *pg = &mem_map[pfn];
	if (!(pg->flags & PAGE_FLAG_FREE))
		return false;
	if (!(pg->flags & PAGE_FLAG_BUDDY))
		return false;
	if (pg->order != order)
		return false;
	if (pg->flags & PAGE_FLAG_RESERVED)
		return false;

	return true;
}

static void mark_allocated(uint64_t pfn, unsigned order)
{
	uint64_t n = 1ULL << order;
	for (uint64_t i = 0; i < n; i++) {
		struct page *pg = &mem_map[pfn + i];
		pg->flags &= (uint32_t)~(PAGE_FLAG_FREE | PAGE_FLAG_BUDDY);
		pg->refcount = 1;
	}
	/* Record the order on the head page so pmm_free_pages() can be
	   checked against it, and so a free with the wrong order is caught
	   rather than silently corrupting the free lists. */
	mem_map[pfn].order = (uint8_t)order;
}

/* ── Allocation ──────────────────────────────────────────────────────── */

static paddr_t alloc_from_zone(unsigned order, enum pmm_zone zone)
{
	/* Find the smallest available block that can satisfy the request. */
	for (unsigned o = order; o < PMM_MAX_ORDER; o++) {
		if (list_empty(&free_lists[zone][o]))
			continue;

		struct page *pg = list_first_entry(&free_lists[zone][o],
		                                   struct page, node);
		uint64_t pfn = (uint64_t)(pg - mem_map);
		block_remove(pg, o, zone);

		/* Split down to the requested order, putting each discarded
		   upper half back on its own free list. */
		while (o > order) {
			o--;
			block_insert(buddy_pfn(pfn, o), o, zone);
		}

		mark_allocated(pfn, order);

		uint64_t n = 1ULL << order;
		stats.free_pages      -= n;
		stats.allocated_pages += n;
		stats.zone_free[zone] -= n;

		return pfn_to_pa(pfn);
	}
	return 0;
}

paddr_t pmm_alloc_pages_zone(unsigned order, enum pmm_zone zone)
{
	if (order >= PMM_MAX_ORDER) {
		kerr("pmm: order %u exceeds the maximum %u (%zu KiB); use "
		     "vmm_alloc() for allocations this large",
		     order, PMM_MAX_ORDER - 1,
		     (size_t)((1UL << (PMM_MAX_ORDER - 1)) * HAVEN_PAGE_SIZE / 1024));
		return 0;
	}
	if (zone >= PMM_ZONE_COUNT)
		zone = PMM_ZONE_NORMAL;

	/* While the early bound is in force, a request for NORMAL memory is
	   served from DMA32 instead, because DMA32 is by definition the
	   memory below 4 GiB that the boot direct map reaches. */
	if (early_limit && early_limit <= PMM_ZONE_DMA32_LIMIT &&
	    zone == PMM_ZONE_NORMAL)
		zone = PMM_ZONE_DMA32;

	irqflags_t f = pmm_lock();
	stats.alloc_calls++;

	paddr_t pa = 0;

	/* Try the requested zone, then zones with *lower* address limits.
	   The direction matters: a NORMAL request may be satisfied from
	   DMA32 memory, but a DMA32 request must never be satisfied from
	   NORMAL — the caller asked for an address its device can reach.
	   Spending scarce low memory on a request that did not need it is
	   wasteful, so NORMAL is tried first and falls back only when it is
	   exhausted. */
	static const enum pmm_zone order_normal[] = {
		PMM_ZONE_NORMAL, PMM_ZONE_DMA32, PMM_ZONE_DMA
	};
	static const enum pmm_zone order_dma32[] = {
		PMM_ZONE_DMA32, PMM_ZONE_DMA
	};
	static const enum pmm_zone order_dma[] = { PMM_ZONE_DMA };

	const enum pmm_zone *try_order;
	size_t n_try;

	switch (zone) {
	case PMM_ZONE_DMA:
		try_order = order_dma;    n_try = ARRAY_SIZE(order_dma);
		break;
	case PMM_ZONE_DMA32:
		try_order = order_dma32;  n_try = ARRAY_SIZE(order_dma32);
		break;
	case PMM_ZONE_NORMAL:
	default:
		try_order = order_normal; n_try = ARRAY_SIZE(order_normal);
		break;
	}

	for (size_t i = 0; i < n_try && !pa; i++)
		pa = alloc_from_zone(order, try_order[i]);

	if (!pa)
		stats.alloc_failures++;

	pmm_unlock(f);

	if (!pa)
		kerr("pmm: out of memory: cannot allocate %llu pages from "
		     "zone %s (%llu pages free overall)",
		     (unsigned long long)(1ULL << order), zone_name(zone),
		     (unsigned long long)stats.free_pages);

	return pa;
}

paddr_t pmm_alloc_pages(unsigned order)
{
	return pmm_alloc_pages_zone(order, PMM_ZONE_NORMAL);
}

paddr_t pmm_alloc_page(void)
{
	return pmm_alloc_pages_zone(0, PMM_ZONE_NORMAL);
}

paddr_t pmm_alloc_page_zeroed(void)
{
	paddr_t pa = pmm_alloc_page();
	if (pa)
		memset((void *)arch_phys_to_virt(pa), 0, HAVEN_PAGE_SIZE);
	return pa;
}

static unsigned order_for_pages(size_t nr_pages)
{
	unsigned order = 0;
	while ((1UL << order) < nr_pages && order < PMM_MAX_ORDER)
		order++;
	return order;
}

paddr_t pmm_alloc_contig(size_t nr_pages, enum pmm_zone zone)
{
	if (nr_pages == 0)
		return 0;
	return pmm_alloc_pages_zone(order_for_pages(nr_pages), zone);
}

/* ── Free ────────────────────────────────────────────────────────────── */

void pmm_free_pages(paddr_t pa, unsigned order)
{
	if (!pa)
		return;

	if (order >= PMM_MAX_ORDER) {
		kerr("pmm: free of order %u is out of range", order);
		return;
	}
	if (!IS_ALIGNED(pa, (paddr_t)HAVEN_PAGE_SIZE << order)) {
		/* An unaligned free would insert a block at a PFN whose
		   buddy arithmetic is wrong, silently corrupting the free
		   lists in a way that only shows up as a double allocation
		   much later. */
		kerr("pmm: free of 0x%llx is not aligned to its order-%u "
		     "block size; ignoring",
		     (unsigned long long)pa, order);
		return;
	}

	uint64_t pfn = pa_to_pfn(pa);
	if (pfn >= max_pfn) {
		kerr("pmm: free of 0x%llx is beyond the end of RAM",
		     (unsigned long long)pa);
		return;
	}

	irqflags_t f = pmm_lock();

	struct page *head = &mem_map[pfn];

	if (head->flags & PAGE_FLAG_FREE) {
		pmm_unlock(f);
		kerr("pmm: double free of 0x%llx", (unsigned long long)pa);
		return;
	}
	if (head->flags & PAGE_FLAG_RESERVED) {
		pmm_unlock(f);
		kerr("pmm: attempt to free reserved page 0x%llx",
		     (unsigned long long)pa);
		return;
	}

	enum pmm_zone zone = zone_for_pa(pa);
	uint64_t      n    = 1ULL << order;

	/* Clear the whole block's flags before coalescing, so a partially
	   freed block cannot be seen as a merge partner. */
	for (uint64_t i = 0; i < n; i++) {
		mem_map[pfn + i].flags &= (uint32_t)~(PAGE_FLAG_SLAB |
		                                      PAGE_FLAG_PGTABLE);
		mem_map[pfn + i].refcount = 0;
	}

	stats.free_calls++;
	stats.free_pages      += n;
	stats.allocated_pages -= n;
	stats.zone_free[zone] += n;

	/* Coalesce upward as far as the buddies allow. */
	unsigned o = order;
	while (o + 1 < PMM_MAX_ORDER) {
		uint64_t bpfn = buddy_pfn(pfn, o);

		if (!block_is_free_buddy(bpfn, o))
			break;
		/* The buddy must be in the same zone, or merging would
		   produce a block that spans a zone boundary and could be
		   handed to a device that cannot address all of it. */
		if (zone_for_pa(pfn_to_pa(bpfn)) != zone)
			break;

		block_remove(&mem_map[bpfn], o, zone);

		/* The merged block starts at the lower of the two. */
		pfn = (pfn < bpfn) ? pfn : bpfn;
		o++;
	}

	block_insert(pfn, o, zone);
	pmm_unlock(f);
}

void pmm_free_page(paddr_t pa)
{
	pmm_free_pages(pa, 0);
}

void pmm_free_contig(paddr_t pa, size_t nr_pages)
{
	if (!pa || !nr_pages)
		return;
	pmm_free_pages(pa, order_for_pages(nr_pages));
}

/* ── Reference counting ─────────────────────────────────────────────── */

void pmm_page_get(paddr_t pa)
{
	struct page *pg = pmm_page_for(pa);
	if (!pg)
		return;

	irqflags_t f = pmm_lock();
	if (pg->refcount < UINT16_MAX)
		pg->refcount++;
	pmm_unlock(f);
}

bool pmm_page_put(paddr_t pa)
{
	struct page *pg = pmm_page_for(pa);
	if (!pg)
		return false;

	irqflags_t f = pmm_lock();
	bool last = false;
	if (pg->refcount > 0) {
		pg->refcount--;
		last = (pg->refcount == 0);
	}
	unsigned order = pg->order;
	pmm_unlock(f);

	if (last)
		pmm_free_pages(pa, order);
	return last;
}

/* ── Initialisation ──────────────────────────────────────────────────
   Bootstrap problem: mem_map must exist before any page can be allocated,
   but mem_map is itself several hundred kilobytes.  Solution: a one-shot
   bump allocator over the firmware memory map, which picks the first
   usable region large enough and above the kernel image.  Those pages are
   then marked reserved like any other, so they are described by the same
   structure that describes everything else. */

static paddr_t bootstrap_alloc(const struct boot_info *bi, size_t bytes,
                               paddr_t avoid_start, paddr_t avoid_end)
{
	size_t need = ALIGN_UP(bytes, HAVEN_PAGE_SIZE);

	for (uint32_t i = 0; i < bi->memmap_count; i++) {
		const struct boot_memmap_entry *e = &bi->memmap[i];
		if (e->type != BOOT_MEM_USABLE)
			continue;

		paddr_t start = ALIGN_UP(e->base, HAVEN_PAGE_SIZE);
		paddr_t end   = ALIGN_DOWN(e->base + e->length, HAVEN_PAGE_SIZE);

		/* Never use the first megabyte: it holds the real-mode IVT,
		   the BIOS data area, the EGA aperture and the SMP
		   trampoline target.  Firmware sometimes reports parts of it
		   as usable anyway. */
		if (start < 0x100000)
			start = 0x100000;

		/* The metadata array is written through the direct map
		   immediately, so it must land inside the window boot.S
		   mapped.  See the comment on early_limit. */
		if (end > PMM_ZONE_DMA32_LIMIT)
			end = PMM_ZONE_DMA32_LIMIT;
		if (end <= start)
			continue;

		/* Step over the kernel image. */
		if (start < avoid_end && end > avoid_start) {
			if (end - avoid_end >= need)
				start = ALIGN_UP(avoid_end, HAVEN_PAGE_SIZE);
			else
				continue;
		}

		if (end > start && end - start >= need)
			return start;
	}
	return 0;
}

static void mark_range(paddr_t start, paddr_t end, uint32_t flags)
{
	uint64_t first = pa_to_pfn(ALIGN_DOWN(start, HAVEN_PAGE_SIZE));
	uint64_t last  = pa_to_pfn(ALIGN_UP(end, HAVEN_PAGE_SIZE));

	if (last > max_pfn)
		last = max_pfn;

	for (uint64_t pfn = first; pfn < last; pfn++)
		mem_map[pfn].flags |= flags;
}

/* Hand a usable range to the buddy allocator, one maximal aligned block
   at a time.  Working downward from the largest order that fits keeps the
   free lists short and lets a 4 MiB DMA request succeed right after boot
   instead of having to coalesce its way up from single pages. */
static void free_range(paddr_t start, paddr_t end)
{
	uint64_t pfn = pa_to_pfn(ALIGN_UP(start, HAVEN_PAGE_SIZE));
	uint64_t lim = pa_to_pfn(ALIGN_DOWN(end, HAVEN_PAGE_SIZE));

	if (lim > max_pfn)
		lim = max_pfn;

	while (pfn < lim) {
		struct page *pg = &mem_map[pfn];

		/* Skip anything already claimed. */
		if (pg->flags & (PAGE_FLAG_RESERVED | PAGE_FLAG_FREE)) {
			pfn++;
			continue;
		}

		/* The largest order whose block is aligned here and fits in
		   the remaining range, and none of whose pages are reserved. */
		unsigned order = 0;
		while (order + 1 < PMM_MAX_ORDER) {
			unsigned next = order + 1;
			uint64_t n    = 1ULL << next;

			if (pfn & (n - 1))
				break;
			if (pfn + n > lim)
				break;

			bool clean = true;
			for (uint64_t i = 0; i < n; i++) {
				if (mem_map[pfn + i].flags &
				    (PAGE_FLAG_RESERVED | PAGE_FLAG_FREE)) {
					clean = false;
					break;
				}
			}
			if (!clean)
				break;

			/* A block must not straddle a zone boundary. */
			if (zone_for_pa(pfn_to_pa(pfn)) !=
			    zone_for_pa(pfn_to_pa(pfn + n - 1)))
				break;

			order = next;
		}

		enum pmm_zone zone = zone_for_pa(pfn_to_pa(pfn));
		uint64_t      n    = 1ULL << order;

		block_insert(pfn, order, zone);

		stats.free_pages      += n;
		stats.zone_free[zone] += n;

		pfn += n;
	}
}

void pmm_init(const struct boot_info *bi)
{
	memset(&stats, 0, sizeof(stats));

	for (int z = 0; z < PMM_ZONE_COUNT; z++)
		for (int o = 0; o < PMM_MAX_ORDER; o++)
			list_init(&free_lists[z][o]);

	/* The metadata array must cover every page up to the highest
	   address firmware reported, including the holes: PFN arithmetic in
	   the buddy code indexes mem_map directly, so a sparse array would
	   need a lookup on every access. */
	paddr_t highest = 0;
	for (uint32_t i = 0; i < bi->memmap_count; i++) {
		paddr_t top = bi->memmap[i].base + bi->memmap[i].length;
		/* Ignore an MMIO aperture mapped far above RAM (a PCI BAR
		   can sit at 0xFE000000 or above 4 GiB); allocating metadata
		   for the gap would cost more than the RAM it describes. */
		if (bi->memmap[i].type != BOOT_MEM_USABLE &&
		    bi->memmap[i].base > bi->ram_highest)
			continue;
		if (top > highest)
			highest = top;
	}

	max_pfn = pa_to_pfn(ALIGN_UP(highest, HAVEN_PAGE_SIZE));
	if (max_pfn == 0)
		panic("pmm: firmware reported no memory at all");

	size_t  map_bytes = max_pfn * sizeof(struct page);
	paddr_t map_pa    = bootstrap_alloc(bi, map_bytes,
	                                    bi->kernel_phys_start,
	                                    bi->kernel_phys_end);
	if (!map_pa)
		panic("pmm: no region large enough for %zu KiB of page "
		      "metadata covering %llu pages",
		      map_bytes / 1024, (unsigned long long)max_pfn);

	mem_map = (struct page *)arch_phys_to_virt(map_pa);
	memset(mem_map, 0, map_bytes);

	for (uint64_t pfn = 0; pfn < max_pfn; pfn++) {
		mem_map[pfn].zone = (uint8_t)zone_for_pa(pfn_to_pa(pfn));
		list_init(&mem_map[pfn].node);
	}

	/* Everything is reserved until proven usable.  Starting from
	   "reserved" rather than "free" means a region firmware forgot to
	   describe is withheld rather than handed out — the safe direction
	   for the error, and the opposite of HavenDOS's fixed-constant
	   reserve, which defaulted the unknown to free. */
	for (uint64_t pfn = 0; pfn < max_pfn; pfn++)
		mem_map[pfn].flags |= PAGE_FLAG_RESERVED;

	/* Release the usable regions. */
	for (uint32_t i = 0; i < bi->memmap_count; i++) {
		const struct boot_memmap_entry *e = &bi->memmap[i];
		if (e->type != BOOT_MEM_USABLE)
			continue;

		uint64_t first = pa_to_pfn(ALIGN_UP(e->base, HAVEN_PAGE_SIZE));
		uint64_t last  = pa_to_pfn(ALIGN_DOWN(e->base + e->length,
		                                      HAVEN_PAGE_SIZE));
		if (last > max_pfn)
			last = max_pfn;
		for (uint64_t pfn = first; pfn < last; pfn++)
			mem_map[pfn].flags &= (uint32_t)~PAGE_FLAG_RESERVED;
	}

	/* Re-reserve everything that must never be allocated, after the
	   usable pass, so a firmware map that overlaps a reserved region
	   with a usable one resolves in favour of reserved. */

	/* The first megabyte. */
	mark_range(0, 0x100000, PAGE_FLAG_RESERVED);

	/* The kernel image, from the linker's own symbols. */
	mark_range(bi->kernel_phys_start, bi->kernel_phys_end,
	           PAGE_FLAG_RESERVED);

	/* The page metadata we just allocated. */
	mark_range(map_pa, map_pa + map_bytes, PAGE_FLAG_RESERVED);

	/* Non-usable firmware regions: framebuffer apertures, ACPI tables,
	   loader structures, bad RAM. */
	for (uint32_t i = 0; i < bi->memmap_count; i++) {
		const struct boot_memmap_entry *e = &bi->memmap[i];
		if (e->type == BOOT_MEM_USABLE)
			continue;
		mark_range(e->base, e->base + e->length, PAGE_FLAG_RESERVED);
	}

	/* Count the reserve, then build the free lists. */
	for (uint64_t pfn = 0; pfn < max_pfn; pfn++) {
		enum pmm_zone z = (enum pmm_zone)mem_map[pfn].zone;
		stats.total_pages++;
		stats.zone_total[z]++;
		if (mem_map[pfn].flags & PAGE_FLAG_RESERVED)
			stats.reserved_pages++;
	}

	for (uint32_t i = 0; i < bi->memmap_count; i++) {
		const struct boot_memmap_entry *e = &bi->memmap[i];
		if (e->type == BOOT_MEM_USABLE)
			free_range(e->base, e->base + e->length);
	}

	kinfo("pmm: %llu MiB RAM in %llu pages, %llu MiB usable, "
	      "%llu MiB reserved",
	      (unsigned long long)(stats.total_pages * HAVEN_PAGE_SIZE / (1 << 20)),
	      (unsigned long long)stats.total_pages,
	      (unsigned long long)(stats.free_pages * HAVEN_PAGE_SIZE / (1 << 20)),
	      (unsigned long long)(stats.reserved_pages * HAVEN_PAGE_SIZE / (1 << 20)));
	kinfo("pmm: page metadata %zu KiB at 0x%llx, covering PFN 0..%llu",
	      map_bytes / 1024, (unsigned long long)map_pa,
	      (unsigned long long)max_pfn);

	for (int z = 0; z < PMM_ZONE_COUNT; z++) {
		if (!stats.zone_total[z])
			continue;
		kinfo("pmm: zone %-6s %6llu pages total, %6llu free",
		      zone_name((enum pmm_zone)z),
		      (unsigned long long)stats.zone_total[z],
		      (unsigned long long)stats.zone_free[z]);
	}

	/* Hold allocations inside the boot direct map until vmm_init() has
	   extended it; see the comment on early_limit. */
	early_limit = PMM_ZONE_DMA32_LIMIT;
	if (stats.zone_free[PMM_ZONE_DMA] + stats.zone_free[PMM_ZONE_DMA32] == 0)
		panic("pmm: no usable memory below 4 GiB; the kernel cannot "
		      "build its page tables");

	if (!pmm_check())
		panic("pmm: allocator is inconsistent immediately after init");
}

void pmm_set_alloc_limit(paddr_t limit)
{
	irqflags_t f = pmm_lock();
	early_limit = limit;
	pmm_unlock(f);

	if (limit)
		kinfo("pmm: allocations bounded to below 0x%llx until the "
		      "direct map covers all of RAM",
		      (unsigned long long)limit);
	else
		kinfo("pmm: allocation bound lifted; all of RAM is available");
}

paddr_t pmm_alloc_limit(void)
{
	return early_limit;
}

void pmm_get_stats(struct pmm_stats *out)
{
	if (!out)
		return;
	irqflags_t f = pmm_lock();
	*out = stats;
	pmm_unlock(f);
}

bool pmm_check(void)
{
	irqflags_t f = pmm_lock();
	bool     ok    = true;
	uint64_t counted = 0;

	for (int z = 0; z < PMM_ZONE_COUNT && ok; z++) {
		for (int o = 0; o < PMM_MAX_ORDER && ok; o++) {
			struct page *pg;
			uint64_t     guard = 0;

			list_for_each_entry(pg, &free_lists[z][o], node) {
				/* A corrupt list would otherwise loop here
				   forever; the bound is larger than any
				   possible page count. */
				if (++guard > (1ULL << 32)) {
					kerr("pmm_check: free list z%d o%d "
					     "does not terminate", z, o);
					ok = false;
					break;
				}

				if (pg < mem_map || pg >= mem_map + max_pfn) {
					kerr("pmm_check: free list z%d o%d "
					     "holds %p, outside mem_map",
					     z, o, (void *)pg);
					ok = false;
					break;
				}

				uint64_t pfn = (uint64_t)(pg - mem_map);

				if (!(pg->flags & PAGE_FLAG_FREE)) {
					kerr("pmm_check: pfn %llu is on a free "
					     "list but not marked free",
					     (unsigned long long)pfn);
					ok = false;
					break;
				}
				if (pg->order != o) {
					kerr("pmm_check: pfn %llu is on the "
					     "order-%d list but records order %u",
					     (unsigned long long)pfn, o,
					     pg->order);
					ok = false;
					break;
				}
				if (pfn & ((1ULL << o) - 1)) {
					kerr("pmm_check: pfn %llu is not "
					     "aligned to order %d",
					     (unsigned long long)pfn, o);
					ok = false;
					break;
				}
				if (pfn + (1ULL << o) > max_pfn) {
					kerr("pmm_check: order-%d block at pfn "
					     "%llu runs past the end of RAM",
					     o, (unsigned long long)pfn);
					ok = false;
					break;
				}

				/* No page of a free block may be reserved. */
				for (uint64_t i = 0; i < (1ULL << o); i++) {
					if (mem_map[pfn + i].flags &
					    PAGE_FLAG_RESERVED) {
						kerr("pmm_check: free block at "
						     "pfn %llu contains "
						     "reserved pfn %llu",
						     (unsigned long long)pfn,
						     (unsigned long long)(pfn + i));
						ok = false;
						break;
					}
				}
				if (!ok)
					break;

				counted += 1ULL << o;
			}
		}
	}

	if (ok && counted != stats.free_pages) {
		kerr("pmm_check: free lists hold %llu pages but the counter "
		     "says %llu",
		     (unsigned long long)counted,
		     (unsigned long long)stats.free_pages);
		ok = false;
	}

	pmm_unlock(f);
	return ok;
}

void pmm_dump(void)
{
	struct pmm_stats s;
	pmm_get_stats(&s);

	kinfo("pmm: %llu total, %llu free, %llu allocated, %llu reserved pages",
	      (unsigned long long)s.total_pages,
	      (unsigned long long)s.free_pages,
	      (unsigned long long)s.allocated_pages,
	      (unsigned long long)s.reserved_pages);
	kinfo("pmm: %llu allocations, %llu frees, %llu failures",
	      (unsigned long long)s.alloc_calls,
	      (unsigned long long)s.free_calls,
	      (unsigned long long)s.alloc_failures);

	irqflags_t f = pmm_lock();
	for (int z = 0; z < PMM_ZONE_COUNT; z++) {
		if (!stats.zone_total[z])
			continue;
		char line[192];
		size_t pos = 0;
		int n = snprintf(line, sizeof(line), "pmm: zone %-6s free:",
		                 zone_name((enum pmm_zone)z));
		pos = (n > 0) ? (size_t)n : 0;
		for (int o = 0; o < PMM_MAX_ORDER; o++) {
			size_t cnt = list_length(&free_lists[z][o]);
			if (pos < sizeof(line)) {
				n = snprintf(line + pos, sizeof(line) - pos,
				             " %zu", cnt);
				if (n > 0)
					pos += (size_t)n;
			}
		}
		pmm_unlock(f);
		kinfo("%s  (orders 0..%d)", line, PMM_MAX_ORDER - 1);
		f = pmm_lock();
	}
	pmm_unlock(f);
}
