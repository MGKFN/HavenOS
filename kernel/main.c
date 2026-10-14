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
#include <haven/printf.h>
#include <haven/irq.h>
#include <haven/time.h>
#include <haven/sched.h>
#include <haven/syscall.h>
#include <haven/selftest.h>
#include <haven/vfs.h>
#include <haven/ramfs.h>
#include <haven/devfs.h>
#include <haven/random.h>
#include <haven/proc.h>
#include <haven/errno.h>

/*
 * Is `flag` present in the kernel command line, as a whole word?
 *
 * Whole-word matching matters: a substring test would see "nosmp" inside
 * "nosmpfoo", and more importantly would see "selftest" inside a path.
 * HavenDOS's apply_cmdline() used a token comparison for the same reason.
 */
static bool cmdline_has(const struct boot_info *bi, const char *flag)
{
	size_t flen = strlen(flag);
	const char *p = bi->cmdline;

	while (*p) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;

		const char *start = p;
		while (*p && *p != ' ')
			p++;

		size_t tlen = (size_t)(p - start);
		if (tlen == flen && strncmp(start, flag, flen) == 0)
			return true;
	}
	return false;
}

/*
 * Build the initial namespace.
 *
 * A RAM filesystem at the root and a device filesystem at /dev, with the
 * standard directories created so that anything looking for /tmp or /etc
 * finds them.  HavenDOS created its equivalent set in create_sysfiles(),
 * writing README.TXT, CHANGELOG.TXT and an ETC directory into its ramfs
 * on every boot; the same idea, with real directories rather than names
 * containing slashes.
 */
static void mount_root_filesystems(void)
{
	int rc = ramfs_mount_at("/");
	if (rc != 0)
		panic("vfs: cannot mount the root filesystem: %s",
		      haven_strerror(rc));

	/* Standard directories.  Created before /dev is mounted over one of
	   them, so the mount point exists as a directory either way. */
	static const char *const dirs[] = {
		"/dev", "/tmp", "/etc", "/bin", "/home", "/var", "/proc",
	};
	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		rc = vfs_mkdir(dirs[i], VFS_PERM_DEFAULT_DIR);
		if (rc != 0 && rc != -HAVEN_EEXIST)
			kwarn("vfs: cannot create %s: %s", dirs[i],
			      haven_strerror(rc));
	}

	rc = devfs_mount_at("/dev");
	if (rc != 0) {
		/* Not fatal: the kernel log still works, and saying so is
		   more useful than stopping the boot. */
		kerr("vfs: cannot mount /dev: %s; devices will not be "
		     "reachable as files", haven_strerror(rc));
	} else {
		mem_devices_init();
	}

	/* A release file, generated from the one version constant rather
	   than written as a literal -- which is the drift HavenDOS's
	   ETC/SYSCONF.TXT suffered, reporting VERSION=0.6.1 while the
	   kernel was 0.7.18. */
	char release[256];
	int n = snprintf(release, sizeof(release),
	                 "NAME=HavenOS\n"
	                 "VERSION=%s\n"
	                 "CODENAME=%s\n"
	                 "ARCH=%s\n"
	                 "BUILD_DATE=%s\n"
	                 "BUILD_REV=%s\n"
	                 "ANCESTOR=HavenDOS\n",
	                 HAVENOS_VERSION, HAVENOS_CODENAME, HAVENOS_ARCH,
	                 HAVENOS_BUILD_DATE, HAVENOS_BUILD_REV);
	if (n > 0)
		vfs_write_file("/etc/os-release", release, (size_t)n);
}

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

	/* ── Stage 8: interrupts ────────────────────────────────────────
	   Provides: CPU exception handling, the IRQ registry, the interrupt
	   controllers.
	   Requires: the object allocator, for shared-line handler records,
	   and the MMIO window, for mapping the Local APIC. */
	arch_cpu_init_tables();
	irq_init();

	/* ── Stage 9: timekeeping ───────────────────────────────────────
	   Provides: the tick, one-shot timers, blocking sleep.
	   Requires: the IRQ layer, to take the timer interrupt. */
	time_init();

	/* ── Stage 10: the scheduler ────────────────────────────────────
	   Provides: threads, preemption, blocking.
	   Requires: the allocator (thread structures and kernel stacks)
	   and per-CPU state (run queues).  The timer is started AFTER
	   sched_init(), because a tick that arrives before there is a
	   current thread has nothing to account time to. */
	sched_init();
	arch_timer_init(HZ);
	arch_irq_enable();

	kinfo("kernel: interrupts enabled; the system is now preemptive");

	/* ── Stage 11: system calls ─────────────────────────────────────
	   Provides: the user/kernel boundary.
	   Requires: the GDT's user segments and a per-CPU kernel stack in
	   the TSS, both established in stage 8, and the scheduler, since a
	   syscall can block. */
	syscall_init();

	/* ── Stage 12: filesystems ──────────────────────────────────────
	   Provides: a namespace, /dev, and the file-backed syscalls.
	   Requires: the allocator (nodes and open-file structures) and
	   mutexes (which block, so the scheduler must be running). */
	vfs_init();
	mount_root_filesystems();

	/* ── Stage 13: processes ────────────────────────────────────────
	   Provides: address-space isolation and ring-3 execution.
	   Requires: user segments and a TSS (stage 8), the scheduler
	   (stage 10), the syscall entry (stage 11) and a filesystem to
	   load executables from (stage 12). */
	proc_init();

	/* ── Stage 14: report, and verify on request ───────────────────── */
	pmm_dump();
	kmalloc_dump();
	sched_dump();
	vfs_dump();

	if (cmdline_has(bi, "selftest")) {
		bool ok = selftest_run_all();
		if (!ok)
			kerr("kernel: self-tests FAILED; see the log above");
	} else {
		kinfo("kernel: pass `selftest` on the kernel command line "
		      "(or pick it from the GRUB menu) to run the in-kernel "
		      "test suite");
	}

	kinfo(" ");
	kinfo("kernel: boot complete; entering the idle loop");
	kinfo("kernel: %u CPU online, %llu MiB RAM, scheduler running",
	      smp_online_count(),
	      (unsigned long long)(bi->ram_total / (1024 * 1024)));

	/* The boot thread has nothing left to do.  Rather than spinning, it
	   becomes a low-priority thread that simply blocks, so the idle
	   thread takes over and the CPU actually halts between interrupts. */
	for (;;) {
		sched_preempt_if_needed();
		arch_idle();
	}
}
