/* SPDX-License-Identifier: MIT
 *
 * entry.S — interrupt entry, exception entry, and context switching.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS generated its stubs with assembler macros inside an
 * __asm__() block in idt.c, which worked but put 60 lines of assembly
 * inside a C file where no assembler syntax highlighting or debugger
 * line-number information could reach it.  More importantly it only
 * covered vectors 0-15 and IRQs 0-15: vectors 16-31 (including #XF, the
 * SIMD exception, and #MC) had no handler at all, so taking one meant a
 * triple fault with no diagnostic.
 *
 * HavenOS generates all 256 and routes them through one frame layout.
 *
 * THE THREE HARD PARTS, STATED PLAINLY
 *
 * 1. SWAPGS.  The per-CPU block is reached through GS.  While user code
 *    runs, GS belongs to user space and the kernel's value is parked in
 *    MSR_KERNEL_GS_BASE.  Every entry from ring 3 must SWAPGS on the way
 *    in and again on the way out; every entry from ring 0 must not.
 *    Swapping when we should not makes the kernel read user-controlled
 *    memory as its per-CPU block.  Not swapping when we should leaks a
 *    kernel pointer into a register user code can read.  The test is the
 *    low two bits of the pushed CS, and it is written once, here.
 *
 * 2. Error codes.  Some vectors push one, most do not.  Rather than two
 *    frame shapes, the stubs for vectors without an error code push a
 *    zero in its place, so the frame is uniform and the C handler needs
 *    no special cases.
 *
 * 3. The red zone.  The System V ABI lets a leaf function use 128 bytes
 *    below RSP without reserving them.  An interrupt that pushed a frame
 *    there would corrupt it.  The kernel is compiled -mno-red-zone for
 *    exactly this reason, and these stubs therefore need no extra
 *    adjustment -- but if that flag were ever dropped, this is the file
 *    that would break, silently and intermittently.
 */

#include <haven/arch/x86_64/gdt.h>
#include <haven/arch/x86_64/percpu_asm.h>

.section .text

/* ─── Save and restore the general-purpose register block ─────────────
   Push order is the reverse of struct trapframe's field order, because
   the stack grows down.  The _Static_assert in regs.h guards the size;
   the order has to be kept in agreement by reading both. */
.macro PUSH_REGS
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
.endm

.macro POP_REGS
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rbp
	popq %rdi
	popq %rsi
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax
.endm

/* ─── Stub generation ─────────────────────────────────────────────────
   Vectors 8, 10, 11, 12, 13, 14, 17, 21, 29 and 30 push an error code;
   every other vector does not.  The ERR form therefore pushes only the
   vector number; the NOERR form pushes a zero first so both frames end
   up identical. */

.macro ISR_NOERR vec
	.align 16
	.global isr_stub_\vec
isr_stub_\vec:
	pushq $0              /* placeholder error code */
	pushq $\vec
	jmp   isr_common
.endm

.macro ISR_ERR vec
	.align 16
	.global isr_stub_\vec
isr_stub_\vec:
	/* The CPU has already pushed the error code. */
	pushq $\vec
	jmp   isr_common
.endm

/* Vectors 0-31: CPU exceptions. */
ISR_NOERR 0    /* #DE divide error            */
ISR_NOERR 1    /* #DB debug                   */
ISR_NOERR 2    /* NMI                         */
ISR_NOERR 3    /* #BP breakpoint              */
ISR_NOERR 4    /* #OF overflow                */
ISR_NOERR 5    /* #BR bound range exceeded    */
ISR_NOERR 6    /* #UD invalid opcode          */
ISR_NOERR 7    /* #NM device not available    */
ISR_ERR   8    /* #DF double fault            */
ISR_NOERR 9    /* coprocessor segment overrun */
ISR_ERR   10   /* #TS invalid TSS             */
ISR_ERR   11   /* #NP segment not present     */
ISR_ERR   12   /* #SS stack fault             */
ISR_ERR   13   /* #GP general protection      */
ISR_ERR   14   /* #PF page fault              */
ISR_NOERR 15   /* reserved                    */
ISR_NOERR 16   /* #MF x87 FPU error           */
ISR_ERR   17   /* #AC alignment check         */
ISR_NOERR 18   /* #MC machine check           */
ISR_NOERR 19   /* #XM SIMD FP exception       */
ISR_NOERR 20   /* #VE virtualisation          */
ISR_ERR   21   /* #CP control protection      */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29   /* #VC VMM communication       */
ISR_ERR   30   /* #SX security exception      */
ISR_NOERR 31

