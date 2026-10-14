/* SPDX-License-Identifier: MIT
 *
 * kmalloc.c — the kernel object allocator.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's heap.c was a first-fit free list over a *static 256 KiB
 * array*.  Every kmalloc() in the system competed for that quarter
 * megabyte, it could never grow, and its own comment recorded the
 * consequence: the large buffers had to be moved off the heap entirely
 * onto physically contiguous PMM runs.
 *
 * What HavenDOS's v0.6.8 heap rewrite got right, and HavenOS keeps:
 *
 *   - a magic value in every block header, checked before any field of
 *     that header is trusted;
 *   - double-free detection via a distinct free magic;
 *   - the payload poisoned on free, so use-after-free produces
 *     recognisable garbage rather than plausible stale data;
 *   - a public integrity scanner (heap_check(), here kmalloc_check())
 *     callable from a shell command.
 *
 * What changes:
 *
 *   - the backing store is the VMM arena, so the allocator grows on
 *     demand instead of being capped at a compile-time constant;
 *   - allocation is by size class rather than first fit, so a small
 *     allocation does not walk past large free blocks and fragmentation
 *     between classes cannot occur;
 *   - large requests bypass the caches and get whole pages, so a 1 MiB
 *     allocation is a page mapping rather than a heap search;
 *   - a trailing redzone, checked on free, catches the overrun-by-a-few-
 *     bytes case that a header magic alone cannot see.
 */
#include <haven/mm.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/printf.h>
#include <haven/string.h>
#include <haven/list.h>

/* Magic values.  Distinct for live and free so a double free is
   distinguishable from corruption, which is the distinction that makes
   the log message actionable.
 *
 * These are HavenDOS's own values, kept deliberately: 0xA110CA7E reads as
 * "allocate" and 0xFEEBDAED as "freed" in its heap.c.  Someone who has
 * debugged HavenDOS will recognise them in a HavenOS memory dump, and
 * there is no engineering reason to pick different numbers. */
#define KM_MAGIC_LIVE  0xA110CA7Eu
#define KM_MAGIC_FREE  0xFEEBDAEDu
#define KM_REDZONE     0xA5A5A5A5u

/* Size classes.  Powers of two from 16 bytes to 2 KiB; anything larger
   goes to the page path.  Powers of two waste up to half a slot in the
   worst case, which is the accepted cost of never having to search. */
static const size_t class_size[] = {
	16, 32, 64, 128, 256, 512, 1024, 2048
};
#define NR_CLASSES ARRAY_SIZE(class_size)

#define KM_LARGE_THRESHOLD 2048

struct km_header {
	uint32_t magic;
	uint32_t klass;      /* size-class index, or NR_CLASSES for large */
	size_t   size;       /* bytes the caller asked for */
	struct list_head node;
};

/* A slab: one or more pages carved into equal slots of one size class. */
#define SLAB_PAGES 4
#define SLAB_BYTES (SLAB_PAGES * HAVEN_PAGE_SIZE)

struct slab {
	struct list_head node;
	struct list_head free_slots;   /* struct km_header, magic FREE */
	uint32_t         klass;
	uint32_t         total_slots;
	uint32_t         free_count;
	vaddr_t          base;
};

struct cache {
	struct list_head slabs;
	size_t           obj_size;
	size_t           slot_size;    /* header + payload + redzone */
	uint64_t         live_objects;
};

static struct cache      caches[NR_CLASSES];
static struct list_head  large_allocs;
static struct kmalloc_stats stats;
static bool              kmalloc_ready;

static volatile uint32_t km_lock_word;

static irqflags_t km_lock(void)
{
	irqflags_t f = arch_irq_save();
	while (__atomic_exchange_n(&km_lock_word, 1, __ATOMIC_ACQUIRE))
		arch_pause();
	return f;
}

static void km_unlock(irqflags_t f)
{
	__atomic_store_n(&km_lock_word, 0, __ATOMIC_RELEASE);
	arch_irq_restore(f);
}

