/* SPDX-License-Identifier: MIT
 *
 * main.c — the architecture-neutral kernel entry point.
 *
 * There is no inline assembly, no port I/O and no architecture
 * conditional in this file, by design and by enforcement
 * (scripts/check-portability.sh).  Everything it needs from the hardware
 * it asks for through include/haven/arch.h.  That is what makes the ARM64
 * port described in docs/PORTING_ARM64.md a matter of adding a directory
 * rather than editing the kernel.
 *
 * BOOT SEQUENCE, WRITTEN DOWN
 * HavenDOS's kernel_main() grew to a hundred lines of initialisation
 * calls whose order nobody could see, and its own source comments
 * catalogue the resulting bugs: net_init() running after the screen that
 * reported whether a NIC was found, so every boot said "Not detected";
 * virtio_scan_drives() running lazily, so the same screen misreported
 * working disks.  Neither was a driver bug.
 *
 * So each stage below states what it provides and what it requires, and
 * a stage may only use facilities from the stages above it.  When a stage
 * fails, it says so and the kernel either continues degraded or stops —
 * never continues while reporting success.
 */
#include <haven/boot.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/arch.h>
#include <haven/mm.h>
#include <haven/smp.h>
#include <haven/string.h>
#include <haven/version.h>

static void print_banner(const struct boot_info *bi)
{
	kinfo(" ");
	kinfo("  HavenOS %s \"%s\"", HAVENOS_VERSION, HAVENOS_CODENAME);
	kinfo("  %s", HAVENOS_COPYRIGHT);
	kinfo("  a modern 64-bit successor to HavenDOS");
	kinfo(" ");
	kinfo("kernel: built %s (rev %s) for %s",
	      HAVENOS_BUILD_DATE, HAVENOS_BUILD_REV, HAVENOS_ARCH);
	kinfo("kernel: image %llu KiB at physical 0x%llx",
	      (unsigned long long)((bi->kernel_phys_end -
	                            bi->kernel_phys_start) / 1024),
	      (unsigned long long)bi->kernel_phys_start);
}

static void report_memory_map(const struct boot_info *bi)
{
	kinfo("memory: firmware reported %u regions, %llu MiB usable",
	      bi->memmap_count,
	      (unsigned long long)(bi->ram_total / (1024 * 1024)));

	for (uint32_t i = 0; i < bi->memmap_count; i++) {
		const struct boot_memmap_entry *e = &bi->memmap[i];
		kdebug("memory:   0x%016llx..0x%016llx  %8llu KiB  %s",
		       (unsigned long long)e->base,
		       (unsigned long long)(e->base + e->length),
		       (unsigned long long)(e->length / 1024),
		       boot_mem_type_name(e->type));
	}
}

/*
 * Prove the memory subsystem works before anything relies on it.
 *
 * This runs on every boot, not only under a test flag.  An allocator that
 * is subtly wrong corrupts unrelated state hours later; eighty
 * microseconds of checking at boot is a trade worth making every time,
 * and it is the same reasoning behind HavenDOS adding heap_check() to its
 * shell — except it runs automatically rather than when someone thinks to
 * type it.
 */
