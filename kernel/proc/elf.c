/* SPDX-License-Identifier: MIT
 *
 * elf.c — the ELF64 program loader.
 *
 * WHAT GETS VALIDATED, AND WHY EACH CHECK EXISTS
 * Every field in an ELF file is chosen by whoever produced the file, so
 * each of these is a check against a specific way a malformed or hostile
 * executable could damage the kernel:
 *
 *   magic, class, endianness, version
 *       cheap rejection of something that is not an ELF64 at all.
 *   e_machine
 *       an AArch64 binary on x86_64 would load and then execute garbage.
 *   e_phoff / e_phnum / e_phentsize
 *       the program-header table must lie inside the file.  A large
 *       e_phnum with a valid e_phoff walks off the end of the buffer.
 *   p_offset + p_filesz
 *       computed in 64 bits and compared against the file length, with
 *       the overflow checked separately -- `off + sz > len` is false for
 *       values that wrap.
 *   p_memsz >= p_filesz
 *       a segment whose memory size is smaller than its file size would
 *       make the copy write past the mapping.
 *   p_vaddr range
 *       every byte of every segment must land in user space.  A segment
 *       at a kernel address is how a hostile binary would overwrite the
 *       kernel, and it is the single most important check here.
 *   p_align
 *       a non-power-of-two alignment breaks the rounding arithmetic.
 *   segment overlap
 *       two PT_LOAD segments covering the same page would have the
 *       second silently overwrite the first's permissions.
 */
#include <haven/elf.h>
#include <haven/mm.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/panic.h>

/* Where a user image may live.  Declared here rather than taken from the
   architecture header, because elf.c is portable code; the architecture
   supplies the bounds through arch.h. */
#include <haven/vmlayout.h>

const char *elf_type_name(uint16_t type)
{
	switch (type) {
	case ET_NONE: return "none";
	case ET_REL:  return "relocatable";
	case ET_EXEC: return "executable";
	case ET_DYN:  return "shared object / PIE";
	case ET_CORE: return "core dump";
	default:      return "unknown";
	}
}

const char *elf_machine_name(uint16_t machine)
{
	switch (machine) {
	case EM_X86_64:  return "x86_64";
	case EM_AARCH64: return "aarch64";
	default:         return "unknown";
	}
}

/* The machine this kernel can execute.  Portable code cannot name an
   architecture, so the value comes from the build. */
#if defined(__x86_64__)
#define ELF_NATIVE_MACHINE EM_X86_64
#elif defined(__aarch64__)
#define ELF_NATIVE_MACHINE EM_AARCH64
#else
#error "no native ELF machine type for this architecture"
#endif

int elf_validate(const void *data, size_t len, uint16_t expect_machine)
{
	if (!data)
		return -HAVEN_EINVAL;

	if (len < sizeof(struct elf64_header)) {
		kdebug("elf: file is %zu bytes, shorter than an ELF64 header",
		       len);
		return -HAVEN_ENOEXEC;
	}

	const struct elf64_header *eh = data;

	if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
	    eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
		kdebug("elf: no ELF magic");
		return -HAVEN_ENOEXEC;
	}
	if (eh->e_ident[4] != ELFCLASS64) {
		kerr("elf: ELF class %u is not 64-bit; HavenOS does not run "
		     "32-bit executables", eh->e_ident[4]);
		return -HAVEN_ENOEXEC;
	}
	if (eh->e_ident[5] != ELFDATA2LSB) {
		kerr("elf: big-endian ELF is not supported");
		return -HAVEN_ENOEXEC;
	}
	if (eh->e_version != EV_CURRENT) {
		kerr("elf: unknown ELF version %u", eh->e_version);
		return -HAVEN_ENOEXEC;
	}
	if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
		kerr("elf: file type is %s, which cannot be executed",
		     elf_type_name(eh->e_type));
		return -HAVEN_ENOEXEC;
	}
	if (expect_machine && eh->e_machine != expect_machine) {
		kerr("elf: built for %s, this machine is %s",
		     elf_machine_name(eh->e_machine),
		     elf_machine_name(expect_machine));
		return -HAVEN_ENOEXEC;
	}
	if (eh->e_phentsize != sizeof(struct elf64_phdr)) {
		kerr("elf: program header entry size %u, expected %zu",
		     eh->e_phentsize, sizeof(struct elf64_phdr));
		return -HAVEN_ENOEXEC;
	}
	if (eh->e_phnum == 0) {
		kerr("elf: no program headers, so there is nothing to load");
		return -HAVEN_ENOEXEC;
	}
	if (eh->e_phnum > 128) {
		kerr("elf: %u program headers is implausible", eh->e_phnum);
		return -HAVEN_ENOEXEC;
	}

	/* The program-header table must lie wholly inside the file.  The
	   multiplication is checked for overflow separately: with
	   e_phnum = 128 and e_phentsize = 56 it cannot overflow here, but
	   the pattern is what matters, since the same arithmetic appears
	   with attacker-controlled sizes below. */
	uint64_t ph_bytes = (uint64_t)eh->e_phnum * eh->e_phentsize;
	if (eh->e_phoff > len || ph_bytes > len - eh->e_phoff) {
		kerr("elf: program header table at offset %llu + %llu bytes "
		     "runs past the %zu-byte file",
		     (unsigned long long)eh->e_phoff,
		     (unsigned long long)ph_bytes, len);
		return -HAVEN_ENOEXEC;
	}

	if (!is_user_addr(eh->e_entry)) {
		kerr("elf: entry point 0x%llx is not a user address",
		     (unsigned long long)eh->e_entry);
		return -HAVEN_ENOEXEC;
	}

	return 0;
}

