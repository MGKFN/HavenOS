/* SPDX-License-Identifier: MIT
 *
 * linker.ld — HavenOS x86_64 kernel image layout.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS linked at `. = 1M` with four sections and one symbol,
 * _kernel_end.  Its own pmm.c records what that cost: the reserve below
 * the kernel was a hard-coded constant that was wrong twice (1 MiB, then
 * 4 MiB, against a 6.31 MiB image), each time leaving live kernel BSS
 * inside the allocator's free pool.  The fix in v0.7.18 was to derive the
 * boundary from _kernel_end instead of a constant.
 *
 * HavenOS takes that lesson further: the image exports both virtual and
 * physical bounds, and per-section bounds besides, so
 *   - pmm_init() reserves exactly the image, computed not guessed, and
 *   - vmm_protect_kernel() can make .text read-execute, .rodata
 *     read-only+NX and .data/.bss read-write+NX, which needs each
 *     section's real extent.
 *
 * LOAD vs LINK ADDRESS
 * The kernel is linked at 0xFFFFFFFF80200000 (the top 2 GiB, the standard
 * x86_64 kernel window that -mcmodel=kernel generates code for) and loaded
 * at physical 0x200000.  AT() on every section records the physical address
 * in the ELF program headers, which is what GRUB loads by.  2 MiB rather
 * than 1 MiB because that leaves the whole first 2 MiB page free for
 * firmware, the EGA/MMIO hole and SMP trampolines, and because it lets the
 * kernel window be described by 2 MiB pages from offset zero.
 */

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x200000;

ENTRY(_start)

/* Explicit program headers, so the loadable image carries real
   permissions instead of one RWX blob.  The kernel installs its own page
   protections in vmm_protect_kernel() regardless, but a segment layout
   that already says "this part is code, this part is constant data" means
   the ELF and the page tables agree, and `readelf -l` on a crash dump is
   worth reading. */
PHDRS
{
	boot   PT_LOAD FLAGS(5);   /* R-X: multiboot header + trampoline */
	text   PT_LOAD FLAGS(5);   /* R-X: kernel code */
	rodata PT_LOAD FLAGS(4);   /* R--: constants and registration tables */
	data   PT_LOAD FLAGS(6);   /* RW-: mutable data and bss */
}

SECTIONS
{
	. = KERNEL_VMA + KERNEL_LMA;

	__kernel_start = .;
	__kernel_phys_start = . - KERNEL_VMA;

	/* The Multiboot2 header must appear within the first 32 KiB of the
	   file, so it leads the image, and the 32-bit trampoline sits with
	   it to keep all pre-long-mode code in one place. */
	.boot ALIGN(4K) : AT(ADDR(.boot) - KERNEL_VMA)
	{
		KEEP(*(.multiboot))
		*(.boot.text)
		*(.boot.data)
	} :boot

	__text_start = .;
	.text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
	{
		*(.text.unlikely .text.unlikely.*)
		*(.text .text.* .gnu.linkonce.t.*)
	} :text
	. = ALIGN(4K);
	__text_end = .;

	__rodata_start = .;
	.rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
	{
		*(.rodata .rodata.* .gnu.linkonce.r.*)

		/* Driver registration table.  Each driver contributes one
		   struct driver pointer here through a HAVEN_DRIVER()
		   macro, so device_init() can walk the set of linked-in
		   drivers without a central list anyone has to remember to
		   edit.  HavenDOS called every driver's init function by
		   name from kernel_main(), which is why its boot order bugs
		   (net_init() after the POST screen that queried it) were
		   possible at all. */
		. = ALIGN(8);
		__haven_drivers_start = .;
		KEEP(*(.haven.drivers))
		__haven_drivers_end = .;

		/* In-kernel self tests, registered the same way. */
		. = ALIGN(8);
		__haven_tests_start = .;
		KEEP(*(.haven.tests))
		__haven_tests_end = .;

		/* Init calls, ordered by level. */
		. = ALIGN(8);
		__haven_initcalls_start = .;
		KEEP(*(.haven.initcall.0))
		KEEP(*(.haven.initcall.1))
		KEEP(*(.haven.initcall.2))
		KEEP(*(.haven.initcall.3))
		__haven_initcalls_end = .;
	} :rodata
	. = ALIGN(4K);
	__rodata_end = .;

	__data_start = .;
	.data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
	{
		*(.data .data.* .gnu.linkonce.d.*)
	} :data

	/* Per-CPU data template.  Copied once per CPU by smp_init(); the
	   BSP uses the template in place. */
	. = ALIGN(4K);
	__percpu_start = .;
	.percpu : AT(ADDR(.percpu) - KERNEL_VMA)
	{
		*(.percpu .percpu.*)
	} :data
	. = ALIGN(4K);
	__percpu_end = .;
	__data_end = .;

	__bss_start = .;
	.bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
	{
		*(COMMON)
		*(.bss .bss.*)
	} :data
	. = ALIGN(4K);
	__bss_end = .;

	__kernel_end = .;
	__kernel_phys_end = . - KERNEL_VMA;

	/* Debug info is kept in the ELF for GDB but never loaded, and the
	   comment/note sections are dropped so they cannot land in the
	   middle of a loadable segment. */
	/DISCARD/ :
	{
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame.*)
		*(.gcc_except_table*)
	}
}