static void verify_memory_subsystem(void)
{
	/* Physical: allocate, verify distinctness and alignment, free. */
	paddr_t pages[8];
	bool    ok = true;

	for (size_t i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = pmm_alloc_page_zeroed();
		if (!pages[i] || !IS_ALIGNED(pages[i], HAVEN_PAGE_SIZE))
			ok = false;
		for (size_t j = 0; j < i; j++) {
			if (pages[i] == pages[j])
				ok = false;
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(pages); i++) {
		if (pages[i])
			pmm_free_page(pages[i]);
	}
	if (!ok)
		panic("mm: the physical allocator returned overlapping or "
		      "misaligned pages");
	if (!pmm_check())
		panic("mm: the physical allocator is inconsistent");

	/* Virtual: a multi-page arena allocation must be readable and
	   writable across its whole extent.  Any mapping gap shows up here
	   rather than as a fault in whatever used the memory next. */
	const size_t probe_len = 64 * 1024;
	vaddr_t probe = vmm_alloc(probe_len, VM_PROT_READ | VM_PROT_WRITE);
	if (!probe)
		panic("mm: cannot allocate a %zu KiB virtual region",
		      probe_len / 1024);

	volatile uint64_t *p = (volatile uint64_t *)probe;
	size_t words = probe_len / sizeof(uint64_t);
	for (size_t i = 0; i < words; i++)
		p[i] = 0x5A5A0000ULL + i;
	for (size_t i = 0; i < words; i++) {
		if (p[i] != 0x5A5A0000ULL + i)
			panic("mm: virtual mapping is not coherent at offset "
			      "%zu (read 0x%llx)", i * sizeof(uint64_t),
			      (unsigned long long)p[i]);
	}
	vmm_free(probe, probe_len);

	/* Objects: every size class, plus a large allocation, plus the
	   corruption detectors. */
	void *objs[10];
	size_t sizes[10] = { 8, 16, 40, 100, 200, 500, 1000, 2000, 5000, 70000 };
	for (size_t i = 0; i < ARRAY_SIZE(objs); i++) {
		objs[i] = kzalloc(sizes[i]);
		if (!objs[i])
			panic("mm: kzalloc(%zu) failed during startup "
			      "verification", sizes[i]);
		memset(objs[i], (int)(0x40 + i), sizes[i]);
	}
	if (!kmalloc_check())
		panic("mm: the object allocator is inconsistent");
	for (size_t i = 0; i < ARRAY_SIZE(objs); i++)
		kfree(objs[i]);
	if (!kmalloc_check())
		panic("mm: the object allocator is inconsistent after free");

	kinfo("mm: startup verification passed "
	      "(physical, virtual and object allocators)");
}

void haven_kmain(struct boot_info *bi)
{
	/* ── Stage 1: identity ──────────────────────────────────────────
	   Provides: the banner and the image's own description.
	   Requires: the console, established by the architecture entry. */
	print_banner(bi);
	arch_init();
	report_memory_map(bi);

	/* ── Stage 2: per-CPU state ─────────────────────────────────────
	   Provides: this_cpu(), preempt_disable(), arch_cpu_id().
	   Requires: nothing.  Deliberately before memory management, so
	   that an allocator failure can already report which CPU it was on. */
	smp_init_bsp();

	/* ── Stage 3: physical memory ───────────────────────────────────
	   Provides: pmm_alloc_page() and friends.
	   Requires: the firmware memory map and the kernel image bounds,
	   both of which arrive in boot_info. */
	pmm_init(bi);

	/* ── Stage 4: virtual memory ────────────────────────────────────
	   Provides: the real kernel address space, the direct map over all
	   of RAM, the kernel arena, the MMIO window.
	   Requires: the PMM, for page-table pages.  Lifts the PMM's early
	   allocation bound as its last act. */
	vmm_init(bi);

	/* ── Stage 5: object allocator ──────────────────────────────────
	   Provides: kmalloc() and friends.
	   Requires: the VMM arena as its backing store. */
	kmalloc_init();

	/* ── Stage 6: prove stages 3-5 before anything depends on them ── */
	verify_memory_subsystem();

	/* ── Stage 7: lock down the kernel image ────────────────────────
	   Provides: W^X on the kernel image -- .text read-execute, .rodata
	   and .data non-executable.
	   Requires: the kernel image mapped at 4 KiB granularity, which
	   vmm_init() arranged for exactly this purpose.
	   Done after verification, so a memory bug is diagnosed before the
	   kernel's own pages become read-only. */
	vmm_protect_kernel();

	pmm_dump();
	kmalloc_dump();

	kinfo(" ");
	kinfo("kernel: reached the end of the implemented boot sequence");
	kinfo("kernel: idling");

	for (;;)
		arch_idle();
}
