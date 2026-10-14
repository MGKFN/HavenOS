/* SPDX-License-Identifier: MIT
 *
 * multiboot2.c — translate the Multiboot2 handover into struct boot_info.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS parsed these structures inline in kernel/main.c, and its own
 * source comments record what that cost:
 *
 *   - the Multiboot1 mmap `type` field was read from offset 16, which is
 *     the *upper* dword of `length` — zero for every region below 4 GiB.
 *     `type == 1` therefore never matched, the "more accurate" memory-map
 *     path was dead code for several releases, and RAM size silently fell
 *     back to mem_upper.
 *   - the Multiboot2 mmap tag (type 6) was never parsed at all, so RAM was
 *     pinned to 64 MiB on every UEFI boot regardless of the machine.
 *
 * Both bugs share one root cause: structure offsets written as literals at
 * the point of use. This file instead declares every Multiboot2 structure
 * as a real C type with _Static_assert on its size, so a wrong offset is a
 * compile error rather than a boot-time mystery. The entry-size field from
 * the tag is still honoured for the stride, because the spec allows it to
 * grow — but each entry's fields are read through the struct.
 */
#include <haven/boot.h>
#include <haven/console.h>
#include <haven/string.h>
#include <haven/arch.h>
#include <haven/multiboot2.h>

#define MB2_LOADER_MAGIC 0x36D76289u

enum mb2_tag_type {
	MB2_TAG_END            = 0,
	MB2_TAG_CMDLINE        = 1,
	MB2_TAG_LOADER_NAME    = 2,
	MB2_TAG_MODULE         = 3,
	MB2_TAG_BASIC_MEMINFO  = 4,
	MB2_TAG_BOOTDEV        = 5,
	MB2_TAG_MMAP           = 6,
	MB2_TAG_VBE            = 7,
	MB2_TAG_FRAMEBUFFER    = 8,
	MB2_TAG_ELF_SECTIONS   = 9,
	MB2_TAG_APM            = 10,
	MB2_TAG_EFI32          = 11,
	MB2_TAG_EFI64          = 12,
	MB2_TAG_SMBIOS         = 13,
	MB2_TAG_ACPI_OLD       = 14,   /* RSDP v1 */
	MB2_TAG_ACPI_NEW       = 15,   /* RSDP v2+ */
	MB2_TAG_NETWORK        = 16,
	MB2_TAG_EFI_MMAP       = 17,
	MB2_TAG_EFI_BS         = 18,   /* boot services not terminated */
	MB2_TAG_EFI32_IH       = 19,
	MB2_TAG_EFI64_IH       = 20,
	MB2_TAG_LOAD_BASE_ADDR = 21,
};

struct mb2_tag {
	uint32_t type;
	uint32_t size;
} HAVEN_PACKED;

struct mb2_tag_string {
	uint32_t type;
	uint32_t size;
	char     string[];
} HAVEN_PACKED;

struct mb2_tag_module {
	uint32_t type;
	uint32_t size;
	uint32_t mod_start;
	uint32_t mod_end;
	char     cmdline[];
} HAVEN_PACKED;

struct mb2_tag_basic_meminfo {
	uint32_t type;
	uint32_t size;
	uint32_t mem_lower;    /* KiB below 1 MiB */
	uint32_t mem_upper;    /* KiB above 1 MiB */
} HAVEN_PACKED;

/* The memory-map entry.  This is the structure whose field offsets
   HavenDOS got wrong: addr at 0, len at 8, type at 16, reserved at 20. */
struct mb2_mmap_entry {
	uint64_t addr;
	uint64_t len;
	uint32_t type;
	uint32_t zero;
} HAVEN_PACKED;

HAVEN_STATIC_ASSERT(sizeof(struct mb2_mmap_entry) == 24,
                    "Multiboot2 mmap entry must be 24 bytes");
