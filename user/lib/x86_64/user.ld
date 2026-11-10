/* SPDX-License-Identifier: MIT
 *
 * Link layout for a static HavenOS program.
 *
 * Two PT_LOAD segments with different permissions on separate pages, so
 * the kernel can map code read-execute and data read-write.  A linker's
 * default script is free to merge them into one RWX segment, which the
 * ELF loader would then have to either refuse or map writable AND
 * executable.
 */
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);   /* R-X */
	data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
	. = 0x400000;

	.text   : { *(.text .text.*) }     :text
	.rodata : { *(.rodata .rodata.*) } :text

	. = ALIGN(0x1000);

	.data   : { *(.data .data.*) }     :data
	.bss    : { *(.bss .bss.*) *(COMMON) } :data

	/DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
