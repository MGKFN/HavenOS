/* SPDX-License-Identifier: MIT
 *
 * exceptions.c — CPU exception handling and the central trap dispatcher.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's isr_handler() was a good panic screen: it cleared the display
 * to red, named the exception, and dumped every register.  HavenOS keeps
 * that — the information is exactly right — and changes two things.
 *
 * First, the report goes to the kernel log, so it reaches the serial
 * console as well as the display.  HavenDOS's version wrote directly to
 * video memory, so a fault on a machine whose display was not working
 * produced nothing at all.
 *
 * Second, an exception is no longer unconditionally fatal.  A page fault
 * in user space is a normal event: it is how demand paging, stack growth
 * and copy-on-write are implemented.  An invalid opcode in user space
 * kills a process, not the machine.  Only a fault that the kernel itself
 * took, or one that cannot be attributed, panics.  HavenDOS had no user
 * mode, so "exception means panic" was correct there and is not here.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/irq.h>
#include <haven/fault.h>
#include <haven/smp.h>
#include <haven/mm.h>
#include <haven/printf.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/idt.h>
#include <haven/arch/x86_64/io.h>
#include <haven/arch/x86_64/memlayout.h>
#include <haven/arch/x86_64/regs.h>

static const char *const exc_name[32] = {
	[0]  = "divide error (#DE)",
	[1]  = "debug (#DB)",
	[2]  = "non-maskable interrupt (NMI)",
	[3]  = "breakpoint (#BP)",
	[4]  = "overflow (#OF)",
	[5]  = "bound range exceeded (#BR)",
	[6]  = "invalid opcode (#UD)",
	[7]  = "device not available (#NM)",
	[8]  = "double fault (#DF)",
	[9]  = "coprocessor segment overrun",
	[10] = "invalid TSS (#TS)",
	[11] = "segment not present (#NP)",
	[12] = "stack-segment fault (#SS)",
	[13] = "general protection fault (#GP)",
	[14] = "page fault (#PF)",
	[15] = "reserved",
	[16] = "x87 FPU error (#MF)",
	[17] = "alignment check (#AC)",
	[18] = "machine check (#MC)",
	[19] = "SIMD floating-point exception (#XM)",
	[20] = "virtualisation exception (#VE)",
	[21] = "control protection exception (#CP)",
	[29] = "VMM communication exception (#VC)",
	[30] = "security exception (#SX)",
};

static inline uint64_t read_cr2(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr2, %0" : "=r"(v));
	return v;
}

static inline uint64_t read_cr3(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr3, %0" : "=r"(v));
	return v;
}

/* Describe a page-fault error code in words.  A reader should not have to
   decode a bitmask to learn that the kernel tried to execute a data page. */
static void describe_page_fault(uint64_t err, uint64_t addr, char *out,
                                size_t outsz)
{
	const char *what = (err & PF_FETCH) ? "instruction fetch"
	                 : (err & PF_WRITE) ? "write"
	                                    : "read";
	const char *why  = (err & PF_PRESENT)
	                 ? "protection violation on a present page"
	                 : "page not present";
	const char *who  = (err & PF_USER) ? "user mode" : "kernel mode";

	const char *hint = "";
	if (addr < HAVEN_PAGE_SIZE)
		hint = "  [likely a NULL pointer dereference]";
	else if (!is_canonical(addr))
		hint = "  [non-canonical address: uninitialised or corrupted "
		       "pointer]";
	else if (addr >= HHDM_BASE && !(err & PF_USER))
		hint = "  [kernel address: a kernel pointer bug, or memory "
		       "that was freed]";
	else if (addr < USER_MAX && !(err & PF_USER))
		hint = "  [kernel touched a user address directly: use "
		       "copy_from_user()]";

	snprintf(out, outsz, "%s from %s, %s%s", what, who, why, hint);
}

