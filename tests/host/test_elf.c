/* SPDX-License-Identifier: MIT
 *
 * test_elf.c — attack the real kernel/proc/elf.c with malformed and
 * hostile executables.
 *
 * This is the most security-sensitive test in the suite.  An ELF file is
 * entirely attacker-controlled, and the loader is asked to map parts of
 * it into memory at addresses the file itself specifies.  Every check in
 * elf.c exists because of a specific way a crafted file could damage the
 * kernel, and each of those is tested here by crafting exactly that file.
 *
 * HavenDOS had no program loader and therefore none of this attack
 * surface -- but it also had no way to run a program that was not
 * compiled into the kernel.  This is the cost of gaining that, paid
 * deliberately.
 */
#include "harness.h"

#include <haven/elf.h>
#include <haven/errno.h>
#include <haven/arch.h>

/* A buffer big enough for a header, a few program headers and a small
   segment payload. */
#define IMG_SIZE 8192
static uint8_t img[IMG_SIZE];

#define USER_OK_ADDR 0x400000ULL     /* a plausible user load address */
#define KERNEL_ADDR  0xFFFFFFFF80200000ULL

/* Build a minimal but valid ELF64 executable with `nph` PT_LOAD
   segments.  Returns the total image length. */
static size_t build_elf(uint16_t nph, uint16_t machine)
{
	memset(img, 0, sizeof(img));

	struct elf64_header *eh = (struct elf64_header *)img;
	eh->e_ident[0] = ELF_MAGIC0;
	eh->e_ident[1] = ELF_MAGIC1;
	eh->e_ident[2] = ELF_MAGIC2;
	eh->e_ident[3] = ELF_MAGIC3;
	eh->e_ident[4] = ELFCLASS64;
	eh->e_ident[5] = ELFDATA2LSB;
	eh->e_ident[6] = EV_CURRENT;
	eh->e_type      = ET_EXEC;
	eh->e_machine   = machine;
	eh->e_version   = EV_CURRENT;
	eh->e_entry     = USER_OK_ADDR;
	eh->e_phoff     = sizeof(struct elf64_header);
	eh->e_ehsize    = sizeof(struct elf64_header);
	eh->e_phentsize = sizeof(struct elf64_phdr);
	eh->e_phnum     = nph;

	struct elf64_phdr *ph = (struct elf64_phdr *)(img + eh->e_phoff);
	size_t payload_off = eh->e_phoff +
	                     (size_t)nph * sizeof(struct elf64_phdr);
	payload_off = (payload_off + 4095) & ~(size_t)4095;

	for (uint16_t i = 0; i < nph; i++) {
		ph[i].p_type   = PT_LOAD;
		ph[i].p_flags  = PF_R | PF_X;
		ph[i].p_offset = payload_off;
		ph[i].p_vaddr  = USER_OK_ADDR + (uint64_t)i * 0x10000;
		ph[i].p_filesz = 256;
		ph[i].p_memsz  = 4096;
		ph[i].p_align  = 4096;
	}

	/* Recognisable payload, so a successful load can be verified byte
	   for byte rather than just reported as succeeding. */
	for (size_t i = 0; i < 256; i++)
		img[payload_off + i] = (uint8_t)(i ^ 0x5A);

	return payload_off + 256;
}

static struct elf64_header *hdr(void) { return (struct elf64_header *)img; }
static struct elf64_phdr *phdr(int i)
{
	return (struct elf64_phdr *)(img + hdr()->e_phoff) + i;
}