/* Vectors 32-255: hardware IRQs, IPIs and software interrupts.
   Generated with .altmacro so a counter can be pasted into the label. */
.altmacro
.set vec_n, 32
.rept 224
	ISR_NOERR %vec_n
	.set vec_n, vec_n + 1
.endr
.noaltmacro

/* A table of all 256 stub addresses, so idt_init() can install them with
   a loop instead of 256 extern declarations and 256 assignments -- which
   is how HavenDOS's idt.c ended up covering only the first 16 vectors and
   the first 16 IRQs, leaving #MC, #XM and #AC with no handler at all. */
.section .rodata
	.align 8
	.global isr_stub_table
isr_stub_table:
/* `%expr` substitution only happens in a macro ARGUMENT, so the .quad has
   to live inside a macro of its own rather than directly in the .rept. */
.altmacro
.macro STUB_ENTRY n
	.quad isr_stub_\n
.endm
.set tbl_n, 0
.rept 256
	STUB_ENTRY %tbl_n
	.set tbl_n, tbl_n + 1
.endr
.noaltmacro
	.global isr_stub_table_end
isr_stub_table_end:

.section .text

/* ─── The common path ────────────────────────────────────────────────── */
	.align 16
isr_common:
	PUSH_REGS

	/* Decide whether GS must be swapped, from the privilege level the
	   interrupted code was running at.  The pushed CS sits above the
	   register block: 15 registers, then vector, then error code, then
	   RIP, then CS. */
	testl $3, 17*8(%rsp)
	jz    1f
	swapgs
1:
	/* The ABI wants the stack 16-byte aligned at a call.  Entry pushed
	   22 eight-byte words onto a 16-byte-aligned frame, which leaves
	   RSP ≡ 0 (mod 16) already — but clear the direction flag, since
	   the ABI permits a caller to leave it set and our string
	   operations assume forward. */
	cld

	movq %rsp, %rdi        /* struct trapframe * */
	call isr_dispatch

	/* Swap back on the same condition.  Re-reading the pushed CS rather
	   than remembering the earlier test is deliberate: a handler is
	   allowed to change the return frame (that is how a signal or a
	   fork return is delivered), and the swap must follow where we are
	   actually returning to. */
	testl $3, 17*8(%rsp)
	jz    2f
	swapgs
2:
	POP_REGS
	addq $16, %rsp         /* discard vector and error code */
	iretq

/* ─── Context switch ─────────────────────────────────────────────────
   void arch_context_switch(struct arch_context *from, struct arch_context *to)

   Only the callee-saved registers need saving: by the C ABI, the caller
   of arch_context_switch() has already preserved anything else it cared
   about.  That is what makes a context switch cheap -- seven pushes, a
   stack-pointer swap and seven pops.

   The switch works by leaving the outgoing thread's state on its own
   stack and recording only the stack pointer.  When that thread is next
   scheduled, the pops below restore it and `ret` returns into the middle
   of whatever called arch_context_switch() -- which is why the function
   appears to return twice, once per thread.
   ────────────────────────────────────────────────────────────────────── */
	.align 16
	.global arch_context_switch
	.type arch_context_switch, @function
arch_context_switch:
	pushfq
	pushq %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15

	movq %rsp, 0(%rdi)     /* from->rsp = current stack pointer */
	movq 0(%rsi), %rsp     /* switch to the incoming stack */

	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	popfq
	ret
	.size arch_context_switch, . - arch_context_switch

/*
 * void arch_context_start(struct arch_context *to)
 *
 * First entry into a context that has never run.  There is no outgoing
 * state to save, so this is the second half of arch_context_switch()
 * alone.  Never returns.
 */
	.align 16
	.global arch_context_start
	.type arch_context_start, @function
arch_context_start:
	movq 0(%rdi), %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	popfq
	ret
	.size arch_context_start, . - arch_context_start

/*
 * Trampoline for a brand-new kernel thread.
 *
 * arch_context_init_kernel() fabricates a stack whose `ret` lands here,
 * with the entry point in R12 and its argument in R13 -- both
 * callee-saved, so the pops in arch_context_switch() deliver them
 * untouched.  Going through a trampoline rather than returning straight
 * to the entry point means a thread that returns normally still has
 * somewhere to go: thread_exit(), rather than whatever happened to be on
 * the stack.
 */
	.align 16
	.global thread_trampoline
	.type thread_trampoline, @function
