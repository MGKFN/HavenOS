/* SPDX-License-Identifier: MIT
 *
 * gdt.c — the Global Descriptor Table and per-CPU Task State Segment.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's GDT was three entries: null, a ring-0 code segment and a
 * ring-0 data segment, both covering the whole 4 GiB address space,
 * read-write-executable.  There were no ring-3 descriptors because there
 * was no ring 3, and no TSS because nothing ever changed privilege level.
 *
 * HavenOS needs all of it:
 *
 *   - ring-3 code and data descriptors, so user processes can run;
 *   - a TSS per CPU holding the ring-0 stack pointer the CPU switches to
 *     on an interrupt taken in user mode -- without which a user-mode
 *     fault would try to push the trap frame onto the *user* stack, which
 *     the user controls;
 *   - an IST entry with its own stack for the double-fault handler,
 *     because the ordinary path would need a usable stack and a double
 *     fault is frequently caused by not having one.
 *
 * SEGMENT SELECTOR ORDER IS NOT ARBITRARY
 * The SYSCALL/SYSRET instructions do not read a selector operand: they
 * derive the target selectors arithmetically from MSR_STAR.  SYSCALL
 * loads CS from STAR[47:32] and SS from that plus 8; SYSRET loads CS from
 * STAR[63:48] plus 16 and SS from that plus 8.  So the table must be laid
 * out as kernel code, kernel data, user data, user code -- with user data
 * BEFORE user code, which looks wrong until you know why.  Getting the
 * order wrong produces a #GP on the first return to user mode with no
 * other symptom.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/string.h>
#include <haven/smp.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/gdt.h>

/* Descriptor access-byte and flag bits. */
#define DESC_ACCESSED   (1ULL << 40)
#define DESC_RW         (1ULL << 41)   /* readable (code) / writable (data) */
#define DESC_CONFORMING (1ULL << 42)
#define DESC_EXEC       (1ULL << 43)
#define DESC_CODEDATA   (1ULL << 44)   /* 1 = code or data, 0 = system */
#define DESC_DPL(x)     ((uint64_t)(x) << 45)
#define DESC_PRESENT    (1ULL << 47)
#define DESC_LONG       (1ULL << 53)   /* L: 64-bit code segment */
#define DESC_DB         (1ULL << 54)   /* D/B: 32-bit default operand */
#define DESC_GRAN       (1ULL << 55)   /* G: limit is in 4 KiB units */

#define TSS_TYPE_AVAIL64 (0x9ULL << 40)

struct HAVEN_PACKED tss {
	uint32_t reserved0;
	uint64_t rsp0;        /* stack the CPU loads on a ring 3 -> 0 trap */
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist[7];      /* Interrupt Stack Table */
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iomap_base;
};

HAVEN_STATIC_ASSERT(sizeof(struct tss) == 104, "x86_64 TSS must be 104 bytes");

struct HAVEN_PACKED gdt_ptr {
	uint16_t limit;
	uint64_t base;
};

/* Per CPU: 7 eight-byte slots.  The TSS descriptor is 16 bytes in long
   mode (it carries a 64-bit base), so it occupies slots 5 and 6. */
#define GDT_SLOTS 7

static uint64_t   gdt[HAVEN_MAX_CPUS][GDT_SLOTS] HAVEN_ALIGNED(16);
static struct tss tss_table[HAVEN_MAX_CPUS]      HAVEN_ALIGNED(16);

/* Dedicated stacks for the IST entries.  Static rather than allocated,
   because the double-fault handler must have a stack even when the
   allocator is what failed. */
#define IST_STACK_PAGES 2
static uint8_t ist_stacks[HAVEN_MAX_CPUS][IST_DEPTH]
                         [IST_STACK_PAGES * 4096] HAVEN_ALIGNED(16);

static uint64_t make_segment(bool code, int dpl)
{
	uint64_t d = DESC_PRESENT | DESC_CODEDATA | DESC_RW | DESC_DPL(dpl);

	if (code) {
		d |= DESC_EXEC | DESC_LONG;
	} else {
		/* A 64-bit data descriptor's base and limit are ignored, but
		   the D/B bit must be set for a stack segment to behave as
		   32-bit-default — which matters only for compatibility-mode
		   code.  Setting it is harmless and matches what every other
		   x86_64 kernel does. */
		d |= DESC_DB | DESC_GRAN;
	}
	return d;
}

