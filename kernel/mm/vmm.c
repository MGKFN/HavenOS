/* SPDX-License-Identifier: MIT
 *
 * vmm.c — kernel virtual address space management.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had no virtual memory, so a "large allocation" meant a
 * physically contiguous run, and the only way to get one was to scan the
 * physical bitmap.  Its 2 MiB update staging buffer and 512 KiB HTTP body
 * were allocated exactly that way, and its own source notes that moving
 * them onto the PMM is what first made pmm_alloc() live — and therefore
 * what first exposed the reserve-boundary bug.
 *
 * With virtual memory the requirement disappears: vmm_alloc() reserves
 * address space from a 16 TiB arena and backs it with whatever physical
 * pages happen to be free, in any order.  A 4 MiB allocation needs 4 MiB
 * of free RAM, not 4 MiB of adjacent RAM.
 *
 * ARENA ALLOCATION STRATEGY
 * The arena is managed by a sorted free list of extents, which is simple
 * enough to audit and good enough at this scale: kernel virtual
 * allocations are measured in hundreds, not millions.  Coalescing happens
 * on free, so the list does not grow without bound under alloc/free
 * churn.  Guard pages are the reason the extents matter at all — every
 * allocation is followed by one unmapped page, so a buffer overrun off
 * the end of a kmalloc arena block faults instead of reaching the next
 * allocation.
 */
#include <haven/mm.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/string.h>
#include <haven/errno.h>
#include <haven/list.h>

/* The layout constants are architecture-specific, but the VMM needs them.
   They are declared through the arch header rather than hard-coded here,
   so an ARM64 port supplies its own without editing this file. */
#include <haven/vmlayout.h>

struct extent {
	struct list_head node;
	vaddr_t          base;
	size_t           len;
};

/* Bootstrap extents: the arena allocator cannot use kmalloc, because
   kmalloc is built on top of it.  A static pool of extent records breaks
   the cycle.  256 is generously more than the number of live kernel
   virtual regions HavenOS has — and running out is reported loudly rather
   than silently merging unrelated regions. */
#define MAX_EXTENTS 256

static struct extent     extent_pool[MAX_EXTENTS];
static struct list_head  extent_free;    /* unused records */
static struct list_head  arena_free;     /* free address space, sorted */
static struct list_head  arena_used;     /* live allocations */

static vaddr_t mmio_next;
static size_t  mmio_used;

static struct vmm_stats stats;

static volatile uint32_t vmm_lock_word;

static irqflags_t vmm_lock(void)
{
	irqflags_t f = arch_irq_save();
	while (__atomic_exchange_n(&vmm_lock_word, 1, __ATOMIC_ACQUIRE))
		arch_pause();
	return f;
}

static void vmm_unlock(irqflags_t f)
{
	__atomic_store_n(&vmm_lock_word, 0, __ATOMIC_RELEASE);
	arch_irq_restore(f);
}

static struct extent *extent_get(void)
{
	if (list_empty(&extent_free)) {
		kerr("vmm: extent pool exhausted (%d records); the kernel "
		     "virtual arena cannot track another region",
		     MAX_EXTENTS);
		return NULL;
	}
	struct extent *e = list_first_entry(&extent_free, struct extent, node);
	list_del(&e->node);
	return e;
}

static void extent_put(struct extent *e)
{
	e->base = 0;
	e->len  = 0;
	list_add(&e->node, &extent_free);
}

/* Insert `e` into the sorted free list, merging with either neighbour it
   touches.  Coalescing on insert is what keeps the list from fragmenting
   into one record per historical allocation. */
static void arena_insert_free(struct extent *e)
{
	struct extent *pos;
	struct list_head *before = &arena_free;

	list_for_each_entry(pos, &arena_free, node) {
		if (pos->base > e->base)
			break;
		before = &pos->node;
	}
	__list_add(&e->node, before, before->next);

	/* Merge forward. */
	if (e->node.next != &arena_free) {
		struct extent *next = list_entry(e->node.next, struct extent,
		                                 node);
		if (e->base + e->len == next->base) {
			e->len += next->len;
			list_del(&next->node);
			extent_put(next);
		}
	}

	/* Merge backward. */
	if (e->node.prev != &arena_free) {
		struct extent *prev = list_entry(e->node.prev, struct extent,
		                                 node);
		if (prev->base + prev->len == e->base) {
			prev->len += e->len;
			list_del(&e->node);
			extent_put(e);
		}
	}
}

