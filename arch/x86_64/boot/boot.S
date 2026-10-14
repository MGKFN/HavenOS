/* SPDX-License-Identifier: MIT
 *
 * boot.S — HavenOS x86_64 boot entry.
 *
 * WHY MULTIBOOT2 AND NOT A BESPOKE UEFI LOADER
 * HavenDOS reached UEFI hardware by packaging BIOS and x86-64 UEFI GRUB
 * images in one ISO and having GRUB load a 32-bit Multiboot kernel.  That
 * approach was the project's single biggest milestone and it works on real
 * firmware, so HavenOS keeps the proven half — GRUB owning both firmware
 * paths — and replaces the part that was holding it back: the kernel
 * itself is now 64-bit.
 *
 * Writing our own UEFI loader would mean reimplementing firmware
 * enumeration, disk access and Secure Boot signing before the kernel could
 * print a character.  Multiboot2 gets a uniform, already-debugged handover
 * from both BIOS and UEFI, and leaves the UEFI-specific work (which
 * HavenDOS documented as unfinished) to GRUB.  See
 * docs/DESIGN_DECISIONS.md#boot.
 *
 * WHAT THIS FILE DOES
 * GRUB hands control to us in 32-bit protected mode with paging off, so
 * reaching the 64-bit higher-half kernel is this file's whole job:
 *
 *   1. save the Multiboot2 magic and info pointer before touching registers
 *   2. prove the CPU has long mode, and fail legibly if it does not
 *   3. build boot page tables: 4 GiB identity, 4 GiB direct map, and the
 *      kernel's higher-half window
 *   4. enable PAE + long mode + paging, load a 64-bit GDT
 *   5. far-jump to 64-bit code, then absolute-jump into the higher half
 *   6. call x86_boot_main(magic, info)
 *
 * Everything after step 6 is C.
 */

/* ── Layout constants.  Must match arch/x86_64/boot/linker.ld. ───────── */
.set KERNEL_VMA,        0xFFFFFFFF80000000
.set HHDM_PML4_INDEX,   256        /* 0xFFFF800000000000 */
.set KERNEL_PML4_INDEX, 511        /* 0xFFFFFFFF80000000 */
.set KERNEL_PDPT_INDEX, 510

/* Physical address of a higher-half symbol, usable from 32-bit code. */
#define PHYS(sym) ((sym) - KERNEL_VMA)

/* ── Multiboot2 ──────────────────────────────────────────────────────── */
.set MB2_MAGIC,         0xE85250D6
.set MB2_ARCH_I386,     0
.set MB2_TAG_ENTRY,     3
.set MB2_TAG_MODALIGN,  6
.set MB2_TAG_END,       0

/* ── Paging / control-register bits ──────────────────────────────────── */
.set PTE_PRESENT,       (1 << 0)
.set PTE_WRITE,         (1 << 1)
.set PTE_HUGE,          (1 << 7)   /* PS: 2 MiB page at PD level */
.set CR4_PAE,           (1 << 5)
.set CR4_PGE,           (1 << 7)
.set CR0_PE,            (1 << 0)
.set CR0_WP,            (1 << 16)
.set CR0_PG,            (1 << 31)
.set MSR_EFER,          0xC0000080
.set EFER_LME,          (1 << 8)
.set EFER_NXE,          (1 << 11)
.set CPUID_EXT_LM,      (1 << 29)

.set BOOT_STACK_SIZE,   0x10000    /* 64 KiB; the scheduler gives each
                                      thread its own stack later */

/* ════════════════════════════════════════════════════════════════════════
   Multiboot2 header.
   The entry-address tag is what makes a 64-bit ELF loadable by a 32-bit
   loader: without it GRUB would jump to e_entry, which is a higher-half
   address it cannot reach with paging off.
   ════════════════════════════════════════════════════════════════════ */
.section .multiboot, "a", @progbits
.align 8
mb2_header:
	.long   MB2_MAGIC
	.long   MB2_ARCH_I386
	.long   mb2_header_end - mb2_header
	.long   -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header))

	/* Entry address tag: enter at the *physical* address of _start. */
	.align 8
	.word   MB2_TAG_ENTRY
	.word   0                       /* flags: required */
	.long   12
	.long   PHYS(_start)

	/* Page-align any loader modules, so an initrd can be mapped
	   directly instead of being copied. */
	.align 8
	.word   MB2_TAG_MODALIGN
	.word   0
	.long   8

	.align 8
	.word   MB2_TAG_END
	.word   0
	.long   8
mb2_header_end:

/* ════════════════════════════════════════════════════════════════════════
   32-bit entry.
   ════════════════════════════════════════════════════════════════════ */