static void install_tss_descriptor(uint32_t cpu)
{
	uint64_t base  = (uint64_t)(vaddr_t)&tss_table[cpu];
	uint32_t limit = sizeof(struct tss) - 1;

	/* Low half: limit[15:0], base[15:0], base[23:16], type, base[31:24].
	   High half: base[63:32], reserved. */
	uint64_t low = (uint64_t)(limit & 0xFFFF)
	             | ((base & 0xFFFFFF) << 16)
	             | TSS_TYPE_AVAIL64
	             | DESC_PRESENT
	             | ((uint64_t)((limit >> 16) & 0xF) << 48)
	             | (((base >> 24) & 0xFF) << 56);

	gdt[cpu][GDT_SLOT_TSS]     = low;
	gdt[cpu][GDT_SLOT_TSS + 1] = base >> 32;
}

void gdt_init_cpu(uint32_t cpu)
{
	HAVEN_ASSERT(cpu < HAVEN_MAX_CPUS);

	memset(gdt[cpu], 0, sizeof(gdt[cpu]));

	/* The order is dictated by SYSCALL/SYSRET; see the file comment. */
	gdt[cpu][GDT_SLOT_NULL]        = 0;
	gdt[cpu][GDT_SLOT_KERNEL_CODE] = make_segment(true,  0);
	gdt[cpu][GDT_SLOT_KERNEL_DATA] = make_segment(false, 0);
	gdt[cpu][GDT_SLOT_USER_DATA]   = make_segment(false, 3);
	gdt[cpu][GDT_SLOT_USER_CODE]   = make_segment(true,  3);

	memset(&tss_table[cpu], 0, sizeof(tss_table[cpu]));

	/* Each IST entry points at the TOP of its stack, because the stack
	   grows down.  Pointing at the base is a mistake that produces a
	   handler which immediately writes below its own allocation. */
	for (int i = 0; i < IST_DEPTH; i++) {
		tss_table[cpu].ist[i] =
			(uint64_t)(vaddr_t)&ist_stacks[cpu][i]
			[IST_STACK_PAGES * 4096];
	}

	/* An I/O map base beyond the TSS limit means "no I/O map", which
	   makes every I/O port access from ring 3 fault.  Leaving it at
	   zero would instead make the CPU read the I/O permission bitmap
	   from the start of the TSS and interpret unrelated fields as
	   permissions. */
	tss_table[cpu].iomap_base = sizeof(struct tss);

	install_tss_descriptor(cpu);

	struct gdt_ptr ptr = {
		.limit = sizeof(gdt[cpu]) - 1,
		.base  = (uint64_t)(vaddr_t)gdt[cpu],
	};

	__asm__ volatile("lgdt %0" :: "m"(ptr) : "memory");

	/* Reload the data selectors so they refer to the new table.  CS is
	   reloaded by a far return, since `mov` to CS is illegal. */
	__asm__ volatile(
		"movw %[data], %%ax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%ss\n\t"
		"movw %%ax, %%fs\n\t"
		/* GS is NOT reloaded: it carries the per-CPU base, set
		   through MSR_GS_BASE.  Writing the segment register here
		   would zero that base and break this_cpu(). */
		"pushq %[code]\n\t"
		"leaq  1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n"
		"1:\n\t"
		:: [data] "i"(GDT_SEL_KERNEL_DATA),
		   [code] "i"((uint64_t)GDT_SEL_KERNEL_CODE)
		: "rax", "memory");

	uint16_t tss_sel = GDT_SEL_TSS;
	__asm__ volatile("ltr %0" :: "m"(tss_sel) : "memory");
}

void gdt_init(void)
{
	gdt_init_cpu(0);
	kinfo("gdt: descriptor table and TSS installed for the boot CPU "
	      "(kernel + user segments, %d IST stacks)", IST_DEPTH);
}

void gdt_init_ap(uint32_t cpu)
{
	gdt_init_cpu(cpu);
}

void tss_init(uint32_t cpu, vaddr_t kstack_top)
{
	HAVEN_ASSERT(cpu < HAVEN_MAX_CPUS);
	tss_table[cpu].rsp0 = kstack_top;
}

void tss_set_kstack(uint32_t cpu, vaddr_t kstack_top)
{
	if (cpu >= HAVEN_MAX_CPUS)
		return;
	/* This runs on every context switch into a user thread: the CPU
	   must find the *incoming* thread's kernel stack here, not the
	   outgoing one's, or a fault in the new thread would push its trap
	   frame onto a stack another thread is using. */
	tss_table[cpu].rsp0 = kstack_top;
}

vaddr_t tss_get_kstack(uint32_t cpu)
{
	if (cpu >= HAVEN_MAX_CPUS)
		return 0;
	return tss_table[cpu].rsp0;
}