/* Carve `len` bytes out of the free list, first fit. */
static vaddr_t arena_take(size_t len)
{
	struct extent *pos;

	list_for_each_entry(pos, &arena_free, node) {
		if (pos->len < len)
			continue;

		vaddr_t base = pos->base;

		if (pos->len == len) {
			list_del(&pos->node);
			extent_put(pos);
		} else {
			pos->base += len;
			pos->len  -= len;
		}

		struct extent *used = extent_get();
		if (!used) {
			/* Put the address space back rather than leaking it. */
			struct extent *back = extent_get();
			if (back) {
				back->base = base;
				back->len  = len;
				arena_insert_free(back);
			}
			return 0;
		}
		used->base = base;
		used->len  = len;
		list_add(&used->node, &arena_used);

		return base;
	}
	return 0;
}

static struct extent *arena_find_used(vaddr_t base)
{
	struct extent *pos;
	list_for_each_entry(pos, &arena_used, node) {
		if (pos->base == base)
			return pos;
	}
	return NULL;
}

/* ── Public mapping interface ────────────────────────────────────────── */

int vmm_map(vaddr_t va, paddr_t pa, size_t len, uint32_t prot)
{
	int rc = arch_pt_map(arch_pt_kernel(), va, pa, len, prot);
	if (rc == 0)
		stats.mapped_pages += DIV_ROUND_UP(len, HAVEN_PAGE_SIZE);
	return rc;
}

int vmm_unmap(vaddr_t va, size_t len)
{
	int rc = arch_pt_unmap(arch_pt_kernel(), va, len);
	if (rc == 0) {
		uint64_t n = DIV_ROUND_UP(len, HAVEN_PAGE_SIZE);
		stats.mapped_pages = stats.mapped_pages > n
		                   ? stats.mapped_pages - n : 0;
	}
	return rc;
}

int vmm_protect(vaddr_t va, size_t len, uint32_t prot)
{
	return arch_pt_protect(arch_pt_kernel(), va, len, prot);
}

/* ── Arena allocation ────────────────────────────────────────────────── */

vaddr_t vmm_alloc(size_t len, uint32_t prot)
{
	if (len == 0)
		return 0;

	len = ALIGN_UP(len, HAVEN_PAGE_SIZE);

	/* One guard page per allocation.  It costs address space, of which
	   there are 16 TiB, and buys a fault instead of silent corruption
	   when something runs off the end. */
	size_t with_guard = len + HAVEN_PAGE_SIZE;

	irqflags_t f  = vmm_lock();
	vaddr_t    va = arena_take(with_guard);
	if (va)
		stats.arena_used_bytes += with_guard;
	vmm_unlock(f);

	if (!va) {
		kerr("vmm: no %zu KiB region available in the kernel arena",
		     len / 1024);
		return 0;
	}

	/* Back the range page by page.  Physical contiguity is not required
	   and not attempted: that is the entire advantage over HavenDOS's
	   contiguous-run approach. */
	size_t done = 0;
	while (done < len) {
		paddr_t pa = pmm_alloc_page();
		if (!pa)
			goto unwind;

		int rc = vmm_map(va + done, pa, HAVEN_PAGE_SIZE, prot);
		if (rc != 0) {
			pmm_free_page(pa);
			goto unwind;
		}
		done += HAVEN_PAGE_SIZE;
	}

	return va;

unwind:
	/* Free exactly the pages this call mapped, then return the address
	   space.  A partial failure that leaked either would be invisible
	   until the system ran out of one of them much later. */
	for (size_t off = 0; off < done; off += HAVEN_PAGE_SIZE) {
		paddr_t pa = arch_pt_translate(arch_pt_kernel(), va + off);
		vmm_unmap(va + off, HAVEN_PAGE_SIZE);
		if (pa)
			pmm_free_page(pa);
	}

	f = vmm_lock();
	struct extent *used = arena_find_used(va);
	if (used) {
		list_del(&used->node);
		struct extent *back = extent_get();
		if (back) {
			back->base = used->base;
			back->len  = used->len;
			arena_insert_free(back);
		}
		stats.arena_used_bytes -= used->len;
		extent_put(used);
	}
	vmm_unlock(f);

	kerr("vmm: out of physical memory while backing a %zu KiB "
	     "allocation", len / 1024);
	return 0;
}

