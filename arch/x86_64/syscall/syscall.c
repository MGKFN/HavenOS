/* SPDX-License-Identifier: MIT
 *
 * syscall.c — the x86_64 SYSCALL/SYSRET entry configuration.
 *
 * WHY SYSCALL AND NOT A SOFTWARE INTERRUPT
 * `int 0x80` goes through the IDT, which means a privilege-level change
 * driven by a descriptor lookup, a stack switch from the TSS, and a full
 * interrupt frame -- on the order of a hundred cycles before any kernel
 * code runs.  SYSCALL loads CS, SS and RIP from MSRs with no memory
 * access at all, which is several times faster, and system-call latency
 * is on the path of every I/O operation a program performs.
 *
 * The cost is that SYSCALL does almost nothing for you: it does not
 * switch stacks, does not disable interrupts and does not save anything
 * but RIP and RFLAGS. The entry path in cpu/entry.S has to do all of it
 * by hand, in the right order; the comment there explains why the first
 * three instructions are the most safety-critical in the kernel.
 */
#include <haven/arch.h>
#include <haven/syscall.h>
#include <haven/console.h>
#include <haven/sched.h>
#include <haven/smp.h>
#include <haven/arch/x86_64/io.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/gdt.h>
#include <haven/arch/x86_64/idt.h>
#include <haven/arch/x86_64/regs.h>

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_CSTAR  0xC0000083
#define MSR_SFMASK 0xC0000084

#define EFER_SCE   (1ULL << 0)   /* SYSCALL enable */

extern void syscall_entry(void);

/*
 * The architecture half of the dispatcher.  Unpacks the register layout
 * into struct syscall_args and hands over to the portable code.
 */
void syscall_dispatch(struct trapframe *tf);

void syscall_dispatch(struct trapframe *tf)
{
	struct syscall_args a = {
		.nr = tf->rax,
		.arg = {
			tf->rdi,
			tf->rsi,
			tf->rdx,
			/* r10, not rcx: the SYSCALL instruction overwrites rcx
			   with the return address, so the fourth argument has
			   to travel somewhere else.  This is why the ABI
			   comment in syscall.h names r10 explicitly. */
			tf->r10,
			tf->r8,
			tf->r9,
		},
	};

	int64_t rc = syscall_invoke(&a);

	/* The result goes back in rax, overwriting the call number. */
	tf->rax = (uint64_t)rc;

	/* A syscall is a safe place to honour a preemption the timer asked
	   for: we are in thread context on a kernel stack, with the user
	   frame fully described in the trap frame.  Doing it here rather
	   than only in the idle loop is what keeps a compute-bound process
	   from monopolising its CPU between interrupts. */
	sched_preempt_if_needed();
}

void arch_syscall_init(void)
{
	/* STAR holds the selectors.  SYSCALL takes CS from bits 47:32 and
	   SS from that value plus 8; SYSRET takes CS from bits 63:48 plus
	   16 and SS from that plus 8.  Those fixed offsets are why the GDT
	   is laid out kernel-code, kernel-data, user-data, user-code -- see
	   the comment at the top of cpu/gdt.c. */
	uint64_t star = ((uint64_t)GDT_SEL_KERNEL_CODE << 32) |
	                ((uint64_t)(GDT_SEL_USER_CODE - 16) << 48);
	wrmsr(MSR_STAR, star);

	/* LSTAR is the 64-bit entry point. */
	wrmsr(MSR_LSTAR, (uint64_t)(vaddr_t)syscall_entry);

	/* CSTAR is the compatibility-mode (32-bit) entry.  HavenOS does not
	   support 32-bit processes, so point it at the same handler: a
	   32-bit program that tries will get a clean kernel entry and an
	   -ENOSYS rather than an undefined jump. */
	wrmsr(MSR_CSTAR, (uint64_t)(vaddr_t)syscall_entry);

	/* SFMASK: RFLAGS bits cleared on entry.  IF must be here -- SYSCALL
	   does not disable interrupts by itself, and the first three
	   instructions of the entry path run on the USER stack with the
	   per-CPU pointer half-installed.  An interrupt there would be
	   catastrophic and extremely hard to reproduce.
	   TF and DF are cleared too: TF so a user-set single-step flag does
	   not single-step the kernel, DF because the kernel's string
	   operations assume forward. */
	wrmsr(MSR_SFMASK, (1ULL << 9)    /* IF */
	                | (1ULL << 8)    /* TF */
	                | (1ULL << 10)   /* DF */
	                | (1ULL << 18)   /* AC: keep SMAP enforcement on */
	                | (1ULL << 12) | (1ULL << 13));  /* IOPL */

	/* Finally enable the instruction.  Doing this last means no CPU can
	   take a SYSCALL before LSTAR points somewhere valid. */
	uint64_t efer = rdmsr(MSR_EFER);
	wrmsr(MSR_EFER, efer | EFER_SCE);
}