static inline size_t slot_size_for(size_t obj)
{
	/* Header, payload, then a 4-byte redzone, the whole slot rounded to
	   16 bytes so every payload is 16-byte aligned — which the x86_64
	   ABI requires for anything that might hold a vector type, and
	   which costs nothing here. */
	return ALIGN_UP(sizeof(struct km_header) + obj + sizeof(uint32_t), 16);
}

static inline uint32_t *redzone_of(struct km_header *h)
{
	return (uint32_t *)((uint8_t *)(h + 1) + h->size);
}

static inline void *payload_of(struct km_header *h)
{
	return (void *)(h + 1);
}

static inline struct km_header *header_of(void *p)
{
	return ((struct km_header *)p) - 1;
}

static int class_for(size_t size)
{
	for (size_t i = 0; i < NR_CLASSES; i++) {
		if (size <= class_size[i])
			return (int)i;
	}
	return -1;
}

/* ── Slab growth ─────────────────────────────────────────────────────── */

static struct slab *slab_create(uint32_t klass)
{
	struct cache *c = &caches[klass];

	vaddr_t base = vmm_alloc(SLAB_BYTES, VM_PROT_READ | VM_PROT_WRITE);
	if (!base)
		return NULL;

	/* The slab descriptor lives in the slab's own first slot-aligned
	   bytes, so growing the allocator never needs the allocator. */
	struct slab *s = (struct slab *)base;
	size_t header_bytes = ALIGN_UP(sizeof(struct slab), 16);

	list_init(&s->node);
	list_init(&s->free_slots);
	s->klass       = klass;
	s->base        = base;
	s->free_count  = 0;
	s->total_slots = 0;

	for (size_t off = header_bytes;
	     off + c->slot_size <= SLAB_BYTES;
	     off += c->slot_size) {
		struct km_header *h = (struct km_header *)(base + off);
		h->magic = KM_MAGIC_FREE;
		h->klass = klass;
		h->size  = 0;
		list_init(&h->node);
		list_add(&h->node, &s->free_slots);
		s->total_slots++;
		s->free_count++;
	}

	if (s->total_slots == 0) {
		/* A slot size larger than a whole slab would loop forever
		   handing out nothing; the class table makes this
		   impossible, but an edit to SLAB_PAGES or class_size[]
		   could, so fail loudly rather than silently. */
		vmm_free(base, SLAB_BYTES);
		kerr("kmalloc: size class %u (%zu bytes) does not fit in a "
		     "%zu KiB slab", klass, c->obj_size, SLAB_BYTES / 1024);
		return NULL;
	}

	list_add(&s->node, &c->slabs);
	stats.slab_pages += SLAB_PAGES;

	return s;
}

/* ── Allocation ──────────────────────────────────────────────────────── */

static void *alloc_large(size_t size)
{
	size_t total = ALIGN_UP(sizeof(struct km_header) + size +
	                        sizeof(uint32_t), HAVEN_PAGE_SIZE);

	vaddr_t va = vmm_alloc(total, VM_PROT_READ | VM_PROT_WRITE);
	if (!va)
		return NULL;

	struct km_header *h = (struct km_header *)va;
	h->magic = KM_MAGIC_LIVE;
	h->klass = (uint32_t)NR_CLASSES;
	h->size  = size;
	list_init(&h->node);
	list_add(&h->node, &large_allocs);

	*redzone_of(h) = KM_REDZONE;

	return payload_of(h);
}