void vmm_free(vaddr_t va, size_t len)
{
	if (!va)
		return;

	irqflags_t f = vmm_lock();
	struct extent *used = arena_find_used(va);
	if (!used) {
		vmm_unlock(f);
		kerr("vmm: free of 0x%llx, which is not a live arena "
		     "allocation", (unsigned long long)va);
		return;
	}

	size_t total = used->len;             /* includes the guard page */
	list_del(&used->node);
	struct extent *back = extent_get();
	if (back) {
		back->base = used->base;
		back->len  = total;
		arena_insert_free(back);
	}
	stats.arena_used_bytes -= total;
	extent_put(used);
	vmm_unlock(f);

	/* The guard page was never mapped, so unmap only the data range.
	   Using `len` from the caller rather than the recorded extent would
	   leak pages if the caller passed a smaller length than it
	   allocated, so the recorded size wins. */
	size_t data_len = total - HAVEN_PAGE_SIZE;
	if (len && ALIGN_UP(len, HAVEN_PAGE_SIZE) != data_len)
		kwarn("vmm: free of 0x%llx given length %zu but it was "
		      "allocated with length %zu; using the recorded size",
		      (unsigned long long)va, len, data_len);

	for (size_t off = 0; off < data_len; off += HAVEN_PAGE_SIZE) {
		paddr_t pa = arch_pt_translate(arch_pt_kernel(), va + off);
		vmm_unmap(va + off, HAVEN_PAGE_SIZE);
		if (pa)
			pmm_free_page(pa);
	}
}

/* ── MMIO ────────────────────────────────────────────────────────────── */

vaddr_t vmm_map_mmio(paddr_t pa, size_t len)
{
	if (len == 0)
		return 0;

	/* Device registers are not RAM: the mapping must be uncached, or
	   the CPU will cache a status register and the driver will poll a
	   stale value forever.  It must also be non-executable — there is
	   never a reason to execute from a device aperture. */
	paddr_t page_base = ALIGN_DOWN(pa, HAVEN_PAGE_SIZE);
	size_t  offset    = pa - page_base;
	size_t  map_len   = ALIGN_UP(offset + len, HAVEN_PAGE_SIZE);

	irqflags_t f = vmm_lock();
	if (mmio_next + map_len > MMIO_BASE + MMIO_SIZE) {
		vmm_unlock(f);
		kerr("vmm: MMIO window exhausted");
		return 0;
	}
	vaddr_t va = mmio_next;
	mmio_next += map_len + HAVEN_PAGE_SIZE;   /* guard page */
	mmio_used += map_len;
	stats.mmio_used_bytes = mmio_used;
	vmm_unlock(f);

	int rc = vmm_map(va, page_base, map_len,
	                 VM_PROT_READ | VM_PROT_WRITE |
	                 VM_ATTR_NOCACHE | VM_ATTR_GLOBAL);
	if (rc != 0) {
		kerr("vmm: cannot map MMIO 0x%llx+%zu: %s",
		     (unsigned long long)pa, len, haven_strerror(rc));
		return 0;
	}

	return va + offset;
}

void vmm_unmap_mmio(vaddr_t va, size_t len)
{
	if (!va)
		return;

	vaddr_t base    = ALIGN_DOWN(va, HAVEN_PAGE_SIZE);
	size_t  offset  = va - base;
	size_t  map_len = ALIGN_UP(offset + len, HAVEN_PAGE_SIZE);

	vmm_unmap(base, map_len);

	irqflags_t f = vmm_lock();
	mmio_used = mmio_used > map_len ? mmio_used - map_len : 0;
	stats.mmio_used_bytes = mmio_used;
	vmm_unlock(f);
	/* The address space itself is not reclaimed: MMIO mappings last for
	   the life of the driver, and a bump pointer over a 16 TiB window
	   cannot plausibly run out. */
}

/* ── Kernel stacks ───────────────────────────────────────────────────── */