thread_trampoline:
	movq %r13, %rdi        /* arg */
	callq *%r12            /* entry(arg) */

	/* The thread's function returned.  That is a normal way for a
	   thread to finish, so exit cleanly with its return value rather
	   than falling through into unrelated code. */
	movl %eax, %edi
	call thread_exit

	/* thread_exit() does not return.  If it ever does, stop here
	   instead of executing whatever follows in memory. */
3:
	cli
	hlt
	jmp 3b
	.size thread_trampoline, . - thread_trampoline

/* ─── SYSCALL entry ──────────────────────────────────────────────────
   SYSCALL is not an interrupt: the CPU does not switch stacks, does not
   push a frame, and does not disable interrupts.  It puts the return
   address in RCX and the caller's RFLAGS in R11, loads CS and SS from
   MSR_STAR, and jumps to MSR_LSTAR -- still on the USER stack, with
   interrupts enabled.

   That makes the first three instructions the most safety-critical in the
   kernel:

     swapgs          we are in ring 0 but GS still points at user memory
     save user RSP   because the next instruction destroys it
     load kernel RSP from the per-CPU block, which is why swapgs is first

   Everything after that is ordinary.
   ────────────────────────────────────────────────────────────────────── */
	.align 16
	.global syscall_entry
	.type syscall_entry, @function
syscall_entry:
	swapgs

	/* Park the user stack pointer in the per-CPU block and take the
	   kernel stack.  %gs:8 is percpu_anchor.block; the two offsets
	   below are PERCPU_OFF_* from percpu_asm.h, checked against the C
	   struct by _Static_assert. */
	movq %gs:PERCPU_ANCHOR_BLOCK, %rax
	movq %rsp, PERCPU_OFF_USER_RSP(%rax)
	movq PERCPU_OFF_KSTACK(%rax), %rsp

	/* Build a trap frame by hand, in the same layout the interrupt
	   stubs produce, so syscalls and interrupts share every piece of
	   code above this file. */
	pushq $GDT_SEL_USER_DATA              /* ss     */
	pushq PERCPU_OFF_USER_RSP(%rax)       /* rsp    */
	pushq %r11                            /* rflags, saved by SYSCALL */
	pushq $GDT_SEL_USER_CODE              /* cs     */
	pushq %rcx                            /* rip,    saved by SYSCALL */
	pushq $0                              /* error code */
	pushq $0x80                           /* vector: a syscall marker */
	PUSH_REGS

	cld
	movq %rsp, %rdi
	call syscall_dispatch

	/* The dispatcher may have changed the frame (a thread that was
	   preempted and rescheduled, or a signal delivery), so reload
	   everything from it rather than assuming registers survived. */
	POP_REGS
	addq $16, %rsp                        /* vector, error code */

	popq %rcx                             /* rip    -> RCX for SYSRET */
	addq $8,  %rsp                        /* cs: SYSRET derives it */
	popq %r11                             /* rflags -> R11 for SYSRET */
	popq %rsp                             /* back to the user stack */
	/* ss is left on the (now abandoned) kernel stack; SYSRET derives it */

	swapgs
	sysretq
	.size syscall_entry, . - syscall_entry

.section .note.GNU-stack, "", @progbits

/* ─── First entry into user mode ──────────────────────────────────────
   arch_context_init_user() fabricates a stack whose `ret` lands here,
   with a complete iretq frame immediately above.  Entering ring 3
   requires iretq rather than ret, because only iretq can change the
   privilege level, the stack and the flags together.

   The swapgs is on the same principle as everywhere else in this file:
   we are about to run user code, so GS must stop pointing at the
   kernel's per-CPU block before it does.
   ────────────────────────────────────────────────────────────────────── */
	.section .text
	.align 16
	.global user_return_trampoline
	.type user_return_trampoline, @function
user_return_trampoline:
	/* Zero every register the new process has no business inheriting.
	   A fresh process must not start life holding kernel pointers left
	   over in registers from whatever ran before it. */
	xorq %rax, %rax
	xorq %rbx, %rbx
	xorq %rcx, %rcx
	xorq %rdx, %rdx
	xorq %rsi, %rsi
	xorq %rdi, %rdi
	xorq %rbp, %rbp
	xorq %r8,  %r8
	xorq %r9,  %r9
	xorq %r10, %r10
	xorq %r11, %r11
	xorq %r12, %r12
	xorq %r13, %r13
	xorq %r14, %r14
	xorq %r15, %r15

	swapgs
	iretq
	.size user_return_trampoline, . - user_return_trampoline