.section .boot.text, "ax", @progbits
.code32
.global _start
.type _start, @function
_start:
	cli
	cld

	/* GRUB's state is only guaranteed in EAX/EBX, so bank them before
	   anything else can clobber them.  HavenDOS learned this the hard
	   way and did the same thing first in its own boot.S. */
	movl    %eax, PHYS(boot_magic)
	movl    %ebx, PHYS(boot_mbinfo)

	movl    $PHYS(boot_stack_top), %esp
	xorl    %ebp, %ebp

	/* ── Step 2: does this CPU have long mode? ─────────────────────── */

	/* CPUID itself is not guaranteed on pre-Pentium parts; detect it by
	   trying to toggle EFLAGS.ID. */
	pushfl
	popl    %eax
	movl    %eax, %ecx
	xorl    $(1 << 21), %eax
	pushl   %eax
	popfl
	pushfl
	popl    %eax
	xorl    %ecx, %eax
	testl   $(1 << 21), %eax
	jz      .Lno_long_mode
	pushl   %ecx                    /* restore the caller's EFLAGS */
	popfl

	/* Extended leaves present? */
	movl    $0x80000000, %eax
	cpuid
	cmpl    $0x80000001, %eax
	jb      .Lno_long_mode

	movl    $0x80000001, %eax
	cpuid
	testl   $CPUID_EXT_LM, %edx
	jz      .Lno_long_mode

	/* ── Step 3: build the boot page tables ───────────────────────────
	   Three windows, all with 2 MiB pages:

	     0x0000000000000000 +4 GiB  identity — we are executing here
	     0xFFFF800000000000 +4 GiB  direct map (HHDM), phys_to_virt()
	     0xFFFFFFFF80000000 +1 GiB  kernel image window

	   The identity window exists only so the instruction after the
	   far jump is fetchable; vmm_init() tears it down once the real
	   tables are built.  The HHDM is extended to cover all of RAM by
	   vmm_init() as well — 4 GiB is enough to get the PMM running.

	   All three share pd_tables, so the kernel window and the identity
	   window describe the same physical memory with no duplication. */

	/* Zero pml4 + pdpt_low + pdpt_high + 4 page directories.
	   Multiboot2 loaders must zero .bss, but a boot that depends on
	   that and gets it wrong fails in a way that is very hard to read,
	   so spend the 7 K writes and be certain. */
	movl    $PHYS(boot_pml4), %edi
	xorl    %eax, %eax
	movl    $((4096 * 3 + 4096 * 4) / 4), %ecx
	rep stosl

	/* Page directories: 2048 entries × 2 MiB = 4 GiB, identity. */
	movl    $PHYS(boot_pd), %edi
	movl    $(PTE_PRESENT | PTE_WRITE | PTE_HUGE), %eax
	xorl    %edx, %edx
	movl    $2048, %ecx
.Lfill_pd:
	movl    %eax, (%edi)
	movl    %edx, 4(%edi)
	addl    $0x200000, %eax
	adcl    $0, %edx                /* carry into the high dword */
	addl    $8, %edi
	loop    .Lfill_pd

	/* pdpt_low[0..3] → the four page directories. */
	movl    $PHYS(boot_pdpt_low), %edi
	movl    $PHYS(boot_pd), %eax
	orl     $(PTE_PRESENT | PTE_WRITE), %eax
	movl    $4, %ecx
.Lfill_pdpt_low:
	movl    %eax, (%edi)
	movl    $0, 4(%edi)
	addl    $4096, %eax
	addl    $8, %edi
	loop    .Lfill_pdpt_low

	/* pdpt_high[510] → the first page directory, so the kernel window
	   at 0xFFFFFFFF80000000 covers physical 0..1 GiB. */
	movl    $PHYS(boot_pdpt_high), %edi
	movl    $PHYS(boot_pd), %eax
	orl     $(PTE_PRESENT | PTE_WRITE), %eax
	movl    %eax, (KERNEL_PDPT_INDEX * 8)(%edi)
	movl    $0,   (KERNEL_PDPT_INDEX * 8 + 4)(%edi)

	/* pml4[0] = pml4[256] = pdpt_low;  pml4[511] = pdpt_high. */
	movl    $PHYS(boot_pml4), %edi
	movl    $PHYS(boot_pdpt_low), %eax
	orl     $(PTE_PRESENT | PTE_WRITE), %eax
	movl    %eax, (%edi)
	movl    $0,   4(%edi)
	movl    %eax, (HHDM_PML4_INDEX * 8)(%edi)
	movl    $0,   (HHDM_PML4_INDEX * 8 + 4)(%edi)
	movl    $PHYS(boot_pdpt_high), %eax
	orl     $(PTE_PRESENT | PTE_WRITE), %eax
	movl    %eax, (KERNEL_PML4_INDEX * 8)(%edi)
	movl    $0,   (KERNEL_PML4_INDEX * 8 + 4)(%edi)

	/* ── Step 4: PAE → CR3 → LME/NXE → paging ───────────────────────
	   The order is mandatory: LME may only be set while paging is off,
	   and CR3 must already point at a valid PML4 when CR0.PG goes on. */
	movl    %cr4, %eax
	orl     $(CR4_PAE | CR4_PGE), %eax
	movl    %eax, %cr4

	movl    $PHYS(boot_pml4), %eax
	movl    %eax, %cr3

	movl    $MSR_EFER, %ecx
	rdmsr
	orl     $(EFER_LME | EFER_NXE), %eax
	wrmsr

	movl    %cr0, %eax
	orl     $(CR0_PG | CR0_WP | CR0_PE), %eax
	movl    %eax, %cr0

	/* ── Step 5: into 64-bit code ──────────────────────────────────── */
	lgdt    PHYS(boot_gdt_ptr)
	ljmpl   $0x08, $PHYS(.Llong_mode)

