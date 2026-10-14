/* SPDX-License-Identifier: MIT
 *
 * regs.h — the x86_64 trap frame.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's regs_t was built to match what its `pusha` stub happened to
 * leave on the stack, including an `esp_dummy` field that existed only
 * because `pusha` pushes a stale ESP nobody can use.  The layout was a
 * consequence of an instruction rather than a decision, and the struct and
 * the assembly had to be kept in agreement by hand.
 *
 * x86_64 has no `pusha`, so the stub pushes each register explicitly.
 * That turns out to be an advantage: the frame layout is now a deliberate
 * choice, the struct below documents it, and _Static_assert checks the
 * size the assembly depends on.  The order is the reverse of the push
 * order, because the stack grows down.
 */
#ifndef _HAVEN_ARCH_X86_64_REGS_H
#define _HAVEN_ARCH_X86_64_REGS_H

#include <haven/types.h>

struct trapframe {
	/* Pushed by the stub, in reverse order, so r15 is at the lowest
	   address — which is where RSP points when the handler is called. */
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

	/* Pushed by the stub: which vector we are in, and the error code.
	   Vectors that the CPU does not give an error code for get a zero
	   pushed in its place, so every frame has the same shape and the
	   common handler needs no special cases.  HavenDOS did the same
	   thing and it was the right call. */
	uint64_t vector;
	uint64_t error_code;

	/* Pushed by the CPU.  rsp and ss are always present in long mode,
	   even for a same-privilege interrupt — unlike 32-bit protected
	   mode, where they appeared only on a privilege change.  That
	   removes a whole class of frame-layout confusion. */
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
};

HAVEN_STATIC_ASSERT(sizeof(struct trapframe) == 22 * 8,
                    "trapframe layout must match arch/x86_64/cpu/entry.S");

/* Did this trap come from user mode?  The low two bits of CS are the
   privilege level the interrupted code was running at.  This is the test
   that decides whether the entry path must SWAPGS, and getting it wrong
   either leaks a kernel pointer to user space or makes the kernel treat
   user-controlled memory as its per-CPU block. */
static inline bool trapframe_from_user(const struct trapframe *tf)
{
	return (tf->cs & 3) != 0;
}

/* x86 page-fault error-code bits, in CR2's companion word. */
#define PF_PRESENT   (1u << 0)   /* fault was a protection violation, not
                                    a missing page */
#define PF_WRITE     (1u << 1)   /* the access was a write */
#define PF_USER      (1u << 2)   /* the access came from ring 3 */
#define PF_RESERVED  (1u << 3)   /* a reserved page-table bit was set */
#define PF_FETCH     (1u << 4)   /* the access was an instruction fetch */
#define PF_PROTKEY   (1u << 5)
#define PF_SHADOW    (1u << 6)
#define PF_SGX       (1u << 15)

#endif /* _HAVEN_ARCH_X86_64_REGS_H */
