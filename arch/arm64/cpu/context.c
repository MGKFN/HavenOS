/* SPDX-License-Identifier: MIT
 *
 * context.c — AArch64 context switching and the user boundary.
 *
 * STATUS: skeleton.  Every function here panics, and the comments say
 * what the real implementation has to do, because this is the part of an
 * AArch64 port most likely to be written from a misleading analogy with
 * x86_64.
 *
 * WHY IT IS NOT A DIRECT TRANSLATION
 *
 * Context switching.  x86_64's switcher leaves state on the outgoing
 * thread's stack and records only RSP, because the C ABI has already
 * preserved everything the caller cared about.  The same trick works on
 * AArch64, but the register set to save is different: x19-x28 are the
 * callee-saved general registers, x29 is the frame pointer, x30 is the
 * link register, and SP is a separate register per exception level
 * (SP_EL0 and SP_EL1).  d8-d15 are callee-saved too, so a kernel that
 * ever uses floating point must save them -- HavenOS compiles with
 * -mgeneral-regs-only, so it does not, and that is worth stating since
 * the omission would otherwise look like a bug.
 *
 * Entering user mode.  x86_64 needs iretq with a hand-built frame.
 * AArch64 uses `eret`, which takes its target from ELR_EL1 and its
 * processor state from SPSR_EL1 -- two system registers rather than five
 * stack words.  Simpler, but the state to put in SPSR_EL1 (EL0t, with
 * the right DAIF mask) is easy to get subtly wrong in a way that gives
 * user code interrupts masked, which hangs it.
 *
 * The user/kernel copy.  x86_64 needs an exception table because a fault
 * inside the copy loop must be recoverable.  AArch64 needs the same, but
 * it also has a facility x86_64 lacks: LDTR and STTR, "unprivileged
 * load/store", which perform the access with EL0 permissions from EL1.
 * That makes the SMAP equivalent (PAN, Privileged Access Never) usable
 * without a STAC/CLAC window around every copy, which is both faster and
 * safer.  A port that used ordinary loads with PAN disabled would work
 * and would throw away the protection.
 */
#include <haven/arch.h>
#include <haven/panic.h>
#include <haven/errno.h>
#include <haven/arch/arm64/memlayout.h>

#define NOT_YET(what) \
	panic("arm64: " what " is not implemented; see docs/PORTING_ARM64.md")

/* The opaque context.  Sized for x19-x28, x29, x30 and SP: twelve
   64-bit words, versus x86_64's one.  Declared now so that
   arch_context_size() can be right even before the switcher exists,
   since kernel/sched/sched.c places the context inside each thread's
   stack allocation using that size. */
struct arch_context {
	uint64_t sp;
	uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
	uint64_t fp;    /* x29 */
	uint64_t lr;    /* x30 */
};

size_t arch_context_size(void)
{
	return sizeof(struct arch_context);
}

void arch_context_init_kernel(struct arch_context *ctx, vaddr_t stack_top,
                              void (*entry)(void *), void *arg)
{
	(void)ctx; (void)stack_top; (void)entry; (void)arg;
	NOT_YET("kernel thread context creation");
}

void arch_context_init_user(struct arch_context *ctx, vaddr_t kstack_top,
                            vaddr_t entry, vaddr_t ustack)
{
	(void)ctx; (void)kstack_top; (void)entry; (void)ustack;
	NOT_YET("user thread context creation (ELR_EL1/SPSR_EL1)");
}

void arch_context_switch(struct arch_context *from, struct arch_context *to)
{
	(void)from; (void)to;
	NOT_YET("the context switcher");
}

void arch_context_start(struct arch_context *to)
{
	(void)to;
	NOT_YET("first entry into a context");
}

void arch_enter_user(vaddr_t entry, vaddr_t stack)
{
	(void)entry; (void)stack;
	NOT_YET("entering EL0 (eret with ELR_EL1/SPSR_EL1)");
}

/* ── Per-CPU anchor ──────────────────────────────────────────────────
   TPIDR_EL1 is the direct equivalent of the x86_64 GS base: a register
   the kernel owns, holding a pointer to this CPU's block.  It needs no
   SWAPGS counterpart, because EL0 has its own TPIDR_EL0 -- which removes
   a whole class of bug that arch/x86_64/cpu/entry.S has to be careful
   about on every entry and exit. */

void arch_set_current_thread(void *thread)
{
	(void)thread;
	NOT_YET("the per-CPU area (TPIDR_EL1)");
}

void *arch_get_current_thread(void)
{
	NOT_YET("the per-CPU area (TPIDR_EL1)");
}

void arch_set_kernel_stack(vaddr_t stack_top)
{
	(void)stack_top;
	/* AArch64 needs no TSS equivalent: SP_EL1 is a separate register
	   from SP_EL0, so the kernel stack is already in place when an
	   exception from EL0 is taken.  Recording it is still useful for
	   the entry path, but there is no hardware structure to update. */
	NOT_YET("the per-CPU kernel stack record");
}

/* ── User/kernel copies ──────────────────────────────────────────────── */

int arch_copy_from_user(void *dst, const void *user_src, size_t len)
{
	(void)dst; (void)user_src; (void)len;
	NOT_YET("copy_from_user (LDTR with PAN and a fixup table)");
}

int arch_copy_to_user(void *user_dst, const void *src, size_t len)
{
	(void)user_dst; (void)src; (void)len;
	NOT_YET("copy_to_user (STTR with PAN and a fixup table)");
}

int arch_strncpy_from_user(char *dst, const char *user_src, size_t len)
{
	(void)dst; (void)user_src; (void)len;
	NOT_YET("strncpy_from_user");
}