static void test_valid(void)
{
	TEST_GROUP("a well-formed executable is accepted");

	size_t len = build_elf(1, EM_X86_64);

	CHECK_EQ("validation passes", elf_validate(img, len, EM_X86_64), 0);

	haven_test_make_machine(64ULL << 20);
	haven_test_reset_pmm();

	struct arch_pagetable *pt = arch_pt_create();
	struct elf_image info;
	int rc = elf_load(pt, img, len, &info);

	CHECK_EQ("loading succeeds", rc, 0);
	CHECK_EQ("one segment was loaded", info.segments, 1);
	CHECK_EQ("the entry point is reported", info.entry, USER_OK_ADDR);
	CHECK_EQ("the load range starts at the segment", info.load_start,
	         USER_OK_ADDR);

	/* The payload must actually be in memory, at the right address, in
	   the right order -- not merely "a load that returned 0". */
	paddr_t pa = arch_pt_translate(pt, USER_OK_ADDR);
	CHECK_NE("the entry page is mapped", pa, 0);
	if (pa) {
		const uint8_t *mem = (const uint8_t *)arch_phys_to_virt(pa);
		bool match = true;
		for (size_t i = 0; i < 256; i++) {
			if (mem[i] != (uint8_t)(i ^ 0x5A)) {
				match = false;
				break;
			}
		}
		CHECK("the segment's file contents were copied correctly",
		      match);

		/* Past p_filesz but within p_memsz must be zero: that is the
		   .bss region, and leaving the previous owner's data there
		   would leak it into the new process. */
		bool bss_zero = true;
		for (size_t i = 256; i < 4096; i++) {
			if (mem[i] != 0) {
				bss_zero = false;
				break;
			}
		}
		CHECK("the memsz-beyond-filesz region is zeroed, so no stale "
		      "data leaks into the process", bss_zero);
	}

	arch_pt_destroy(pt);
	haven_test_free_machine();
}