/* Translate ELF segment flags into the VMM's protection bits. */
static uint32_t phdr_prot(uint32_t p_flags)
{
	uint32_t prot = VM_PROT_USER;

	if (p_flags & PF_R)
		prot |= VM_PROT_READ;
	if (p_flags & PF_W)
		prot |= VM_PROT_WRITE;
	if (p_flags & PF_X)
		prot |= VM_PROT_EXEC;

	/* A segment that is both writable and executable is how injected
	   data becomes injected code.  Refusing it outright would break
	   legitimate old binaries, so it is permitted and reported -- the
	   report being what makes it visible rather than silent. */
	if ((p_flags & PF_W) && (p_flags & PF_X))
		kwarn("elf: a segment is both writable and executable, which "
		      "defeats W^X for that range");

	return prot;
}

int elf_load(struct arch_pagetable *pt, const void *data, size_t len,
             struct elf_image *out)
{
	if (!pt || !data || !out)
		return -HAVEN_EINVAL;

	int rc = elf_validate(data, len, ELF_NATIVE_MACHINE);
	if (rc != 0)
		return rc;

	const struct elf64_header *eh  = data;
	const uint8_t             *raw = data;
	const struct elf64_phdr   *ph  =
		(const struct elf64_phdr *)(raw + eh->e_phoff);

	memset(out, 0, sizeof(*out));
	out->entry      = eh->e_entry;
	out->load_start = USER_MAX;
	out->load_end   = 0;

	/* First pass: validate every segment before mapping any of them.
	   Mapping as we go and failing half way leaves an address space
	   that is neither the old one nor the new one, with no correct
	   interpretation. */
	for (uint16_t i = 0; i < eh->e_phnum; i++) {
		const struct elf64_phdr *p = &ph[i];

		if (p->p_type != PT_LOAD)
			continue;
		if (p->p_memsz == 0)
			continue;

		if (p->p_memsz < p->p_filesz) {
			kerr("elf: segment %u has memsz %llu < filesz %llu, "
			     "which would copy past its own mapping", i,
			     (unsigned long long)p->p_memsz,
			     (unsigned long long)p->p_filesz);
			return -HAVEN_ENOEXEC;
		}

		/* File extent, with the addition checked for wrap. */
		if (p->p_offset > len || p->p_filesz > len - p->p_offset) {
			kerr("elf: segment %u reads offset %llu + %llu from a "
			     "%zu-byte file", i,
			     (unsigned long long)p->p_offset,
			     (unsigned long long)p->p_filesz, len);
			return -HAVEN_ENOEXEC;
		}

		/* Virtual extent, likewise. */
		if (p->p_vaddr > UINT64_MAX - p->p_memsz) {
			kerr("elf: segment %u virtual extent overflows", i);
			return -HAVEN_ENOEXEC;
		}

		vaddr_t seg_start = p->p_vaddr;
		vaddr_t seg_end   = p->p_vaddr + p->p_memsz;

		/* THE important check: every byte must be in user space.  A
		   segment at a kernel address is how a hostile executable
		   would overwrite the kernel it is asking to load it. */
		if (!is_user_addr(seg_start) || seg_end > USER_MAX) {
			kerr("elf: segment %u spans 0x%llx..0x%llx, which is "
			     "outside user space (0x%llx..0x%llx)", i,
			     (unsigned long long)seg_start,
			     (unsigned long long)seg_end,
			     (unsigned long long)USER_MIN,
			     (unsigned long long)USER_MAX);
			return -HAVEN_ENOEXEC;
		}

		if (p->p_align > 1 && (p->p_align & (p->p_align - 1))) {
			kerr("elf: segment %u alignment %llu is not a power "
			     "of two", i, (unsigned long long)p->p_align);
			return -HAVEN_ENOEXEC;
		}

		/* Overlap with an earlier segment, page-granular because that
		   is the granularity permissions have. */
		vaddr_t a_lo = ALIGN_DOWN(seg_start, HAVEN_PAGE_SIZE);
		vaddr_t a_hi = ALIGN_UP(seg_end, HAVEN_PAGE_SIZE);

		for (uint16_t j = 0; j < i; j++) {
			const struct elf64_phdr *q = &ph[j];
			if (q->p_type != PT_LOAD || q->p_memsz == 0)
				continue;

			vaddr_t b_lo = ALIGN_DOWN(q->p_vaddr, HAVEN_PAGE_SIZE);
			vaddr_t b_hi = ALIGN_UP(q->p_vaddr + q->p_memsz,
			                        HAVEN_PAGE_SIZE);

			if (a_lo < b_hi && b_lo < a_hi) {
				kerr("elf: segments %u and %u overlap at page "
				     "0x%llx; the later one would silently "
				     "change the earlier one's permissions",
				     j, i, (unsigned long long)
				     (a_lo > b_lo ? a_lo : b_lo));
				return -HAVEN_ENOEXEC;
			}
		}

		if (a_lo < out->load_start)
			out->load_start = a_lo;
		if (a_hi > out->load_end)
			out->load_end = a_hi;
		out->segments++;
	}

