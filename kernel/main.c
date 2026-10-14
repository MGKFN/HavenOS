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
 */
#include <haven/boot.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/arch.h>
#include <haven/string.h>
#include <haven/version.h>

static void print_banner(const struct boot_info *bi)
{
	kinfo(" ");
	kinfo("  HavenOS %s \"%s\"", HAVENOS_VERSION, HAVENOS_CODENAME);
	kinfo("  %s", HAVENOS_COPYRIGHT);
	kinfo("  a modern 64-bit successor to HavenDOS");
	kinfo(" ");
	kinfo("kernel: built %s for %s", HAVENOS_BUILD_DATE, HAVENOS_ARCH);
	kinfo("kernel: image %llu KiB at physical 0x%llx",
	      (unsigned long long)((bi->kernel_phys_end - bi->kernel_phys_start) / 1024),
	      (unsigned long long)bi->kernel_phys_start);
}

static void report_memory(const struct boot_info *bi)
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

void haven_kmain(struct boot_info *bi)
{
	print_banner(bi);
	arch_init();
	report_memory(bi);

	kinfo("kernel: reached the end of the implemented boot sequence");
	kinfo("kernel: idling");

	for (;;)
		arch_idle();
}