vaddr_t vmm_alloc_kstack(void)
{
	/* vmm_alloc() already places a guard page above the allocation.  A
	   stack grows downward, so the guard must be BELOW it — which is
	   where the allocation's own base sits relative to the previous
	   allocation's guard.  To get a guard on the low side specifically,
	   allocate one page more than needed and leave the lowest page
	   unmapped. */
	size_t  len = KSTACK_BYTES + HAVEN_PAGE_SIZE;
	vaddr_t va  = vmm_alloc(len, VM_PROT_READ | VM_PROT_WRITE);
	if (!va)
		return 0;

	/* Unmap the lowest page: an overflow past the bottom of the stack
	   now faults at a known address.  This is the protection that would
	   have turned the 64 KiB-buffer-on-a-64 KiB-stack bug caught during
	   this project's own bring-up from memory corruption into an
	   immediate, obvious fault. */
	paddr_t pa = arch_pt_translate(arch_pt_kernel(), va);
	vmm_unmap(va, HAVEN_PAGE_SIZE);
	if (pa)
		pmm_free_page(pa);

	/* The stack pointer starts at the top of the mapped range. */
	return va + len;
}

void vmm_free_kstack(vaddr_t stack_top)
{
	if (!stack_top)
		return;

	size_t  len  = KSTACK_BYTES + HAVEN_PAGE_SIZE;
	vaddr_t base = stack_top - len;

	/* Re-map the guard page so vmm_free()'s unmap loop accounts for the
	   whole extent; it tolerates an already-absent mapping, but
	   re-mapping keeps the page accounting exact. */
	paddr_t pa = pmm_alloc_page();
	if (pa)
		vmm_map(base, pa, HAVEN_PAGE_SIZE,
		        VM_PROT_READ | VM_PROT_WRITE);

	vmm_free(base, len);
}

/* ── Kernel image protection ─────────────────────────────────────────── */

extern uint8_t __boot_start[],   __boot_end[];
extern uint8_t __text_start[],   __text_end[];
extern uint8_t __rodata_start[], __rodata_end[];
extern uint8_t __data_start[],   __data_end[];
extern uint8_t __bss_start[],    __bss_end[];

void vmm_protect_kernel(void)
{
	struct {
		const char *name;
		vaddr_t     start, end;
		uint32_t    prot;
	} regions[] = {
		/* .boot holds the Multiboot2 header and the 32-bit
		   trampoline.  It is never executed again after the long-mode
		   jump, but it is executable code and belongs in the
		   read-execute band rather than being left read-write. */
		{ ".boot",   (vaddr_t)__boot_start,   (vaddr_t)__boot_end,
		  VM_PROT_READ | VM_PROT_EXEC | VM_ATTR_GLOBAL },
		{ ".text",   (vaddr_t)__text_start,   (vaddr_t)__text_end,
		  VM_PROT_READ | VM_PROT_EXEC | VM_ATTR_GLOBAL },
		{ ".rodata", (vaddr_t)__rodata_start, (vaddr_t)__rodata_end,
		  VM_PROT_READ | VM_ATTR_GLOBAL },
		{ ".data",   (vaddr_t)__data_start,   (vaddr_t)__data_end,
		  VM_PROT_READ | VM_PROT_WRITE | VM_ATTR_GLOBAL },
		{ ".bss",    (vaddr_t)__bss_start,    (vaddr_t)__bss_end,
		  VM_PROT_READ | VM_PROT_WRITE | VM_ATTR_GLOBAL },
	};

	for (size_t i = 0; i < ARRAY_SIZE(regions); i++) {
		if (regions[i].end <= regions[i].start)
			continue;

		size_t len = regions[i].end - regions[i].start;
		int rc = vmm_protect(regions[i].start, len, regions[i].prot);
		if (rc != 0) {
			kerr("vmm: cannot protect %s: %s",
			     regions[i].name, haven_strerror(rc));
			continue;
		}

		kinfo("vmm: %-8s 0x%llx..0x%llx  %s%s%s",
		      regions[i].name,
		      (unsigned long long)regions[i].start,
		      (unsigned long long)regions[i].end,
		      (regions[i].prot & VM_PROT_READ)  ? "r" : "-",
		      (regions[i].prot & VM_PROT_WRITE) ? "w" : "-",
		      (regions[i].prot & VM_PROT_EXEC)  ? "x" : "-");
	}

	/* .text is now read-only and .rodata is read-only and
	   non-executable.  A stray kernel write into either faults rather
	   than modifying code — which, with HavenDOS's flat read-write-
	   execute segment, it simply would not have. */
	kinfo("vmm: kernel image permissions applied (W^X enforced)");
}