/* ── Long-mode unreachable: say so and stop. ─────────────────────────
   This is the one error HavenOS can hit before it owns a console, and a
   silent black screen is the worst possible diagnostic, so write straight
   to EGA text memory and to COM1 (which a BIOS has already left usable at
   its default divisor on most machines).  A user who sees this line knows
   the machine is 32-bit only and that HavenDOS, not HavenOS, is what runs
   on it. */
.Lno_long_mode:
	movl    $PHYS(msg_no_lm), %esi
	movl    $0xB8000, %edi
.Lega_loop:
	movb    (%esi), %al
	testb   %al, %al
	jz      .Lega_done
	movb    %al, (%edi)
	movb    $0x4F, 1(%edi)          /* white on red */
	incl    %esi
	addl    $2, %edi
	jmp     .Lega_loop
.Lega_done:
	movl    $PHYS(msg_no_lm), %esi
.Lser_loop:
	movb    (%esi), %al
	testb   %al, %al
	jz      .Lhalt32
	movw    $0x3F8, %dx
	outb    %al, %dx
	incl    %esi
	jmp     .Lser_loop
.Lhalt32:
	cli
	hlt
	jmp     .Lhalt32

/* ════════════════════════════════════════════════════════════════════════
   64-bit code.  Still running identity-mapped at this point.
   ════════════════════════════════════════════════════════════════════ */
.code64
.Llong_mode:
	/* In long mode the data selectors are ignored for addressing, but
	   they must still be a valid writable descriptor for pushes and for
	   the IRET path to behave. */
	movw    $0x10, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	/* Step 5b: leave the identity window.  The jump target is a 64-bit
	   absolute, which is exactly why it needs movabs and an indirect
	   jump — a rel32 jmp cannot span the 2 GiB from here to there. */
	movabsq $.Lhigher_half, %rax
	jmpq    *%rax

.Lhigher_half:
	/* From here on every address is a kernel virtual address. */
	movabsq $boot_stack_top, %rsp
	xorq    %rbp, %rbp
	pushq   $0
	popfq                           /* known-clean RFLAGS, interrupts off */

	/* Step 6: x86_boot_main(uint32_t magic, uint32_t mbinfo_phys) */
	movl    boot_magic(%rip),  %edi
	movl    boot_mbinfo(%rip), %esi
	callq   x86_boot_main

	/* x86_boot_main() is noreturn; if it ever does return, stop here
	   rather than executing whatever follows in memory. */
.Lhalt64:
	cli
	hlt
	jmp     .Lhalt64
.size _start, . - _start

/* ════════════════════════════════════════════════════════════════════════
   Boot data.
   ════════════════════════════════════════════════════════════════════ */
.section .boot.data, "aw", @progbits
.align 8

/* Minimal 64-bit GDT.  arch/x86_64/cpu/gdt.c replaces this with the real
   one (including TSS and user segments) as soon as C code runs; this exists
   only to make the far jump legal. */
boot_gdt:
	.quad   0x0000000000000000      /* 0x00 null */
	.quad   0x00AF9A000000FFFF      /* 0x08 code64: L=1, DPL=0, exec/read */
	.quad   0x00CF92000000FFFF      /* 0x10 data:   DPL=0, read/write */
boot_gdt_end:

boot_gdt_ptr:
	.word   boot_gdt_end - boot_gdt - 1
	.long   PHYS(boot_gdt)          /* 32-bit base: lgdt runs in .code32 */

msg_no_lm:
	.asciz  "HavenOS: this CPU does not support x86_64 long mode. HavenOS requires a 64-bit processor.\r\n"

/* Multiboot2 handover, captured in 32-bit mode and read in 64-bit mode. */
.global boot_magic
.global boot_mbinfo
boot_magic:     .long 0
boot_mbinfo:    .long 0

/* ════════════════════════════════════════════════════════════════════════
   Boot page tables and stack.

   These live in .bss so they cost nothing in the ISO, and they are
   page-aligned because a page-table pointer has no low bits to spare.
   boot_pd is four consecutive directories addressed as one 2048-entry
   array by the fill loop above.
   ════════════════════════════════════════════════════════════════════ */
.section .bss, "aw", @nobits
.align 4096
.global boot_pml4
boot_pml4:              .skip 4096
boot_pdpt_low:          .skip 4096
boot_pdpt_high:         .skip 4096
boot_pd:                .skip 4096 * 4

.align 16
.global boot_stack_bottom
.global boot_stack_top
boot_stack_bottom:      .skip BOOT_STACK_SIZE
boot_stack_top:

/* Mark the stack non-executable for the toolchain's benefit. */
.section .note.GNU-stack, "", @progbits