void *kmalloc(size_t size)
{
	if (size == 0)
		return NULL;

	if (!kmalloc_ready) {
		/* Calling the object allocator before it exists is a boot
		   ordering bug, and the class of bug HavenDOS's boot sequence
		   kept producing.  Say so plainly instead of faulting. */
		kerr("kmalloc: called before kmalloc_init(); this is a boot "
		     "ordering bug");
		return NULL;
	}

	/* Reject implausible sizes before any arithmetic can overflow.  A
	   caller passing (size_t)-1 — the classic result of an unchecked
	   subtraction — would otherwise wrap in slot_size_for().  HavenDOS's
	   heap added the same guard for the same reason. */
	if (size > (SIZE_MAX / 2)) {
		irqflags_t f = km_lock();
		stats.failed_allocs++;
		km_unlock(f);
		kerr("kmalloc: refusing an implausible %zu-byte request "
		     "(likely an overflowed size calculation)", size);
		return NULL;
	}

	if (size > KM_LARGE_THRESHOLD) {
		irqflags_t f = km_lock();
		void *p = alloc_large(size);
		if (p) {
			stats.total_allocs++;
			stats.live_objects++;
			stats.live_bytes += size;
		} else {
			stats.failed_allocs++;
		}
		km_unlock(f);
		if (!p)
			kerr("kmalloc: cannot satisfy a %zu-byte allocation",
			     size);
		return p;
	}

	int klass = class_for(size);
	HAVEN_ASSERT(klass >= 0);

	irqflags_t    f = km_lock();
	struct cache *c = &caches[klass];

	/* First slab with a free slot. */
	struct slab *s = NULL, *pos;
	list_for_each_entry(pos, &c->slabs, node) {
		if (pos->free_count) {
			s = pos;
			break;
		}
	}
	if (!s)
		s = slab_create((uint32_t)klass);
	if (!s) {
		stats.failed_allocs++;
		km_unlock(f);
		kerr("kmalloc: cannot grow the %zu-byte cache for a %zu-byte "
		     "allocation", c->obj_size, size);
		return NULL;
	}

	struct km_header *h = list_first_entry(&s->free_slots,
	                                       struct km_header, node);
	list_del(&h->node);
	s->free_count--;

	h->magic = KM_MAGIC_LIVE;
	h->klass = (uint32_t)klass;
	h->size  = size;
	*redzone_of(h) = KM_REDZONE;

	c->live_objects++;
	stats.total_allocs++;
	stats.live_objects++;
	stats.live_bytes += size;

	km_unlock(f);

	return payload_of(h);
}

void *kzalloc(size_t size)
{
	void *p = kmalloc(size);
	if (p)
		memset(p, 0, size);
	return p;
}

void *kcalloc(size_t n, size_t size)
{
	/* The multiplication is the whole point of this function existing:
	   kmalloc(n * size) with attacker- or arithmetic-controlled n is a
	   classic integer-overflow-to-heap-overflow. */
	if (n != 0 && size > SIZE_MAX / n) {
		kerr("kcalloc: %zu * %zu overflows", n, size);
		return NULL;
	}
	return kzalloc(n * size);
}

size_t kmalloc_size(const void *ptr)
{
	if (!ptr)
		return 0;

	struct km_header *h = header_of((void *)ptr);
	if (h->magic != KM_MAGIC_LIVE)
		return 0;
	return h->size;
}

/* ── Free ────────────────────────────────────────────────────────────── */

static struct slab *slab_for(struct km_header *h, uint32_t klass)
{
	struct cache *c = &caches[klass];
	struct slab  *s;

	vaddr_t addr = (vaddr_t)h;
	list_for_each_entry(s, &c->slabs, node) {
		if (addr >= s->base && addr < s->base + SLAB_BYTES)
			return s;
	}
	return NULL;
}