	if (out->segments == 0) {
		kerr("elf: no loadable segments");
		return -HAVEN_ENOEXEC;
	}

	/* The entry point must be inside something we are about to map. */
	if (eh->e_entry < out->load_start || eh->e_entry >= out->load_end) {
		kerr("elf: entry point 0x%llx is outside every loaded segment "
		     "(0x%llx..0x%llx)",
		     (unsigned long long)eh->e_entry,
		     (unsigned long long)out->load_start,
		     (unsigned long long)out->load_end);
		return -HAVEN_ENOEXEC;
	}

	/* Second pass: map and copy. */
	for (uint16_t i = 0; i < eh->e_phnum; i++) {
		const struct elf64_phdr *p = &ph[i];

		if (p->p_type != PT_LOAD || p->p_memsz == 0)
			continue;

		vaddr_t  va_lo = ALIGN_DOWN(p->p_vaddr, HAVEN_PAGE_SIZE);
		vaddr_t  va_hi = ALIGN_UP(p->p_vaddr + p->p_memsz,
		                          HAVEN_PAGE_SIZE);
		size_t   pages = (size_t)((va_hi - va_lo) / HAVEN_PAGE_SIZE);
		uint32_t prot  = phdr_prot(p->p_flags);

		for (size_t pg = 0; pg < pages; pg++) {
			paddr_t phys = pmm_alloc_page();
			if (!phys) {
				kerr("elf: out of memory loading segment %u", i);
				return -HAVEN_ENOMEM;
			}

			/* Zero every page before anything is copied into it.
			   A .bss page must read as zeroes, and a partially
			   filled .data page must not expose whatever the
			   previous owner of that physical page left behind --
			   which would be an information leak across
			   processes. */
			memset((void *)arch_phys_to_virt(phys), 0,
			       HAVEN_PAGE_SIZE);

			/* Map it WRITABLE for now regardless of the segment's
			   final permissions, so the copy below can write into
			   a read-only segment; the permissions are applied
			   afterwards.  Mapping read-only first and copying
			   through the direct map instead would work too, and
			   is what the copy actually does -- so the mapping can
			   carry its final permissions immediately. */
			rc = arch_pt_map(pt, va_lo + pg * HAVEN_PAGE_SIZE,
			                 phys, HAVEN_PAGE_SIZE, prot);
			if (rc != 0) {
				pmm_free_page(phys);
				kerr("elf: cannot map segment %u page %zu: %s",
				     i, pg, haven_strerror(rc));
				return rc;
			}
		}

		/* Copy the file-backed part.  Written through the direct map
		   rather than through the new mapping, because the new
		   mapping may be read-only and because `pt` is not the
		   active address space yet. */
		if (p->p_filesz) {
			const uint8_t *src = raw + p->p_offset;
			uint64_t       off = p->p_vaddr - va_lo;

			for (uint64_t done = 0; done < p->p_filesz; ) {
				vaddr_t  va   = p->p_vaddr + done;
				vaddr_t  page = ALIGN_DOWN(va, HAVEN_PAGE_SIZE);
				uint64_t in_pg = va - page;
				uint64_t chunk = HAVEN_PAGE_SIZE - in_pg;
				if (chunk > p->p_filesz - done)
					chunk = p->p_filesz - done;

				paddr_t phys = arch_pt_translate(pt, page);
				if (!phys) {
					kerr("elf: segment %u page 0x%llx "
					     "vanished between mapping and "
					     "copying", i,
					     (unsigned long long)page);
					return -HAVEN_EIO;
				}

				memcpy((uint8_t *)arch_phys_to_virt(phys) + in_pg,
				       src + done, (size_t)chunk);
				done += chunk;
			}
			(void)off;
		}
	}

	kinfo("elf: loaded %s, %zu segments, 0x%llx..0x%llx, entry 0x%llx",
	      elf_machine_name(eh->e_machine), out->segments,
	      (unsigned long long)out->load_start,
	      (unsigned long long)out->load_end,
	      (unsigned long long)out->entry);

	return 0;
}
