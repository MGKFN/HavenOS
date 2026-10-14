/* SPDX-License-Identifier: MIT
 *
 * trampoline.S — bringing an application processor from reset to C.
 *
 * WHY THIS IS SO AWKWARD
 * A processor started by a startup IPI begins executing in 16-bit REAL
 * MODE at physical address (vector << 12), with no paging, no GDT it can
 * use, a 64 KiB addressable segment and nothing set up.  It has to climb
 * the same ladder the boot CPU climbed in boot/boot.S -- real mode, then
 * protected mode, then long mode -- except that it must do it from a page
 * in the first megabyte, because the startup vector is a single byte.
 *
 * HavenDOS never needed any of this: it was single-processor by
 * construction, and the "Multi-core processor support" line in the
 * HavenOS goals is precisely the part that requires this file.
 *
 * POSITION INDEPENDENCE WITHOUT -fPIC
 * This code is assembled as part of the kernel, linked at a higher-half
 * address, and then COPIED to a low physical page at run time.  Every
 * address it references must therefore be computed from the page it is
 * actually running at, not from where the linker put it.  The technique
 * is the same throughout: take a label's offset within the trampoline
 * (label - tramp_start) and add TRAMPOLINE_BASE, the physical address the
 * BSP copies it to.  Getting that wrong gives a processor that jumps into
 * the middle of the BIOS.
 *
 * THE HANDOFF BLOCK
 * The BSP cannot pass arguments to a processor coming out of reset, so it
 * writes them into a fixed block at the end of this page: the page-table
 * root, the stack this CPU should use, the 64-bit entry point, and a flag
 * the AP sets to say it arrived.  The BSP polls that flag, which is how
 * it knows whether a processor started rather than assuming it did.
 */

#include <haven/arch/x86_64/gdt.h>

/* Must match TRAMPOLINE_PHYS in arch/x86_64/smp/smp.c. */
.set TRAMPOLINE_BASE, 0x8000

/* Offset of a label from the start of the page, as a physical address. */
#define TPHYS(label) (TRAMPOLINE_BASE + ((label) - tramp_start))

.set CR0_PE,  (1 << 0)
.set CR0_PG,  (1 << 31)
.set CR4_PAE, (1 << 5)
.set MSR_EFER, 0xC0000080
.set EFER_LME, (1 << 8)
.set EFER_NXE, (1 << 11)

.section .rodata
.align 4096
.global tramp_start
.global tramp_end
.global tramp_size

/* ─────────────────────────── 16-bit real mode ────────────────────────── */
.code16
tramp_start:
	cli
	cld

	/* Zero the segment registers.  A processor out of reset has CS set
	   to the startup vector's segment and the rest undefined, so any
	   data access without this reads from an arbitrary segment. */
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	/* Load the 32-bit GDT.  lgdtl needs a linear address, which in real
	   mode with DS=0 is just the offset -- hence the physical
	   computation. */
	lgdtl   TPHYS(tramp_gdt32_ptr)

	/* Into protected mode.  The far jump that follows is what actually
	   loads CS with a protected-mode selector; setting CR0.PE alone
	   leaves CS holding a real-mode segment. */
	movl    %cr0, %eax
	orl     $CR0_PE, %eax
	movl    %eax, %cr0

	/* A far jump encoded by hand: the assembler cannot produce a
	   32-bit far jump from 16-bit code with an absolute target it
	   computes at link time. */
	.byte   0x66, 0xEA           /* ljmpl */
	.long   TPHYS(tramp_prot32)  /* offset */
	.word   0x08                 /* selector: 32-bit code */

/* ──────────────────────── 32-bit protected mode ──────────────────────── */
.code32
tramp_prot32:
	movw    $0x10, %ax           /* 32-bit data selector */
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	/* PAE must be on before long mode, and the page tables the BSP
	   built are already complete -- an AP shares the kernel address
	   space rather than building its own, which is why it only needs
	   to load CR3. */
	movl    %cr4, %eax
	orl     $CR4_PAE, %eax
	movl    %eax, %cr4

	movl    TPHYS(tramp_cr3), %eax
	movl    %eax, %cr3

	movl    $MSR_EFER, %ecx
	rdmsr
	orl     $(EFER_LME | EFER_NXE), %eax
	wrmsr

	movl    %cr0, %eax
	orl     $CR0_PG, %eax
	movl    %eax, %cr0

	/* The 64-bit GDT, then a far jump to load a 64-bit CS. */
	lgdtl   TPHYS(tramp_gdt64_ptr)
	ljmpl   $0x08, $TPHYS(tramp_long64)

/* ───────────────────────────── 64-bit mode ───────────────────────────── */
.code64
tramp_long64:
	movw    $0x10, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	/* Take the stack the BSP allocated for this CPU.  Until this point
	   the AP has had no stack at all, which is why everything above is
	   written without a single push or call. */
	movq    TPHYS(tramp_stack), %rsp
	xorq    %rbp, %rbp
	pushq   $0
	popfq

	/* Tell the BSP we arrived, BEFORE calling into C.  The BSP is
	   polling this flag with a timeout; setting it after the C entry
	   point ran would make a CPU that starts successfully but faults
	   in C look like one that never started. */
	movl    $1, TPHYS(tramp_started)

	/* Into the higher half and into C.  The entry point and the CPU
	   index both come from the handoff block, because a register set
	   up before the INIT IPI would not have survived it. */
	movl    TPHYS(tramp_cpu_id), %edi
	movq    TPHYS(tramp_entry), %rax
	jmpq    *%rax

	/* The entry point never returns.  If it does, stop here rather
	   than executing whatever the next page holds. */
1:
	cli
	hlt
	jmp     1b

/* ──────────────────────────── Descriptors ────────────────────────────── */
.align 16
tramp_gdt32:
	.quad   0x0000000000000000   /* null */
	.quad   0x00CF9A000000FFFF   /* 0x08 code32 */
	.quad   0x00CF92000000FFFF   /* 0x10 data */
tramp_gdt32_end:

tramp_gdt32_ptr:
	.word   tramp_gdt32_end - tramp_gdt32 - 1
	.long   TPHYS(tramp_gdt32)

.align 16
tramp_gdt64:
	.quad   0x0000000000000000   /* null */
	.quad   0x00AF9A000000FFFF   /* 0x08 code64, L=1 */
	.quad   0x00CF92000000FFFF   /* 0x10 data */
tramp_gdt64_end:

tramp_gdt64_ptr:
	.word   tramp_gdt64_end - tramp_gdt64 - 1
	.long   TPHYS(tramp_gdt64)

/* ───────────────────────── Handoff block ─────────────────────────────
   The BSP writes every field here before sending the startup IPI, and
   polls tramp_started afterwards.  The offsets are exported as symbols so
   the C side computes them rather than hard-coding numbers -- the same
   reasoning as percpu_asm.h.
   ────────────────────────────────────────────────────────────────────── */
.align 16
.global tramp_cr3
.global tramp_stack
.global tramp_entry
.global tramp_started
.global tramp_cpu_id
tramp_cr3:      .quad 0
tramp_stack:    .quad 0
tramp_entry:    .quad 0
tramp_started:  .long 0
tramp_cpu_id:   .long 0

tramp_end:
tramp_size = tramp_end - tramp_start

.section .note.GNU-stack, "", @progbits