HAVEN_STATIC_ASSERT(offsetof(struct mb2_mmap_entry, type) == 16,
                    "Multiboot2 mmap type field must be at offset 16");

struct mb2_tag_mmap {
	uint32_t type;
	uint32_t size;
	uint32_t entry_size;
	uint32_t entry_version;
	uint8_t  entries[];
} HAVEN_PACKED;

enum mb2_mem_type {
	MB2_MEM_AVAILABLE        = 1,
	MB2_MEM_RESERVED         = 2,
	MB2_MEM_ACPI_RECLAIMABLE = 3,
	MB2_MEM_NVS              = 4,
	MB2_MEM_BADRAM           = 5,
};

struct mb2_tag_framebuffer {
	uint32_t type;
	uint32_t size;
	uint64_t addr;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint8_t  bpp;
	uint8_t  fb_type;      /* 0 = indexed, 1 = RGB, 2 = EGA text */
	uint16_t reserved;
	/* RGB layout, present when fb_type == 1 */
	uint8_t  red_field_position;
	uint8_t  red_mask_size;
	uint8_t  green_field_position;
	uint8_t  green_mask_size;
	uint8_t  blue_field_position;
	uint8_t  blue_mask_size;
} HAVEN_PACKED;

#define MB2_FB_INDEXED  0
#define MB2_FB_RGB      1
#define MB2_FB_EGA_TEXT 2

struct mb2_tag_acpi {
	uint32_t type;
	uint32_t size;
	uint8_t  rsdp[];
} HAVEN_PACKED;

struct mb2_tag_u64 {
	uint32_t type;
	uint32_t size;
	uint64_t value;
} HAVEN_PACKED;

/* ── Linker-provided image bounds.  The PMM reserves exactly this. ──── */
extern uint8_t __kernel_phys_start[];
extern uint8_t __kernel_phys_end[];

struct boot_info haven_boot_info;

const char *boot_mem_type_name(uint32_t type)
{
	switch (type) {
	case BOOT_MEM_USABLE:        return "usable";
	case BOOT_MEM_RESERVED:      return "reserved";
	case BOOT_MEM_ACPI_RECLAIM:  return "ACPI reclaimable";
	case BOOT_MEM_ACPI_NVS:      return "ACPI NVS";
	case BOOT_MEM_BAD:           return "bad RAM";
	case BOOT_MEM_BOOTLOADER:    return "bootloader";
	case BOOT_MEM_KERNEL:        return "kernel image";
	case BOOT_MEM_FRAMEBUFFER:   return "framebuffer";
	default:                     return "unknown";
	}
}

static uint32_t translate_mem_type(uint32_t mb2_type)
{
	switch (mb2_type) {
	case MB2_MEM_AVAILABLE:        return BOOT_MEM_USABLE;
	case MB2_MEM_ACPI_RECLAIMABLE: return BOOT_MEM_ACPI_RECLAIM;
	case MB2_MEM_NVS:              return BOOT_MEM_ACPI_NVS;
	case MB2_MEM_BADRAM:           return BOOT_MEM_BAD;
	case MB2_MEM_RESERVED:
	default:
		/* Anything the spec does not define is treated as reserved.
		   Treating an unknown type as usable would hand firmware
		   memory to the allocator. */
		return BOOT_MEM_RESERVED;
	}
}

static void add_memmap(struct boot_info *bi, paddr_t base, uint64_t len,
                       uint32_t type)
{
	if (len == 0)
		return;
	if (bi->memmap_count >= BOOT_MAX_MEMMAP) {
		/* Not silently dropped: a machine with a very fragmented map
		   would otherwise lose RAM with no explanation. */
		kwarn("multiboot2: memory map truncated at %u entries",
		      BOOT_MAX_MEMMAP);
		return;
	}

	bi->memmap[bi->memmap_count].base   = base;
	bi->memmap[bi->memmap_count].length = len;
	bi->memmap[bi->memmap_count].type   = type;
	bi->memmap_count++;

	if (type == BOOT_MEM_USABLE) {
		bi->ram_total += len;
		if (base + len > bi->ram_highest)
			bi->ram_highest = base + len;
	}
}

