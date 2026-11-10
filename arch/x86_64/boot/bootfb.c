/* SPDX-License-Identifier: MIT
 *
 * bootfb.c -- get the screen working before anything else can go wrong.
 *
 * WHY THIS FILE EXISTS
 * The first time HavenOS was booted on a real laptop it showed nothing.
 * GRUB printed one warning and the screen stayed as GRUB left it.  There
 * was no serial port to read, so there was no way to tell whether the
 * kernel had crashed, hung, or was sitting at a shell prompt nobody could
 * see.  A kernel that cannot say what went wrong on the only output device
 * a machine has cannot be debugged on that machine.
 *
 * So the display is now brought up as early as the kernel knows where it
 * is -- before timekeeping, before memory management -- and by a path
 * that does not depend on where firmware put it.
 *
 * WHY NOT THE DIRECT MAP
 * boot.S direct-maps the first 4 GiB of physical memory.  QEMU puts the
 * framebuffer at 2 GiB, so reaching it through the direct map worked in
 * every test.  Real firmware is free to put a 64-bit graphics BAR above
 * 4 GiB, and current laptops usually do; the first write to it through
 * the early direct map is then a page fault, taken before the IDT exists,
 * which is a triple fault.  Widening the early direct map only moves the
 * limit: an AMD machine may place the aperture near the top of a 48-bit
 * address space.
 *
 * Instead the framebuffer gets a window of its own at a fixed virtual
 * address (BOOTFB_BASE), mapped with 2 MiB pages from one statically
 * allocated page directory.  The same code runs for a framebuffer at
 * 2 GiB and one at 1000 GiB, so what QEMU exercises is what a laptop
 * runs.  The kernel's own page tables recreate the window before they
 * are switched to, so the console's pointer never changes.
 *
 * The mapping uses the default cache attributes.  On real hardware that
 * usually means uncached, which makes scrolling slow; a write-combining
 * mapping needs the PAT programmed and is on the roadmap.
 */
#include <haven/types.h>
#include <haven/boot.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/string.h>
#include <haven/errno.h>
#include <haven/panic.h>
#include <haven/arch/x86_64/boot.h>
#include <haven/arch/x86_64/bootfb.h>
#include <haven/arch/x86_64/memlayout.h>

#define PTE_P    (1ULL << 0)
#define PTE_W    (1ULL << 1)
#define PTE_PS   (1ULL << 7)

#define BOOTFB_PDPT_INDEX  511   /* BOOTFB_BASE within PML4[511] */

HAVEN_STATIC_ASSERT(((BOOTFB_BASE >> 30) & 0x1FF) == BOOTFB_PDPT_INDEX,
                    "BOOTFB_BASE must be the top gigabyte");
HAVEN_STATIC_ASSERT(BOOTFB_MAX_BYTES <= PAGE_SIZE_1G,
                    "the boot framebuffer window is one page directory");

/* One page directory: 512 entries of 2 MiB cover the whole window. */
static uint64_t bootfb_pd[512] __attribute__((aligned(4096)));

/* What was mapped, so the kernel's own tables can repeat it. */
static paddr_t bootfb_phys;     /* 2 MiB aligned */
static size_t  bootfb_bytes;    /* multiple of 2 MiB; 0 = nothing mapped */

/* A statically allocated object in the kernel image, to its physical
   address.  The image window maps physical p at KERNEL_VMA_BASE + p. */
static paddr_t image_phys(const void *p)
{
	return (paddr_t)((vaddr_t)p - KERNEL_VMA_BASE);
}

bool x86_bootfb_map(struct boot_framebuffer *fb)
{
	if (!fb || fb->format != BOOT_FB_RGB || fb->address == 0)
		return true;            /* nothing that needs a window */

	uint64_t bytes = (uint64_t)fb->pitch * fb->height;
	if (bytes == 0)
		return false;

	paddr_t start = ALIGN_DOWN(fb->address, PAGE_SIZE_2M);
	paddr_t end   = ALIGN_UP(fb->address + bytes, PAGE_SIZE_2M);

	if (end <= start || end - start > BOOTFB_MAX_BYTES) {
		kwarn("bootfb: framebuffer of %llu MiB at 0x%llx does not fit "
		      "the %llu MiB boot window; the display stays off until "
		      "the kernel's own page tables are up",
		      (unsigned long long)(bytes >> 20),
		      (unsigned long long)fb->address,
		      (unsigned long long)(BOOTFB_MAX_BYTES >> 20));
		return false;
	}

	size_t pages = (size_t)((end - start) / PAGE_SIZE_2M);
	for (size_t i = 0; i < pages; i++)
		bootfb_pd[i] = (start + i * PAGE_SIZE_2M) | PTE_P | PTE_W | PTE_PS;

	/* Hang the directory off the boot tables.  The entry was not present
	   before, so there is no stale translation to flush. */
	boot_pdpt_high[BOOTFB_PDPT_INDEX] = image_phys(bootfb_pd) | PTE_P | PTE_W;

	bootfb_phys  = start;
	bootfb_bytes = (size_t)(end - start);
	fb->mapped_at = BOOTFB_BASE + (fb->address - start);

	kinfo("bootfb: %ux%u, %u bpp at physical 0x%llx%s, mapped at 0x%llx",
	      fb->width, fb->height, fb->bpp,
	      (unsigned long long)fb->address,
	      fb->address >= 0x100000000ULL ? " (above 4 GiB)" : "",
	      (unsigned long long)fb->mapped_at);
	return true;
}

void x86_bootfb_carry_over(void)
{
	if (bootfb_bytes == 0)
		return;

	int rc = arch_pt_map(NULL, BOOTFB_BASE, bootfb_phys, bootfb_bytes,
	                     VM_PROT_READ | VM_PROT_WRITE | VM_ATTR_GLOBAL);
	if (rc != 0)
		panic("bootfb: cannot keep the display mapped across the "
		      "switch to the kernel's page tables: %s",
		      haven_strerror(rc));
}
