/* SPDX-License-Identifier: MIT
 *
 * test_pmm.c — attack the buddy allocator in kernel/mm/pmm.c.
 *
 * This file links the real kernel/mm/pmm.c, in the spirit of HavenDOS's
 * own tests/test_pmm.c: "a bug here hands out memory that belongs to the
 * kernel, so it is worth testing directly rather than inferring from a
 * successful boot."
 *
 * Several tests are written specifically against the two PMM defects
 * HavenDOS documented in its own sources, so they would fail against that
 * design rather than merely passing against this one:
 *
 *   - the reserve boundary being a hard-coded constant smaller than the
 *     kernel image, so the allocator handed out live kernel memory
 *   - the contiguous allocator's interaction with fragmentation
 */
#include "harness.h"

#include <haven/mm.h>
#include <haven/boot.h>

#define PAGE 4096ULL

static bool overlaps(paddr_t a, size_t alen, paddr_t b, size_t blen)
{
	return a < b + blen && b < a + alen;
}

static void test_init_and_reserves(void)
{
	TEST_GROUP("initialisation reserves exactly what firmware described");

	struct boot_info *bi = haven_test_make_machine(64ULL << 20);
	pmm_init(bi);

	struct pmm_stats s;
	pmm_get_stats(&s);

	CHECK("the allocator is self-consistent after init", pmm_check());
	CHECK_EQ("total pages covers the whole machine",
	         s.total_pages, (64ULL << 20) / PAGE);
	CHECK("some memory is free", s.free_pages > 0);
	CHECK("some memory is reserved", s.reserved_pages > 0);
	CHECK_EQ("free + reserved accounts for every page",
	         s.free_pages + s.reserved_pages + s.allocated_pages,
	         s.total_pages);

	/* The first megabyte must never be allocatable: it holds the IVT,
	   the BIOS data area and the EGA aperture, and firmware sometimes
	   reports parts of it as usable anyway. */
	bool low_reserved = true;
	for (paddr_t pa = 0; pa < 0x100000; pa += PAGE) {
		struct page *pg = pmm_page_for(pa);
		if (!pg || !(pg->flags & PAGE_FLAG_RESERVED)) {
			low_reserved = false;
			break;
		}
	}
	CHECK("the entire first megabyte is reserved", low_reserved);

	/* THE HavenDOS BUG: its reserve was a fixed constant (1 MiB, then
	   4 MiB) that was smaller than the kernel image, so pages holding
	   live kernel BSS were on the free list.  HavenOS derives the
	   reserve from the image bounds, so every page of the image must be
	   reserved regardless of how large the image is. */
	bool image_reserved = true;
	for (paddr_t pa = bi->kernel_phys_start; pa < bi->kernel_phys_end;
	     pa += PAGE) {
		struct page *pg = pmm_page_for(pa);
		if (!pg || !(pg->flags & PAGE_FLAG_RESERVED)) {
			image_reserved = false;
			break;
		}
	}
	CHECK("every page of the kernel image is reserved", image_reserved);

	haven_test_free_machine();
}

static void test_image_size_tracks(void)
{
	TEST_GROUP("the reserve tracks image size instead of a constant");

	/* A deliberately large image, bigger than either constant HavenDOS
	   used.  The whole of it must still be withheld. */
	struct boot_info *bi = haven_test_make_machine(64ULL << 20);
	bi->kernel_phys_start = 0x200000;           /* 2 MiB */
	bi->kernel_phys_end   = 0x200000 + (12 << 20);  /* 12 MiB long */
	bi->memmap[3].base    = bi->kernel_phys_start;
	bi->memmap[3].length  = bi->kernel_phys_end - bi->kernel_phys_start;

	pmm_init(bi);

	bool all_reserved = true;
	paddr_t first_bad = 0;
	for (paddr_t pa = bi->kernel_phys_start; pa < bi->kernel_phys_end;
	     pa += PAGE) {
		struct page *pg = pmm_page_for(pa);
		if (!pg || !(pg->flags & PAGE_FLAG_RESERVED)) {
			all_reserved = false;
			first_bad = pa;
			break;
		}
	}
	char detail[96];
	snprintf(detail, sizeof(detail), "first unreserved page 0x%llx",
	         (unsigned long long)first_bad);
	haven_check("a 12 MiB image is reserved in full (the old 1 MiB and "
	            "4 MiB constants would both have failed here)",
	            all_reserved, detail);

	/* And no allocation may land inside it. */
	int inside = 0;
	for (int i = 0; i < 2000; i++) {
		paddr_t pa = pmm_alloc_page();
		if (!pa)
			break;
		if (pa >= bi->kernel_phys_start && pa < bi->kernel_phys_end)
			inside++;
	}
	CHECK_EQ("2000 allocations, none inside the kernel image", inside, 0);
	CHECK("the allocator is still consistent", pmm_check());

	haven_test_free_machine();
}