static void test_header_rejections(void)
{
	TEST_GROUP("malformed headers are rejected");

	size_t len;

	len = build_elf(1, EM_X86_64);
	img[0] = 0;
	CHECK_EQ("no ELF magic", elf_validate(img, len, EM_X86_64),
	         -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_ident[4] = 1;   /* ELFCLASS32 */
	CHECK_EQ("a 32-bit ELF is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_ident[5] = 2;   /* big endian */
	CHECK_EQ("a big-endian ELF is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_version = 7;
	CHECK_EQ("an unknown ELF version is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_type = ET_REL;
	CHECK_EQ("a relocatable object cannot be executed",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	/* An AArch64 binary would load and then execute garbage. */
	len = build_elf(1, EM_AARCH64);
	CHECK_EQ("a binary for another architecture is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_phentsize = 40;
	CHECK_EQ("a wrong program-header entry size is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_phnum = 0;
	CHECK_EQ("no program headers means nothing to load",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_phnum = 60000;
	CHECK_EQ("an implausible program-header count is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	/* A truncated file: the header is valid but the file is shorter
	   than it claims. */
	len = build_elf(1, EM_X86_64);
	CHECK_EQ("a file shorter than an ELF header is rejected",
	         elf_validate(img, 32, EM_X86_64), -HAVEN_ENOEXEC);

	CHECK_EQ("a NULL image is rejected",
	         elf_validate(NULL, 100, EM_X86_64), -HAVEN_EINVAL);
}

static void test_phdr_table_out_of_bounds(void)
{
	TEST_GROUP("the program-header table must lie inside the file");

	size_t len = build_elf(1, EM_X86_64);

	/* A valid e_phoff with a large e_phnum walks off the end of the
	   buffer -- reading kernel heap memory and interpreting it as
	   program headers. */
	hdr()->e_phnum = 100;
	CHECK_EQ("a header count that runs past the file end is rejected",
	         elf_validate(img, 256, EM_X86_64), -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_phoff = len + 1000;
	CHECK_EQ("a header offset past the file end is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_phoff = UINT64_MAX - 16;
	CHECK_EQ("a header offset that would overflow when added to the "
	         "table size is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);
}

static void test_entry_point(void)
{
	TEST_GROUP("the entry point must be in user space and mapped");

	size_t len = build_elf(1, EM_X86_64);
	hdr()->e_entry = KERNEL_ADDR;
	CHECK_EQ("an entry point at a kernel address is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	len = build_elf(1, EM_X86_64);
	hdr()->e_entry = 0;
	CHECK_EQ("a null entry point is rejected",
	         elf_validate(img, len, EM_X86_64), -HAVEN_ENOEXEC);

	/* An entry point in user space but outside every loaded segment
	   would transfer control to unmapped memory. */
	haven_test_make_machine(64ULL << 20);
	haven_test_reset_pmm();

	len = build_elf(1, EM_X86_64);
	hdr()->e_entry = USER_OK_ADDR + 0x500000;

	struct arch_pagetable *pt = arch_pt_create();
	struct elf_image info;
	CHECK_EQ("an entry point outside every segment is rejected at load",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);
	haven_test_free_machine();
}

static void test_segment_rejections(void)
{
	TEST_GROUP("hostile segments are rejected at load");

	haven_test_make_machine(64ULL << 20);

	struct elf_image info;
	size_t len;
	struct arch_pagetable *pt;

	/* THE attack: a segment at a kernel address.  If this were allowed,
	   loading the executable would overwrite the kernel that is loading
	   it.  This is the single most important check in elf.c. */
	haven_test_reset_pmm();
	len = build_elf(1, EM_X86_64);
	phdr(0)->p_vaddr = KERNEL_ADDR;
	hdr()->e_entry   = KERNEL_ADDR;
	pt = arch_pt_create();
	CHECK_EQ("a segment at a KERNEL address is rejected",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);

	/* A segment that starts in user space and runs past its top. */
	haven_test_reset_pmm();
	len = build_elf(1, EM_X86_64);
	phdr(0)->p_vaddr = 0x00007FFFFFFF0000ULL;
	phdr(0)->p_memsz = 0x100000;
	pt = arch_pt_create();
	CHECK_EQ("a segment that runs off the top of user space is rejected",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);

	/* memsz < filesz would make the copy write past the mapping. */
	haven_test_reset_pmm();
	len = build_elf(1, EM_X86_64);
	phdr(0)->p_filesz = 4096;
	phdr(0)->p_memsz  = 256;
	pt = arch_pt_create();
	CHECK_EQ("memsz smaller than filesz is rejected",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);

	/* A file offset past the end of the file. */
	haven_test_reset_pmm();
	len = build_elf(1, EM_X86_64);
	phdr(0)->p_offset = len + 4096;
	pt = arch_pt_create();
	CHECK_EQ("a segment reading past the end of the file is rejected",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);

	/* An offset + size that overflows: `off + sz > len` is FALSE for
	   values that wrap, which is why the check is written as
	   `sz > len - off`. */
	haven_test_reset_pmm();
	len = build_elf(1, EM_X86_64);
	phdr(0)->p_offset = UINT64_MAX - 100;
	phdr(0)->p_filesz = 200;
	pt = arch_pt_create();
	CHECK_EQ("a file extent that overflows on addition is rejected",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);

	/* A virtual extent that overflows. */
	haven_test_reset_pmm();
	len = build_elf(1, EM_X86_64);
	phdr(0)->p_vaddr = UINT64_MAX - 100;
	phdr(0)->p_memsz = 4096;
	pt = arch_pt_create();
	CHECK_EQ("a virtual extent that overflows on addition is rejected",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);

	/* A non-power-of-two alignment breaks the rounding arithmetic. */
	haven_test_reset_pmm();
	len = build_elf(1, EM_X86_64);
	phdr(0)->p_align = 3000;
	pt = arch_pt_create();
	CHECK_EQ("a non-power-of-two alignment is rejected",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);

	haven_test_free_machine();
}

static void test_overlapping_segments(void)
{
	TEST_GROUP("overlapping segments are rejected");

	haven_test_make_machine(64ULL << 20);
	haven_test_reset_pmm();

	size_t len = build_elf(2, EM_X86_64);

	/* Two segments on the same page.  The second would silently replace
	   the first's permissions -- which is how a read-only segment ends
	   up writable without anyone asking for it. */
	phdr(1)->p_vaddr = phdr(0)->p_vaddr + 512;
	phdr(1)->p_flags = PF_R | PF_W;

	struct arch_pagetable *pt = arch_pt_create();
	struct elf_image info;
	CHECK_EQ("two segments sharing a page are rejected",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);

	/* Adjacent but non-overlapping segments are fine. */
	haven_test_reset_pmm();
	len = build_elf(2, EM_X86_64);
	pt = arch_pt_create();
	CHECK_EQ("two segments on separate pages load fine",
	         elf_load(pt, img, len, &info), 0);
	CHECK_EQ("both were loaded", info.segments, 2);
	arch_pt_destroy(pt);

	haven_test_free_machine();
}

static void test_permissions_applied(void)
{
	TEST_GROUP("segment permissions reach the page tables");

	haven_test_make_machine(64ULL << 20);
	haven_test_reset_pmm();

	size_t len = build_elf(2, EM_X86_64);
	phdr(0)->p_flags = PF_R | PF_X;          /* text  */
	phdr(1)->p_flags = PF_R | PF_W;          /* data  */

	struct arch_pagetable *pt = arch_pt_create();
	struct elf_image info;
	CHECK_EQ("the image loads", elf_load(pt, img, len, &info), 0);

	uint32_t text_prot = haven_test_pt_prot(pt, phdr(0)->p_vaddr);
	uint32_t data_prot = haven_test_pt_prot(pt, phdr(1)->p_vaddr);

	CHECK("the text segment is executable",
	      (text_prot & VM_PROT_EXEC) != 0);
	CHECK("the text segment is NOT writable",
	      (text_prot & VM_PROT_WRITE) == 0);
	CHECK("the data segment is writable",
	      (data_prot & VM_PROT_WRITE) != 0);
	CHECK("the data segment is NOT executable",
	      (data_prot & VM_PROT_EXEC) == 0);
	CHECK("both are user-accessible",
	      (text_prot & VM_PROT_USER) && (data_prot & VM_PROT_USER));

	arch_pt_destroy(pt);
	haven_test_free_machine();
}

static void test_non_load_segments_ignored(void)
{
	TEST_GROUP("segment types other than PT_LOAD are skipped");

	haven_test_make_machine(64ULL << 20);
	haven_test_reset_pmm();

	size_t len = build_elf(3, EM_X86_64);
	phdr(1)->p_type = PT_GNU_STACK;
	/* A PT_GNU_STACK segment legitimately has a zero address and a
	   nonsensical size; loading it would fail the user-space check for
	   no reason. */
	phdr(1)->p_vaddr = 0;
	phdr(1)->p_memsz = 0;
	phdr(2)->p_type  = PT_NOTE;

	struct arch_pagetable *pt = arch_pt_create();
	struct elf_image info;
	CHECK_EQ("an image with PT_GNU_STACK and PT_NOTE loads",
	         elf_load(pt, img, len, &info), 0);
	CHECK_EQ("only the PT_LOAD segment was mapped", info.segments, 1);

	arch_pt_destroy(pt);

	/* An image with no PT_LOAD at all has nothing to run. */
	haven_test_reset_pmm();
	len = build_elf(1, EM_X86_64);
	phdr(0)->p_type = PT_NOTE;
	pt = arch_pt_create();
	CHECK_EQ("an image with no loadable segment is rejected",
	         elf_load(pt, img, len, &info), -HAVEN_ENOEXEC);
	arch_pt_destroy(pt);

	haven_test_free_machine();
}

int main(void)
{
	printf("HavenOS host tests: ELF64 loader\n");
	printf("================================\n");
	printf("linking the real kernel/proc/elf.c\n");

	haven_test_capture_log(true);

	test_valid();
	test_header_rejections();
	test_phdr_table_out_of_bounds();
	test_entry_point();
	test_segment_rejections();
	test_overlapping_segments();
	test_permissions_applied();
	test_non_load_segments_ignored();

	haven_test_capture_log(false);
	return haven_test_report("elf");
}