void kfree(void *ptr)
{
	if (!ptr)
		return;

	struct km_header *h = header_of(ptr);

	irqflags_t f = km_lock();

	/* Validate the magic before trusting any other field.  This is
	   exactly HavenDOS's v0.6.8 rule and the reason its heap stopped
	   corrupting itself on a bad pointer. */
	if (h->magic == KM_MAGIC_FREE) {
		km_unlock(f);
		kerr("kfree: double free of %p", ptr);
		return;
	}
	if (h->magic != KM_MAGIC_LIVE) {
		km_unlock(f);
		kerr("kfree: %p has header magic 0x%x, expected 0x%x; this is "
		     "not an allocation this allocator made, or the header "
		     "has been overwritten",
		     ptr, h->magic, KM_MAGIC_LIVE);
		return;
	}

	/* The trailing redzone catches a small overrun, which a header
	   magic cannot see because the overrun is past the payload, not
	   before it. */
	if (*redzone_of(h) != KM_REDZONE) {
		kerr("kfree: redzone after %p (%zu bytes) is 0x%x, expected "
		     "0x%x; the allocation was overrun",
		     ptr, h->size, *redzone_of(h), KM_REDZONE);
		/* Report and continue: refusing to free would leak, and the
		   damage is already done. */
	}

	size_t   size  = h->size;
	uint32_t klass = h->klass;

	/* Poison the payload so a use-after-free reads recognisable
	   garbage.  0xDD is what HavenDOS used and there is no reason to
	   pick a different value — a pattern someone might recognise from
	   two systems is more useful than a novel one. */
	memset(ptr, 0xDD, size);

	if (klass >= NR_CLASSES) {
		/* Large allocation: return the pages. */
		list_del(&h->node);
		h->magic = KM_MAGIC_FREE;

		size_t total = ALIGN_UP(sizeof(struct km_header) + size +
		                        sizeof(uint32_t), HAVEN_PAGE_SIZE);

		stats.total_frees++;
		stats.live_objects--;
		stats.live_bytes -= size;
		km_unlock(f);

		vmm_free((vaddr_t)h, total);
		return;
	}

	struct slab *s = slab_for(h, klass);
	if (!s) {
		km_unlock(f);
		kerr("kfree: %p claims size class %u but lies outside every "
		     "slab of that class", ptr, klass);
		return;
	}

	h->magic = KM_MAGIC_FREE;
	h->size  = 0;
	list_init(&h->node);
	list_add(&h->node, &s->free_slots);
	s->free_count++;

	caches[klass].live_objects--;
	stats.total_frees++;
	stats.live_objects--;
	stats.live_bytes -= size;

	/* Release a slab that has become entirely free, but only when the
	   cache has another one: freeing the last slab of a class that is
	   in active use means the next allocation immediately rebuilds it,
	   which turns an alloc/free pair at a class boundary into a pair of
	   page mappings. */
	if (s->free_count == s->total_slots &&
	    list_length(&caches[klass].slabs) > 1) {
		list_del(&s->node);
		vaddr_t base = s->base;
		stats.slab_pages -= SLAB_PAGES;
		km_unlock(f);
		vmm_free(base, SLAB_BYTES);
		return;
	}

	km_unlock(f);
}

void *krealloc(void *ptr, size_t size)
{
	if (!ptr)
		return kmalloc(size);
	if (size == 0) {
		kfree(ptr);
		return NULL;
	}

	/* Validate before reading the old size.  HavenDOS's krealloc()
	   originally did not, and its own comment records the result: a
	   stray pointer was dereferenced at p-12 before any validation
	   could reject it. */
	struct km_header *h = header_of(ptr);
	if (h->magic != KM_MAGIC_LIVE) {
		kerr("krealloc: %p is not a live allocation (magic 0x%x)",
		     ptr, h->magic);
		return NULL;
	}

	size_t old = h->size;

	/* Growing or shrinking within the same size class needs no copy. */
	if (h->klass < NR_CLASSES && size <= class_size[h->klass]) {
		irqflags_t f = km_lock();
		stats.live_bytes = stats.live_bytes - old + size;
		h->size = size;
		*redzone_of(h) = KM_REDZONE;
		km_unlock(f);
		return ptr;
	}

	void *fresh = kmalloc(size);
	if (!fresh) {
		/* On failure the original must be left intact and returned
		   as NULL, so the caller still owns a valid pointer.  Freeing
		   it here would turn an allocation failure into a
		   use-after-free. */
		return NULL;
	}

	memcpy(fresh, ptr, old < size ? old : size);
	kfree(ptr);
	return fresh;
}

/* ── Integrity scan ──────────────────────────────────────────────────── */