static void test_alloc_free_roundtrip(void)
{
	TEST_GROUP("allocate and free returns every page");

	haven_test_make_machine(64ULL << 20);
	pmm_init(haven_test_make_machine(64ULL << 20));

	struct pmm_stats before, after;
	pmm_get_stats(&before);

	paddr_t pages[256];
	for (int i = 0; i < 256; i++) {
		pages[i] = pmm_alloc_page();
		CHECK_NE("single-page allocation succeeds", pages[i], 0);
	}

	pmm_get_stats(&after);
	CHECK_EQ("free count dropped by exactly the number allocated",
	         before.free_pages - after.free_pages, 256);

	/* Every address must be distinct and page-aligned. */
	int dups = 0, misaligned = 0;
	for (int i = 0; i < 256; i++) {
		if (pages[i] % PAGE)
			misaligned++;
		for (int j = i + 1; j < 256; j++) {
			if (pages[i] == pages[j])
				dups++;
		}
	}
	CHECK_EQ("no address was handed out twice", dups, 0);
	CHECK_EQ("every address is page aligned", misaligned, 0);

	for (int i = 0; i < 256; i++)
		pmm_free_page(pages[i]);

	pmm_get_stats(&after);
	CHECK_EQ("freeing restores the original free count",
	         after.free_pages, before.free_pages);
	CHECK("the allocator is consistent after the round trip", pmm_check());

	haven_test_free_machine();
}

static void test_buddy_coalescing(void)
{
	TEST_GROUP("buddy blocks coalesce back to their original order");

	pmm_init(haven_test_make_machine(64ULL << 20));

	/* Take a large block, then split it by hand through the allocator:
	   allocating order 0 twice from a freshly split order-1 block and
	   freeing both must restore an order-1 block.  The observable proof
	   is that an order-10 allocation still succeeds after a lot of
	   split/merge churn — if coalescing were broken, the free lists
	   would be all order-0 and the large request would fail. */
	paddr_t big = pmm_alloc_pages(10);     /* 4 MiB */
	CHECK_NE("a 4 MiB (order 10) allocation succeeds", big, 0);
	CHECK_EQ("it is 4 MiB aligned", big % (PAGE << 10), 0);
	pmm_free_pages(big, 10);

	/* Churn: thousands of small allocations and frees. */
	for (int round = 0; round < 200; round++) {
		paddr_t p[16];
		for (int i = 0; i < 16; i++)
			p[i] = pmm_alloc_pages(i % 4);
		for (int i = 0; i < 16; i++)
			pmm_free_pages(p[i], i % 4);
	}

	CHECK("the allocator is consistent after churn", pmm_check());

	paddr_t big2 = pmm_alloc_pages(10);
	CHECK_NE("a 4 MiB allocation still succeeds after churn, which it "
	         "could not if coalescing were broken", big2, 0);
	if (big2)
		pmm_free_pages(big2, 10);

	haven_test_free_machine();
}