/* ── Statistics ──────────────────────────────────────────────────────── */

void vmm_get_stats(struct vmm_stats *out)
{
	if (!out)
		return;
	irqflags_t f = vmm_lock();
	*out = stats;
	out->pagetable_pages = arch_pt_page_count();
	vmm_unlock(f);
}

/* ── Initialisation ──────────────────────────────────────────────────── */

void vmm_init(const struct boot_info *bi)
{
	list_init(&extent_free);
	list_init(&arena_free);
	list_init(&arena_used);

	for (size_t i = 0; i < MAX_EXTENTS; i++) {
		list_init(&extent_pool[i].node);
		list_add(&extent_pool[i].node, &extent_free);
	}

	/* Build the real kernel page tables.  Until arch_pt_activate_kernel()
	   runs, the boot tables from the trampoline are still live, which is
	   what makes it possible to allocate the new tables at all. */
	arch_pt_init_kernel();

	/* Direct-map every region firmware described, usable or not.  The
	   reserved ones are included because drivers legitimately need to
	   read ACPI tables and firmware structures, and the framebuffer
	   aperture has to be reachable for the console.
	   HavenDOS's 4 GiB flat segment gave it this for free and paid for
	   it with no protection at all; here it is an explicit, non-
	   executable, global mapping. */
	uint64_t mapped_ram = 0;    /* usable RAM, the number a user cares about */
	uint64_t mapped_all = 0;    /* including firmware regions and apertures */

	for (uint32_t i = 0; i < bi->memmap_count; i++) {
		const struct boot_memmap_entry *e = &bi->memmap[i];

		paddr_t start = ALIGN_DOWN(e->base, HAVEN_PAGE_SIZE);
		paddr_t end   = ALIGN_UP(e->base + e->length, HAVEN_PAGE_SIZE);
		if (end <= start)
			continue;

		int rc = arch_pt_map_hhdm(start, end - start);
		if (rc != 0) {
			kwarn("vmm: cannot direct-map 0x%llx..0x%llx (%s): %s",
			      (unsigned long long)start,
			      (unsigned long long)end,
			      boot_mem_type_name(e->type),
			      haven_strerror(rc));
			continue;
		}
		mapped_all += end - start;
		if (e->type == BOOT_MEM_USABLE)
			mapped_ram += end - start;
	}

	int rc = arch_pt_map_kernel_image(bi->kernel_phys_start,
	                                  bi->kernel_phys_end);
	if (rc != 0)
		panic("vmm: cannot map the kernel image: %s",
		      haven_strerror(rc));

	arch_pt_activate_kernel();

	/* The identity map was only ever needed for the instruction after
	   the long-mode far jump.  Dropping it means a kernel pointer bug
	   that yields a small address faults instead of silently reading
	   physical memory. */
	arch_pt_drop_identity_map();

	/* Publish the arena. */
	struct extent *heap = extent_get();
	if (!heap)
		panic("vmm: cannot create the kernel arena extent");
	heap->base = KHEAP_BASE;
	heap->len  = KHEAP_SIZE;
	arena_insert_free(heap);

	stats.arena_total_bytes = KHEAP_SIZE;
	mmio_next = MMIO_BASE;

	/* The direct map now covers every region firmware described, so the
	   PMM may hand out memory anywhere.  Until this point it was bounded
	   to the window boot.S mapped. */
	pmm_set_alloc_limit(0);

	kinfo("vmm: direct map at 0x%llx covers %llu MiB of RAM "
	      "(%llu MiB including firmware regions and apertures)",
	      (unsigned long long)HHDM_BASE,
	      (unsigned long long)(mapped_ram / (1 << 20)),
	      (unsigned long long)(mapped_all / (1 << 20)));
	kinfo("vmm: kernel arena %llu TiB at 0x%llx, MMIO window at 0x%llx",
	      (unsigned long long)(KHEAP_SIZE >> 40),
	      (unsigned long long)KHEAP_BASE,
	      (unsigned long long)MMIO_BASE);
	kinfo("vmm: identity map dropped; %llu page-table pages in use",
	      (unsigned long long)arch_pt_page_count());
}