static void dump_frame(const struct trapframe *tf)
{
	uint64_t vec = tf->vector;
	const char *name = (vec < 32 && exc_name[vec]) ? exc_name[vec]
	                                               : "unknown exception";

	kputs_raw("\n");
	kputs_raw("================================================================================\n");

	char line[256];
	snprintf(line, sizeof(line),
	         "  CPU EXCEPTION %llu on cpu %u: %s\n",
	         (unsigned long long)vec, arch_cpu_id(), name);
	kputs_raw(line);
	kputs_raw("================================================================================\n");

	if (vec == VEC_PAGE_FAULT) {
		uint64_t cr2 = read_cr2();
		char why[192];
		describe_page_fault(tf->error_code, cr2, why, sizeof(why));
		snprintf(line, sizeof(line),
		         "  faulting address : 0x%016llx\n  cause            : %s\n",
		         (unsigned long long)cr2, why);
		kputs_raw(line);
	}

	snprintf(line, sizeof(line),
	         "  error code       : 0x%llx\n", (unsigned long long)tf->error_code);
	kputs_raw(line);

	snprintf(line, sizeof(line),
	         "  rip  0x%016llx   cs  0x%04llx  (ring %llu)\n",
	         (unsigned long long)tf->rip, (unsigned long long)tf->cs,
	         (unsigned long long)(tf->cs & 3));
	kputs_raw(line);
	snprintf(line, sizeof(line),
	         "  rsp  0x%016llx   ss  0x%04llx   rflags 0x%08llx\n",
	         (unsigned long long)tf->rsp, (unsigned long long)tf->ss,
	         (unsigned long long)tf->rflags);
	kputs_raw(line);
	snprintf(line, sizeof(line),
	         "  rax  0x%016llx   rbx 0x%016llx\n",
	         (unsigned long long)tf->rax, (unsigned long long)tf->rbx);
	kputs_raw(line);
	snprintf(line, sizeof(line),
	         "  rcx  0x%016llx   rdx 0x%016llx\n",
	         (unsigned long long)tf->rcx, (unsigned long long)tf->rdx);
	kputs_raw(line);
	snprintf(line, sizeof(line),
	         "  rsi  0x%016llx   rdi 0x%016llx\n",
	         (unsigned long long)tf->rsi, (unsigned long long)tf->rdi);
	kputs_raw(line);
	snprintf(line, sizeof(line),
	         "  rbp  0x%016llx   r8  0x%016llx\n",
	         (unsigned long long)tf->rbp, (unsigned long long)tf->r8);
	kputs_raw(line);
	snprintf(line, sizeof(line),
	         "  r9   0x%016llx   r10 0x%016llx\n",
	         (unsigned long long)tf->r9, (unsigned long long)tf->r10);
	kputs_raw(line);
	snprintf(line, sizeof(line),
	         "  r11  0x%016llx   r12 0x%016llx\n",
	         (unsigned long long)tf->r11, (unsigned long long)tf->r12);
	kputs_raw(line);
	snprintf(line, sizeof(line),
	         "  r13  0x%016llx   r14 0x%016llx\n",
	         (unsigned long long)tf->r13, (unsigned long long)tf->r14);
	kputs_raw(line);
	snprintf(line, sizeof(line),
	         "  r15  0x%016llx   cr3 0x%016llx\n",
	         (unsigned long long)tf->r15, (unsigned long long)read_cr3());
	kputs_raw(line);

	kputs_raw("--------------------------------------------------------------------------------\n");
}

/* Weak defaults for the two portable fault hooks, so this subsystem can
   be brought up and tested before the VM fault handler and the process
   layer exist.  A real implementation anywhere in the tree overrides
   these with no edit here. */
HAVEN_WEAK bool vm_handle_fault(vaddr_t addr, uint32_t flags)
{
	(void)addr;
	(void)flags;
	return false;
}

HAVEN_WEAK bool proc_fault_kill(const char *reason, int signal)
{
	(void)reason;
	(void)signal;
	return false;
}

/* Translate the x86 page-fault error code into the portable flags.  This
   is the only place the two vocabularies meet. */
static uint32_t pf_flags(uint64_t err)
{
	uint32_t f = 0;
	if (err & PF_WRITE)   f |= VM_FAULT_WRITE;
	if (err & PF_FETCH)   f |= VM_FAULT_EXEC;
	if (err & PF_USER)    f |= VM_FAULT_USER;
	if (err & PF_PRESENT) f |= VM_FAULT_PRESENT;
	return f;
}

static void fatal_exception(struct trapframe *tf)
{
	dump_frame(tf);
	backtrace(24);
	panic("unhandled CPU exception %llu (%s) at rip 0x%llx",
	      (unsigned long long)tf->vector,
	      (tf->vector < 32 && exc_name[tf->vector])
	              ? exc_name[tf->vector] : "unknown",
	      (unsigned long long)tf->rip);
}