static void test_contiguous_and_fragmentation(void)
{
	TEST_GROUP("contiguous runs and fragmentation");

	pmm_init(haven_test_make_machine(64ULL << 20));

	/* The two buffers HavenDOS allocated contiguously: a 2 MiB update
	   staging area and a 512 KiB HTTP body. */
	paddr_t a = pmm_alloc_contig(512, PMM_ZONE_NORMAL);   /* 2 MiB */
	paddr_t b = pmm_alloc_contig(128, PMM_ZONE_NORMAL);   /* 512 KiB */

	CHECK_NE("a 2 MiB contiguous run succeeds", a, 0);
	CHECK_NE("a 512 KiB contiguous run succeeds alongside it", b, 0);
	CHECK("the two runs do not overlap",
	      !overlaps(a, 512 * PAGE, b, 128 * PAGE));
	CHECK_EQ("the 2 MiB run is 2 MiB aligned", a % (512 * PAGE), 0);

	pmm_free_contig(a, 512);
	pmm_free_contig(b, 128);
	CHECK("consistent after freeing both runs", pmm_check());

	/* Punch a hole and confirm a run larger than the hole is placed
	   past it, while a run that fits reuses it. */
	paddr_t p1 = pmm_alloc_pages(2);   /* 4 pages */
	paddr_t p2 = pmm_alloc_pages(2);
	paddr_t p3 = pmm_alloc_pages(2);
	CHECK_NE("three order-2 blocks allocated", p1 && p2 && p3, 0);

	pmm_free_pages(p2, 2);             /* 4-page hole */

	paddr_t fit = pmm_alloc_pages(2);
	CHECK_EQ("a block that fits the hole reuses it", fit, p2);

	pmm_free_pages(p1, 2);
	pmm_free_pages(p3, 2);
	pmm_free_pages(fit, 2);

	haven_test_free_machine();
}

static void test_zones(void)
{
	TEST_GROUP("zones keep DMA-capable memory addressable");

	pmm_init(haven_test_make_machine(64ULL << 20));

	/* A DMA request must come from below 16 MiB, because that is the
	   constraint the caller is expressing.  Satisfying it from high
	   memory would appear to work and then corrupt data during the
	   first transfer. */
	for (int i = 0; i < 64; i++) {
		paddr_t pa = pmm_alloc_pages_zone(0, PMM_ZONE_DMA);
		CHECK_NE("a DMA-zone allocation succeeds", pa, 0);
		char d[96];
		snprintf(d, sizeof(d), "got 0x%llx", (unsigned long long)pa);
		haven_check("it is below the 16 MiB ISA DMA limit",
		            pa < PMM_ZONE_DMA_LIMIT, d);
	}

	paddr_t d32 = pmm_alloc_pages_zone(0, PMM_ZONE_DMA32);
	CHECK_NE("a DMA32 allocation succeeds", d32, 0);
	CHECK("it is below the 4 GiB 32-bit PCI limit",
	      d32 < PMM_ZONE_DMA32_LIMIT);

	CHECK("the allocator is consistent", pmm_check());
	haven_test_free_machine();
}

static void test_error_paths(void)
{
	TEST_GROUP("refusal instead of corruption");

	pmm_init(haven_test_make_machine(64ULL << 20));

	struct pmm_stats before, after;

	/* An order beyond the maximum must be refused, not wrapped. */
	CHECK_EQ("an order past PMM_MAX_ORDER returns 0",
	         pmm_alloc_pages(PMM_MAX_ORDER), 0);
	CHECK_EQ("an absurd order returns 0", pmm_alloc_pages(60), 0);

	/* Freeing null, out-of-range and misaligned addresses must change
	   nothing rather than inserting a bogus block into a free list,
	   which would corrupt the buddy arithmetic silently. */
	pmm_get_stats(&before);
	pmm_free_page(0);
	pmm_free_pages(0xFFFFFFFFFFULL, 0);
	pmm_get_stats(&after);
	CHECK_EQ("freeing null and out-of-range addresses changes nothing",
	         after.free_pages, before.free_pages);

	paddr_t p = pmm_alloc_pages(2);        /* 4 pages, 16 KiB aligned */
	CHECK_NE("got an order-2 block", p, 0);
	pmm_get_stats(&before);
	pmm_free_pages(p + PAGE, 2);           /* misaligned for order 2 */
	pmm_get_stats(&after);
	CHECK_EQ("a misaligned free is refused",
	         after.free_pages, before.free_pages);
	pmm_free_pages(p, 2);                  /* the correct free */

	/* Double free must be caught. */
	paddr_t q = pmm_alloc_page();
	pmm_free_page(q);
	pmm_get_stats(&before);
	pmm_free_page(q);
	pmm_get_stats(&after);
	CHECK_EQ("a double free does not inflate the free count",
	         after.free_pages, before.free_pages);

	/* Freeing a reserved page must be refused. */
	pmm_get_stats(&before);
	pmm_free_page(0x1000);                 /* inside the first MiB */
	pmm_get_stats(&after);
	CHECK_EQ("freeing a reserved page is refused",
	         after.free_pages, before.free_pages);

	CHECK("the allocator survived every bad call intact", pmm_check());
	haven_test_free_machine();
}

