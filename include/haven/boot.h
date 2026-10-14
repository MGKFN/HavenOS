/* SPDX-License-Identifier: MIT
 *
 * boot.h — architecture-neutral boot information.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS parsed Multiboot1 and Multiboot2 tags directly inside
 * kernel/main.c, and the parsing bugs that resulted are documented in its
 * own source: the mmap `type` field was read from the wrong offset so the
 * "more accurate" memory map was dead code for several releases, and the
 * Multiboot2 mmap tag was never parsed at all, pinning RAM to 64 MiB on
 * every UEFI boot.  Both bugs were possible because firmware-specific
 * structure layouts were spread through architecture-neutral code.
 *
 * HavenOS confines all firmware parsing to arch/<arch>/boot/.  That code
 * fills in exactly one struct boot_info, which is the only thing the
 * generic kernel ever reads.  A second boot protocol — or an ARM64 device
 * tree — is then a new producer of this struct, not a new set of
 * conditionals in kernel/main.c.
 */
#ifndef _HAVEN_BOOT_H
#define _HAVEN_BOOT_H

#include <haven/types.h>

#define BOOT_MAX_MEMMAP   64
#define BOOT_MAX_MODULES  8
#define BOOT_CMDLINE_MAX  256

enum boot_mem_type {
	BOOT_MEM_USABLE = 1,     /* free RAM the PMM may hand out */
	BOOT_MEM_RESERVED,       /* firmware-owned, never touch */
	BOOT_MEM_ACPI_RECLAIM,   /* ACPI tables; usable after ACPI parsing */
	BOOT_MEM_ACPI_NVS,       /* must be preserved across sleep */
	BOOT_MEM_BAD,            /* firmware reported defective */
	BOOT_MEM_BOOTLOADER,     /* loader structures; reclaimable later */
	BOOT_MEM_KERNEL,         /* the kernel image itself */
	BOOT_MEM_FRAMEBUFFER,    /* MMIO aperture for the display */
};

struct boot_memmap_entry {
	paddr_t  base;
	uint64_t length;
	uint32_t type;           /* enum boot_mem_type */
	uint32_t _pad;
};

enum boot_fb_format {
	BOOT_FB_NONE = 0,
	BOOT_FB_RGB,             /* linear packed pixels */
	BOOT_FB_EGA_TEXT,        /* 2 bytes per cell: char + attribute */
};

struct boot_framebuffer {
	paddr_t  address;
	uint32_t width;          /* pixels, or columns for EGA text */
	uint32_t height;         /* pixels, or rows for EGA text */
	uint32_t pitch;          /* bytes per scanline */
	uint8_t  bpp;
	uint8_t  format;         /* enum boot_fb_format */
	uint8_t  red_shift,   red_bits;
	uint8_t  green_shift, green_bits;
	uint8_t  blue_shift,  blue_bits;
	uint8_t  _pad[2];
};

struct boot_module {
	paddr_t  start;
	paddr_t  end;
	char     name[64];
};

enum boot_firmware {
	BOOT_FW_UNKNOWN = 0,
	BOOT_FW_BIOS,
	BOOT_FW_UEFI,
};

struct boot_info {
	/* Identification of how we got here. */
	uint32_t                 protocol_magic;
	uint8_t                  firmware;       /* enum boot_firmware */
	uint8_t                  _pad[3];
	char                     loader_name[64];
	char                     cmdline[BOOT_CMDLINE_MAX];

	/* Physical memory. */
	struct boot_memmap_entry memmap[BOOT_MAX_MEMMAP];
	uint32_t                 memmap_count;
	uint64_t                 ram_total;      /* sum of USABLE regions */
	uint64_t                 ram_highest;    /* top of the highest region */

	/* Where the kernel image physically lives, so the PMM can reserve it
	   from the linker's own symbols rather than from a hard-coded size.
	   HavenDOS's pmm.c carried two successive fixed reserve constants
	   (1 MiB, then 4 MiB) that both ended up smaller than the image. */
	paddr_t                  kernel_phys_start;
	paddr_t                  kernel_phys_end;

	/* Display handed over by firmware, if any. */
	struct boot_framebuffer  framebuffer;

	/* Loader-supplied modules (initrd and friends). */
	struct boot_module       modules[BOOT_MAX_MODULES];
	uint32_t                 module_count;

	/* Firmware tables, 0 when not provided. */
	paddr_t                  rsdp;           /* ACPI root pointer */
	paddr_t                  smbios;
	paddr_t                  dtb;            /* ARM64 device tree */
};

/* Filled by arch/<arch>/boot before haven_kmain() runs. */
extern struct boot_info haven_boot_info;

/* Architecture-neutral kernel entry.  Never returns. */
void haven_kmain(struct boot_info *info) HAVEN_NORETURN;

const char *boot_mem_type_name(uint32_t type);

#endif /* _HAVEN_BOOT_H */