static void parse_mmap(struct boot_info *bi, const struct mb2_tag_mmap *tag)
{
	uint32_t entry_size = tag->entry_size;

	/* The spec lets entry_size grow in future versions, so the stride
	   comes from the tag.  It must still be at least as large as the
	   structure we read through, or we would read past each entry. */
	if (entry_size < sizeof(struct mb2_mmap_entry)) {
		kerr("multiboot2: mmap entry_size %u is smaller than the "
		     "24-byte entry structure; ignoring the memory map",
		     entry_size);
		return;
	}
	if (entry_size > 256) {
		kerr("multiboot2: implausible mmap entry_size %u", entry_size);
		return;
	}

	uint32_t header = (uint32_t)offsetof(struct mb2_tag_mmap, entries);
	if (tag->size <= header) {
		kwarn("multiboot2: mmap tag carries no entries");
		return;
	}

	uint32_t payload = tag->size - header;
	uint32_t count   = payload / entry_size;

	for (uint32_t i = 0; i < count; i++) {
		const struct mb2_mmap_entry *e =
			(const struct mb2_mmap_entry *)(tag->entries +
			                                (size_t)i * entry_size);
		add_memmap(bi, e->addr, e->len, translate_mem_type(e->type));
	}
}

static void parse_framebuffer(struct boot_info *bi,
                              const struct mb2_tag_framebuffer *tag)
{
	struct boot_framebuffer *f = &bi->framebuffer;

	f->address = tag->addr;
	f->width   = tag->width;
	f->height  = tag->height;
	f->pitch   = tag->pitch;
	f->bpp     = tag->bpp;

	switch (tag->fb_type) {
	case MB2_FB_RGB:
		f->format      = BOOT_FB_RGB;
		f->red_shift   = tag->red_field_position;
		f->red_bits    = tag->red_mask_size;
		f->green_shift = tag->green_field_position;
		f->green_bits  = tag->green_mask_size;
		f->blue_shift  = tag->blue_field_position;
		f->blue_bits   = tag->blue_mask_size;
		break;
	case MB2_FB_EGA_TEXT:
		f->format = BOOT_FB_EGA_TEXT;
		break;
	case MB2_FB_INDEXED:
	default:
		/* An indexed (palette) mode would need the palette uploaded
		   before anything is legible.  Rather than render unreadable
		   colours, decline the display and stay on serial — which is
		   a real improvement over HavenDOS, where a display that did
		   not work meant no output at all. */
		kwarn("multiboot2: indexed-colour framebuffer is not supported; "
		      "console stays on serial");
		f->format = BOOT_FB_NONE;
		return;
	}

	/* The framebuffer aperture is not RAM.  Record it so the PMM cannot
	   hand it out as free memory even if firmware omitted it from the
	   memory map — which some do. */
	uint64_t fb_bytes = (uint64_t)f->pitch * f->height;
	if (fb_bytes)
		add_memmap(bi, f->address, fb_bytes, BOOT_MEM_FRAMEBUFFER);
}

static void parse_module(struct boot_info *bi,
                         const struct mb2_tag_module *tag)
{
	if (bi->module_count >= BOOT_MAX_MODULES) {
		kwarn("multiboot2: ignoring modules beyond the first %u",
		      BOOT_MAX_MODULES);
		return;
	}

	struct boot_module *m = &bi->modules[bi->module_count++];
	m->start = tag->mod_start;
	m->end   = tag->mod_end;

	size_t name_max = tag->size > offsetof(struct mb2_tag_module, cmdline)
	                ? tag->size - offsetof(struct mb2_tag_module, cmdline)
	                : 0;
	if (name_max > sizeof(m->name))
		name_max = sizeof(m->name);
	if (name_max) {
		memcpy(m->name, tag->cmdline, name_max);
		m->name[sizeof(m->name) - 1] = '\0';
	}

	/* Loader modules sit in RAM the loader claimed; keep the PMM off
	   them until whoever consumes the module says it is done. */
	if (m->end > m->start)
		add_memmap(bi, m->start, m->end - m->start, BOOT_MEM_BOOTLOADER);
}