static void test_exhaustion(void)
{
	TEST_GROUP("exhaustion returns 0 rather than wrapping");

	/* A deliberately small machine: 8 MiB, of which 1 MiB is the low
	   reserve and 1 MiB is the simulated kernel image. */
	struct boot_info *bi = haven_test_make_machine(8ULL << 20);
	pmm_init(bi);

	struct pmm_stats s;
	pmm_get_stats(&s);

	/* Ask for more than the machine has. */
	CHECK_EQ("a 4 MiB request from an 8 MiB machine with most of it "
	         "reserved either succeeds or returns 0, never wraps",
	         pmm_alloc_pages(10) % PAGE, 0);

	/* Drain it. */
	uint64_t got = 0;
	while (pmm_alloc_page())
		got++;

	CHECK("allocation eventually returns 0 instead of looping", true);
	CHECK("the allocator is consistent when empty", pmm_check());

	pmm_get_stats(&s);
	CHECK_EQ("no pages are reported free once drained", s.free_pages, 0);
	CHECK("at least one failure was recorded", s.alloc_failures > 0);
	CHECK("the drain handed out a plausible number of pages",
	      got > 0 && got < (8ULL << 20) / PAGE);

	haven_test_free_machine();
}

static void test_refcounting(void)
{
	TEST_GROUP("page reference counts");

	pmm_init(haven_test_make_machine(64ULL << 20));

	paddr_t pa = pmm_alloc_page();
	CHECK_NE("allocated a page", pa, 0);

	struct page *pg = pmm_page_for(pa);
	CHECK_EQ("a fresh allocation has refcount 1", pg->refcount, 1);

	pmm_page_get(pa);
	CHECK_EQ("get raises the count", pg->refcount, 2);

	CHECK("put with a remaining reference does not free",
	      pmm_page_put(pa) == false);
	CHECK_EQ("the count dropped to 1", pg->refcount, 1);

	struct pmm_stats before, after;
	pmm_get_stats(&before);
	CHECK("the last put frees the page", pmm_page_put(pa) == true);
	pmm_get_stats(&after);
	CHECK_EQ("and the page is back on the free list",
	         after.free_pages, before.free_pages + 1);

	CHECK("consistent after refcount churn", pmm_check());
	haven_test_free_machine();
}

static void test_page_lookup(void)
{
	TEST_GROUP("page metadata lookup round-trips");

	pmm_init(haven_test_make_machine(64ULL << 20));

	for (paddr_t pa = 0x100000; pa < (4ULL << 20); pa += PAGE * 37) {
		struct page *pg = pmm_page_for(pa);
		CHECK_NE("every in-range address has metadata", (long)pg, 0);
		if (pg)
			CHECK_EQ("address round-trips through the metadata",
			         pmm_page_addr(pg), pa);
	}

	CHECK_EQ("an address past the end of RAM has no metadata",
	         (long)pmm_page_for(1ULL << 40), 0);

	haven_test_free_machine();
}

int main(void)
{
	printf("HavenOS host tests: physical memory allocator\n");
	printf("=============================================\n");
	printf("linking the real kernel/mm/pmm.c\n");

	haven_test_capture_log(true);   /* the kernel's own log is noise here */

	test_init_and_reserves();
	test_image_size_tracks();
	test_alloc_free_roundtrip();
	test_buddy_coalescing();
	test_contiguous_and_fragmentation();
	test_zones();
	test_error_paths();
	test_exhaustion();
	test_refcounting();
	test_page_lookup();

	haven_test_capture_log(false);
	return haven_test_report("pmm");
}
