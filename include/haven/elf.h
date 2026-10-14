/* SPDX-License-Identifier: MIT
 *
 * elf.h — ELF64 structures and the loader interface.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had no program loader: every program was a C function linked
 * into the kernel.  It did, however, have to deal with ELF in one place,
 * and the way it did is instructive.  Its installer needed to write a
 * bootable kernel to disk, but at runtime the loaded image at 0x100000
 * starts with a Multiboot header rather than ELF magic, so GRUB could not
 * reload it.  Its solution was a documented two-pass build that embedded
 * a complete copy of the pass-one ELF inside the pass-two kernel as
 * kernel_elf_data[], roughly doubling the image size.
 *
 * With a real ELF loader none of that is necessary: a program is a file,
 * and loading it is reading its program headers and mapping them.
 *
 * EVERY FIELD IN AN ELF FILE IS UNTRUSTED
 * An executable may be hostile or simply corrupt.  Offsets can point
 * outside the file, sizes can overflow when added, segment addresses can
 * overlap the kernel, and p_memsz can be smaller than p_filesz.  The
 * loader validates each of those before acting on it; the checks are
 * listed in kernel/proc/elf.c.
 */
#ifndef _HAVEN_ELF_H
#define _HAVEN_ELF_H

#include <haven/types.h>

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1

#define ET_NONE 0
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3
#define ET_CORE 4

#define EM_X86_64  62
#define EM_AARCH64 183

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6
#define PT_TLS     7
#define PT_GNU_STACK 0x6474E551

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

struct elf64_header {
	uint8_t  e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} HAVEN_PACKED;

struct elf64_phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} HAVEN_PACKED;

HAVEN_STATIC_ASSERT(sizeof(struct elf64_header) == 64,
                    "ELF64 header is 64 bytes");
HAVEN_STATIC_ASSERT(sizeof(struct elf64_phdr) == 56,
                    "ELF64 program header is 56 bytes");

/* What the loader produces. */
struct elf_image {
	vaddr_t entry;
	vaddr_t load_start;    /* lowest mapped address */
	vaddr_t load_end;      /* highest mapped address, page-rounded */
	size_t  segments;
};

struct arch_pagetable;

/*
 * Validate an ELF64 header without loading anything.  Separated so a
 * caller can report "not an executable" cheaply and so the validation is
 * directly testable -- which matters because every field here is
 * attacker-controlled.  Returns 0 or -HAVEN_E*.
 */
int elf_validate(const void *data, size_t len, uint16_t expect_machine);

/*
 * Load every PT_LOAD segment of the image in `data` into `pt`.  The
 * caller owns `pt` and must tear it down on failure; the loader does not
 * attempt partial cleanup, because a half-loaded address space has no
 * valid interpretation.
 */
int elf_load(struct arch_pagetable *pt, const void *data, size_t len,
             struct elf_image *out);

const char *elf_type_name(uint16_t type);
const char *elf_machine_name(uint16_t machine);

#endif /* _HAVEN_ELF_H */
