/* SPDX-License-Identifier: MIT
 *
 * context.c — building a thread context that arch_context_switch() can
 * resume into.
 *
 * HOW A NEW THREAD STARTS RUNNING
 * arch_context_switch() (in entry.S) saves state by pushing seven words
 * onto the outgoing thread's stack and recording only the stack pointer;
 * it resumes a thread by loading that pointer, popping the seven words,
 * and executing `ret`.
 *
 * A thread that has never run has no such state to pop.  So this file
 * fabricates it: a stack whose top seven words are exactly what the pops
 * expect, with the `ret` target set to thread_trampoline.  The thread then
 * "resumes" into its own first instruction, and no special case is needed
 * anywhere in the scheduler for a thread's first switch.
 *
 * The stack, from low address to high, must be:
 *
 *      r15, r14, r13, r12, rbx, rbp, rflags, return address
 *
 * matching the pop order in arch_context_switch().  The entry point and
 * its argument travel in r12 and r13 because those are callee-saved and
 * therefore restored by the pops -- which is how the trampoline receives
 * them without needing anywhere else to put them.
 */
#include <haven/arch.h>
#include <haven/panic.h>
#include <haven/string.h>
#include <haven/arch/x86_64/gdt.h>
#include <haven/arch/x86_64/memlayout.h>

/* What arch_context_switch() reads and writes.  Only `rsp` is touched by
   the switch itself; the rest of the state lives on the thread's own
   stack, which is why this structure is so small. */
struct arch_context {
	uint64_t rsp;
};

/* RFLAGS for a new thread: bit 1 is reserved and must read 1, and IF is
   set so the thread is preemptible from its first instruction.  A thread
   that started with interrupts disabled could never be preempted until it
   enabled them, which would make a buggy thread unkillable. */
#define INITIAL_RFLAGS 0x202ULL

extern void thread_trampoline(void);

size_t arch_context_size(void)
{
	return sizeof(struct arch_context);
}

void arch_context_init_kernel(struct arch_context *ctx, vaddr_t stack_top,
                              void (*entry)(void *), void *arg)
{
	HAVEN_ASSERT(ctx != NULL);
	HAVEN_ASSERT(entry != NULL);

	/* The System V ABI requires RSP ≡ 8 (mod 16) at the entry of a
	   function reached by `call`, because the call pushed a return
	   address onto a 16-byte-aligned stack.  thread_trampoline is
	   reached by `ret`, which pops rather than pushes, so the stack
	   must be arranged so that the trampoline's own `call` lands
	   correctly aligned.  Getting this wrong produces a #GP on the
	   first use of an aligned SSE store deep inside unrelated code --
	   a famously confusing symptom. */
	vaddr_t top = ALIGN_DOWN(stack_top, 16);

	uint64_t *sp = (uint64_t *)top;

	/* Pushed in reverse, so the layout below ends up in pop order. */
	*--sp = (uint64_t)(vaddr_t)thread_trampoline;  /* ret target     */
	*--sp = INITIAL_RFLAGS;                        /* popfq          */
	*--sp = 0;                                     /* rbp            */
	*--sp = 0;                                     /* rbx            */
	*--sp = (uint64_t)(vaddr_t)entry;              /* r12 -> callee  */
	*--sp = (uint64_t)(vaddr_t)arg;                /* r13 -> argument */
	*--sp = 0;                                     /* r14            */
	*--sp = 0;                                     /* r15            */

	ctx->rsp = (uint64_t)(vaddr_t)sp;
}

/*
 * A context that enters user mode.
 *
 * Unlike a kernel thread, this cannot simply `ret` to the entry point:
 * returning to ring 3 requires `iretq` with a full frame describing the
 * target privilege level, stack and flags.  So the fabricated stack ends
 * with a trampoline that performs that `iretq`, and the frame it needs is
 * laid out beneath it.
 */
extern void user_return_trampoline(void);

void arch_context_init_user(struct arch_context *ctx, vaddr_t kstack_top,
                            vaddr_t entry, vaddr_t ustack)
{
	HAVEN_ASSERT(ctx != NULL);
	HAVEN_ASSERT(is_user_addr(entry));
	HAVEN_ASSERT(is_user_addr(ustack));

	vaddr_t   top = ALIGN_DOWN(kstack_top, 16);
	uint64_t *sp  = (uint64_t *)top;

	/* The iretq frame, highest address first since iretq pops upward. */
	*--sp = GDT_SEL_USER_DATA;   /* ss     */
	*--sp = ustack;              /* rsp    */
	*--sp = INITIAL_RFLAGS;      /* rflags */
	*--sp = GDT_SEL_USER_CODE;   /* cs     */
	*--sp = entry;               /* rip    */

	/* Then the switch frame, so arch_context_switch() resumes into the
	   trampoline that executes the iretq above. */
	*--sp = (uint64_t)(vaddr_t)user_return_trampoline;
	*--sp = INITIAL_RFLAGS;
	*--sp = 0;   /* rbp */
	*--sp = 0;   /* rbx */
	*--sp = 0;   /* r12 */
	*--sp = 0;   /* r13 */
	*--sp = 0;   /* r14 */
	*--sp = 0;   /* r15 */

	ctx->rsp = (uint64_t)(vaddr_t)sp;
}