/* Bounded copy out of a tag whose string may not be NUL-terminated within
   the tag.  The spec says it is, but a malformed loader must not be able
   to run us off the end of the structure. */
static void copy_tag_string(char *dst, size_t dstsz,
                            const struct mb2_tag_string *tag, size_t header)
{
	if (tag->size <= header) {
		dst[0] = '\0';
		return;
	}

	size_t avail = tag->size - header;
	size_t n     = strnlen(tag->string, avail);
	if (n >= dstsz)
		n = dstsz - 1;
	memcpy(dst, tag->string, n);
	dst[n] = '\0';
}

/*
 * Parse the Multiboot2 information structure at physical address `info_pa`
 * into haven_boot_info.  Returns 0 on success.
 *
 * Called with the identity map still live, but every access goes through
 * the direct map anyway, so this function is already correct once the
 * identity window is gone.
 */
int multiboot2_parse(uint32_t magic, uint32_t info_pa, struct boot_info *bi)
{
	memset(bi, 0, sizeof(*bi));

	bi->protocol_magic    = magic;
	bi->kernel_phys_start = (paddr_t)(uintptr_t)__kernel_phys_start;
	bi->kernel_phys_end   = (paddr_t)(uintptr_t)__kernel_phys_end;
	bi->firmware          = BOOT_FW_UNKNOWN;
	strlcpy(bi->loader_name, "unknown", sizeof(bi->loader_name));

	if (magic != MB2_LOADER_MAGIC) {
		kerr("boot: loader magic 0x%08x is not Multiboot2 (0x%08x); "
		     "HavenOS requires a Multiboot2 loader such as GRUB 2",
		     magic, MB2_LOADER_MAGIC);
		return -1;
	}
	if (info_pa == 0 || (info_pa & 7)) {
		kerr("boot: Multiboot2 info pointer 0x%08x is null or "
		     "misaligned", info_pa);
		return -1;
	}

	const uint8_t *base = (const uint8_t *)arch_phys_to_virt(info_pa);
	uint32_t total_size = *(const uint32_t *)base;

	if (total_size < 8 || total_size > (1u << 20)) {
		kerr("boot: implausible Multiboot2 info size %u", total_size);
		return -1;
	}

	/* Walk the tags.  Each tag is 8-byte aligned; size counts the header.
	   A zero or sub-header size would spin here forever, so both are
	   checked — HavenDOS's own walker added `if(!n) break;` for exactly
	   this reason. */
	size_t off = 8;
	bool   saw_end = false;

	while (off + sizeof(struct mb2_tag) <= total_size) {
		const struct mb2_tag *tag = (const struct mb2_tag *)(base + off);

		if (tag->type == MB2_TAG_END) {
			saw_end = true;
			break;
		}
		if (tag->size < sizeof(struct mb2_tag)) {
			kerr("boot: Multiboot2 tag %u at offset %zu has size %u",
			     tag->type, off, tag->size);
			return -1;
		}
		if (off + tag->size > total_size) {
			kerr("boot: Multiboot2 tag %u runs past the info block",
			     tag->type);
			return -1;
		}

		switch (tag->type) {
		case MB2_TAG_CMDLINE:
			copy_tag_string(bi->cmdline, sizeof(bi->cmdline),
			                (const struct mb2_tag_string *)tag,
			                offsetof(struct mb2_tag_string, string));
			break;

		case MB2_TAG_LOADER_NAME:
			copy_tag_string(bi->loader_name, sizeof(bi->loader_name),
			                (const struct mb2_tag_string *)tag,
			                offsetof(struct mb2_tag_string, string));
			break;

		case MB2_TAG_MODULE:
			parse_module(bi, (const struct mb2_tag_module *)tag);
			break;

		case MB2_TAG_MMAP:
			parse_mmap(bi, (const struct mb2_tag_mmap *)tag);
			break;

		case MB2_TAG_FRAMEBUFFER:
			parse_framebuffer(bi,
			        (const struct mb2_tag_framebuffer *)tag);
			break;

		case MB2_TAG_ACPI_OLD:
		case MB2_TAG_ACPI_NEW:
			/* The tag embeds the RSDP itself, so its address is
			   the address of the tag payload. */
			if (!bi->rsdp)
				bi->rsdp = arch_virt_to_phys(
				        (vaddr_t)((const struct mb2_tag_acpi *)tag)->rsdp);
			break;

		case MB2_TAG_SMBIOS:
			if (!bi->smbios)
				bi->smbios = info_pa + off + 8;
			break;

		case MB2_TAG_EFI64:
		case MB2_TAG_EFI32:
		case MB2_TAG_EFI64_IH:
		case MB2_TAG_EFI32_IH:
		case MB2_TAG_EFI_MMAP:
			/* The presence of any EFI tag is how we know the
			   firmware is UEFI.  HavenDOS could not distinguish
			   the two paths at runtime and had to infer it from
			   whether a framebuffer appeared. */
			bi->firmware = BOOT_FW_UEFI;
			break;

		case MB2_TAG_BASIC_MEMINFO:
			/* Only a fallback: see the ram_total check below. */
			break;

		default:
			break;
		}

		off += ALIGN_UP((size_t)tag->size, 8);
	}

	if (!saw_end)
		kwarn("boot: Multiboot2 tag list was not terminated by an "
		      "end tag; parsed what was readable");

	if (bi->firmware == BOOT_FW_UNKNOWN)
		bi->firmware = BOOT_FW_BIOS;

	/* Fall back to basic meminfo only if the real map produced nothing.
	   HavenDOS had this the other way round by accident — its mmap
	   parsing was broken, so mem_upper was always what got used, and
	   mem_upper is capped well below the truth on some BIOSes. */
	if (bi->memmap_count == 0 || bi->ram_total == 0) {
		off = 8;
		while (off + sizeof(struct mb2_tag) <= total_size) {
			const struct mb2_tag *tag =
				(const struct mb2_tag *)(base + off);
			if (tag->type == MB2_TAG_END || tag->size < 8)
				break;
			if (tag->type == MB2_TAG_BASIC_MEMINFO) {
				const struct mb2_tag_basic_meminfo *m =
					(const struct mb2_tag_basic_meminfo *)tag;
				uint64_t bytes = (uint64_t)m->mem_upper * 1024;
				kwarn("boot: no usable memory map; falling back "
				      "to basic meminfo (%llu MiB)",
				      (unsigned long long)(bytes >> 20));
				add_memmap(bi, 0x100000, bytes,
				           BOOT_MEM_USABLE);
				break;
			}
			off += ALIGN_UP((size_t)tag->size, 8);
		}
	}

	if (bi->ram_total == 0) {
		kerr("boot: firmware reported no usable RAM");
		return -1;
	}

	/* Finally, mark the kernel image itself.  pmm_init() consumes this
	   entry rather than a hard-coded reserve constant, which is the
	   direct fix for the class of bug HavenDOS hit twice. */
	add_memmap(bi, bi->kernel_phys_start,
	           bi->kernel_phys_end - bi->kernel_phys_start,
	           BOOT_MEM_KERNEL);

	/* The Multiboot2 info block is itself in memory we must not reuse
	   until everything has been copied out of it. */
	add_memmap(bi, info_pa, total_size, BOOT_MEM_BOOTLOADER);

	return 0;
}