bool kmalloc_check(void)
{
	irqflags_t f  = km_lock();
	bool       ok = true;

	for (size_t k = 0; k < NR_CLASSES && ok; k++) {
		struct slab *s;
		size_t       slab_guard = 0;

		list_for_each_entry(s, &caches[k].slabs, node) {
			if (++slab_guard > 1000000) {
				kerr("kmalloc_check: slab list for class %zu "
				     "does not terminate", k);
				ok = false;
				break;
			}
			if (s->klass != k) {
				kerr("kmalloc_check: slab at 0x%llx is on the "
				     "class-%zu list but records class %u",
				     (unsigned long long)s->base, k, s->klass);
				ok = false;
				break;
			}
			if (s->free_count > s->total_slots) {
				kerr("kmalloc_check: slab at 0x%llx reports %u "
				     "free of %u slots",
				     (unsigned long long)s->base,
				     s->free_count, s->total_slots);
				ok = false;
				break;
			}

			/* Every slot on the free list must carry the free
			   magic and lie inside this slab. */
			struct km_header *h;
			uint32_t counted = 0;
			list_for_each_entry(h, &s->free_slots, node) {
				if (++counted > s->total_slots) {
					kerr("kmalloc_check: free list of the "
					     "slab at 0x%llx is longer than "
					     "the slab",
					     (unsigned long long)s->base);
					ok = false;
					break;
				}
				vaddr_t a = (vaddr_t)h;
				if (a < s->base || a >= s->base + SLAB_BYTES) {
					kerr("kmalloc_check: free slot %p is "
					     "outside its slab", (void *)h);
					ok = false;
					break;
				}
				if (h->magic != KM_MAGIC_FREE) {
					kerr("kmalloc_check: free slot %p has "
					     "magic 0x%x", (void *)h, h->magic);
					ok = false;
					break;
				}
			}
			if (!ok)
				break;
			if (counted != s->free_count) {
				kerr("kmalloc_check: slab at 0x%llx lists %u "
				     "free slots but counts %u",
				     (unsigned long long)s->base,
				     counted, s->free_count);
				ok = false;
				break;
			}
		}
	}

	/* Large allocations: magic and redzone. */
	if (ok) {
		struct km_header *h;
		size_t guard = 0;
		list_for_each_entry(h, &large_allocs, node) {
			if (++guard > 1000000) {
				kerr("kmalloc_check: large-allocation list "
				     "does not terminate");
				ok = false;
				break;
			}
			if (h->magic != KM_MAGIC_LIVE) {
				kerr("kmalloc_check: large allocation %p has "
				     "magic 0x%x", (void *)h, h->magic);
				ok = false;
				break;
			}
			if (*redzone_of(h) != KM_REDZONE) {
				kerr("kmalloc_check: large allocation %p "
				     "(%zu bytes) was overrun",
				     payload_of(h), h->size);
				ok = false;
				break;
			}
		}
	}

	km_unlock(f);
	return ok;
}

void kmalloc_get_stats(struct kmalloc_stats *out)
{
	if (!out)
		return;
	irqflags_t f = km_lock();
	*out = stats;
	km_unlock(f);
}

void kmalloc_dump(void)
{
	struct kmalloc_stats s;
	kmalloc_get_stats(&s);

	kinfo("kmalloc: %llu live objects, %llu live bytes, %llu slab pages",
	      (unsigned long long)s.live_objects,
	      (unsigned long long)s.live_bytes,
	      (unsigned long long)s.slab_pages);
	kinfo("kmalloc: %llu allocations, %llu frees, %llu failures",
	      (unsigned long long)s.total_allocs,
	      (unsigned long long)s.total_frees,
	      (unsigned long long)s.failed_allocs);

	for (size_t k = 0; k < NR_CLASSES; k++) {
		irqflags_t f = km_lock();
		size_t   nslabs = list_length(&caches[k].slabs);
		uint64_t live   = caches[k].live_objects;
		km_unlock(f);

		if (!nslabs && !live)
			continue;
		kinfo("kmalloc: class %4zu bytes: %zu slabs, %llu live",
		      class_size[k], nslabs, (unsigned long long)live);
	}
}

void kmalloc_init(void)
{
	list_init(&large_allocs);

	for (size_t i = 0; i < NR_CLASSES; i++) {
		list_init(&caches[i].slabs);
		caches[i].obj_size  = class_size[i];
		caches[i].slot_size = slot_size_for(class_size[i]);
		caches[i].live_objects = 0;
	}

	memset(&stats, 0, sizeof(stats));
	kmalloc_ready = true;

	kinfo("kmalloc: %zu size classes (%zu..%zu bytes), %zu KiB slabs, "
	      "larger requests go to whole pages",
	      NR_CLASSES, class_size[0], class_size[NR_CLASSES - 1],
	      SLAB_BYTES / 1024);
}