static void handle_page_fault(struct trapframe *tf)
{
	uint64_t addr = read_cr2();
	uint64_t err  = tf->error_code;

	/* A user-mode fault may be entirely normal: demand paging, stack
	   growth, copy-on-write.  Give the VM layer first refusal. */
	if (trapframe_from_user(tf)) {
		if (vm_handle_fault(addr, pf_flags(err)))
			return;

		/* Not something the VM layer recognises, so the process has
		   genuinely faulted.  Kill it, not the machine. */
		dump_frame(tf);
		if (proc_fault_kill("segmentation fault", 11))
			return;

		/* No process layer yet: fall through to the panic, because a
		   user-mode fault with nothing able to handle it means the
		   kernel has lost track of what is running. */
	}

	/* A kernel-mode fault.  One case is legitimate and must be handled
	   before the panic: a fault inside copy_from_user()/copy_to_user(),
	   which is how a bad user pointer is turned into -EFAULT rather
	   than a dead kernel. */
	if (uaccess_fixup(tf))
		return;

	fatal_exception(tf);
}

static void handle_general_protection(struct trapframe *tf)
{
	if (trapframe_from_user(tf)) {
		dump_frame(tf);
		if (proc_fault_kill("general protection fault", 11))
			return;
	}
	if (uaccess_fixup(tf))
		return;
	fatal_exception(tf);
}

static void handle_invalid_opcode(struct trapframe *tf)
{
	if (trapframe_from_user(tf)) {
		dump_frame(tf);
		if (proc_fault_kill("illegal instruction", 4))
			return;
	}
	fatal_exception(tf);
}

static void handle_machine_check(struct trapframe *tf)
{
	/* A machine check means the hardware has detected an uncorrectable
	   error.  There is nothing to recover: the only useful action is to
	   get the report out before the machine goes further wrong. */
	dump_frame(tf);
	panic("machine check: the hardware reported an uncorrectable error. "
	      "This is a hardware fault, not a HavenOS bug.");
}

static void handle_nmi(struct trapframe *tf)
{
	/* An NMI on a PC is usually a hardware error signalled through the
	   chipset, or a watchdog.  Report it and continue: an NMI that is
	   only informational must not take the system down. */
	uint8_t sys_ctrl = inb(0x61);
	kerr("nmi: non-maskable interrupt at rip 0x%llx, port 0x61 = 0x%02x "
	     "(%s%s)",
	     (unsigned long long)tf->rip, sys_ctrl,
	     (sys_ctrl & 0x80) ? "parity error " : "",
	     (sys_ctrl & 0x40) ? "I/O channel check" : "");
}

/*
 * The single entry point from every one of the 256 stubs in entry.S.
 */
void isr_dispatch(struct trapframe *tf);

void isr_dispatch(struct trapframe *tf)
{
	uint64_t vec = tf->vector;

	/* Hardware interrupts and IPIs. */
	if (vec >= VEC_IRQ_BASE) {
		irq_dispatch((uint32_t)vec);
		return;
	}

	switch (vec) {
	case VEC_PAGE_FAULT:
		handle_page_fault(tf);
		return;
	case VEC_GP_FAULT:
		handle_general_protection(tf);
		return;
	case VEC_INVALID_OPCODE:
		handle_invalid_opcode(tf);
		return;
	case VEC_MACHINE_CHECK:
		handle_machine_check(tf);
		return;
	case VEC_NMI:
		handle_nmi(tf);
		return;

	case VEC_BREAKPOINT:
		/* int3 from user space is a debugger trap, not a crash. */
		if (trapframe_from_user(tf)) {
			kinfo("trap: breakpoint from user mode at rip 0x%llx",
			      (unsigned long long)tf->rip);
			if (proc_fault_kill("breakpoint", 5))
				return;
		}
		dump_frame(tf);
		panic("breakpoint in kernel code at rip 0x%llx",
		      (unsigned long long)tf->rip);
		return;

	case VEC_DIVIDE_ERROR:
		if (trapframe_from_user(tf)) {
			dump_frame(tf);
			if (proc_fault_kill("divide by zero", 8))
				return;
		}
		fatal_exception(tf);
		return;

	case VEC_DEVICE_NA:
		/* #NM is raised when FPU/SSE state needs restoring.  HavenOS
		   does not yet do lazy FPU switching, so reaching this means
		   the kernel executed a vector instruction, which it is
		   compiled not to (-mgeneral-regs-only). */
		fatal_exception(tf);
		return;

	default:
		fatal_exception(tf);
		return;
	}
}

void exceptions_init(void)
{
	kinfo("exceptions: handlers installed; user-mode faults are "
	      "attributed to the process, kernel-mode faults panic");
}
