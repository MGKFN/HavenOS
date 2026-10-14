/* SPDX-License-Identifier: MIT
 *
 * uaccess.c — copying across the user/kernel boundary.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS had no user mode, so it had no boundary to cross and nothing
 * equivalent to this file.  Everything here is new ground, and it is what
 * makes "secure user and kernel separation" a property the hardware
 * enforces rather than a convention the code follows.
 *
 * THE PROBLEM
 * A system call hands the kernel a pointer chosen by user code.  The
 * kernel must read or write through it, and the pointer may be anything:
 * unmapped, read-only, non-canonical, or a kernel address the process is
 * trying to reach.  A plain memcpy() through such a pointer either faults
 * the kernel or, worse, succeeds against kernel memory.
 *
 * THE SOLUTION, IN TWO PARTS
 * 1. Range checking rejects anything outside user space outright, which
 *    handles the "pointer to a kernel address" case with no faulting at
 *    all.
 * 2. For everything else, the copy is performed by a short assembly
 *    sequence whose faulting instructions are recorded in an exception
 *    table.  When a page fault lands on one of those addresses,
 *    uaccess_fixup() redirects execution to a label that returns
 *    -EFAULT.  The kernel survives a bad user pointer by design rather
 *    than by hoping the pointer is good.
 *
 * SMAP
 * When the CPU supports SMAP, ring 0 may not touch user pages at all
 * outside an explicit STAC/CLAC window.  The copy routines open that
 * window only for the duration of the copy, so a stray kernel access to
 * a user address anywhere else in the kernel faults immediately.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/memlayout.h>
#include <haven/arch/x86_64/regs.h>

/* Entries are emitted by the .pushsection in the copy routines below. */
struct uaccess_fixup_entry {
	uint64_t fault_addr;   /* address of the faulting instruction */
	uint64_t resume_addr;  /* where to continue instead */
};

extern const struct uaccess_fixup_entry __uaccess_fixup_start[];
extern const struct uaccess_fixup_entry __uaccess_fixup_end[];

bool uaccess_fixup(struct trapframe *tf)
{
	for (const struct uaccess_fixup_entry *e = __uaccess_fixup_start;
	     e < __uaccess_fixup_end; e++) {
		if (e->fault_addr == tf->rip) {
			/* Resume at the recovery label.  The routine's own
			   epilogue then returns -EFAULT to the caller. */
			tf->rip = e->resume_addr;
			return true;
		}
	}
	return false;
}

/* Is [addr, addr+len) entirely inside user space?
   The length check must not be able to overflow: `addr + len` with a
   length of (size_t)-1 wraps and would pass a naive bounds test. */
static bool user_range_ok(vaddr_t addr, size_t len)
{
	if (len == 0)
		return true;
	if (!is_canonical(addr))
		return false;
	if (addr < USER_MIN)
		return false;
	if (len > USER_MAX)
		return false;
	if (addr > USER_MAX - len)
		return false;
	return true;
}

/* STAC/CLAC are only legal when SMAP is enabled; executing them on a CPU
   without it raises #UD.  The feature test makes them conditional. */
static HAVEN_ALWAYS_INLINE void user_access_begin(void)
{
	if (arch_cpu_has_smap())
		__asm__ volatile("stac" ::: "cc");
}

static HAVEN_ALWAYS_INLINE void user_access_end(void)
{
	if (arch_cpu_has_smap())
		__asm__ volatile("clac" ::: "cc");
}

/*
 * The copy loop.  Byte at a time, deliberately: a word-at-a-time version
 * needs alignment handling and a tail, which triples the number of
 * instructions that can fault and therefore the number of fixup entries
 * to get right.  System-call buffers are small, and correctness here is
 * worth more than throughput.
 */
static int copy_bytes(void *dst, const void *src, size_t len)
{
	int rc = 0;

	__asm__ volatile(
		"1:\n\t"
		"testq %[len], %[len]\n\t"
		"jz    3f\n\t"
		"movb  (%[src]), %%al\n"        /* may fault: reading user */
		"2:\n\t"
		"movb  %%al, (%[dst])\n"        /* may fault: writing user */
		"incq  %[src]\n\t"
		"incq  %[dst]\n\t"
		"decq  %[len]\n\t"
		"jmp   1b\n"
		"3:\n\t"
		".pushsection .uaccess_fixup, \"a\", @progbits\n\t"
		".align 8\n\t"
		".quad 1b + 6\n\t"              /* the load  */
		".quad 4f\n\t"
		".quad 2b\n\t"                  /* the store */
		".quad 4f\n\t"
		".popsection\n\t"
		".pushsection .text\n"
		"4:\n\t"
		"movl $%c[efault], %[rc]\n\t"
		"jmp  3b\n\t"
		".popsection\n\t"
		: [rc] "+r"(rc), [src] "+r"(src), [dst] "+r"(dst),
		  [len] "+r"(len)
		: [efault] "i"(-HAVEN_EFAULT)
		: "al", "memory", "cc");

	return rc;
}

int arch_copy_from_user(void *dst, const void *user_src, size_t len)
{
	if (!user_range_ok((vaddr_t)user_src, len)) {
		/* Rejected without ever dereferencing it.  This is the check
		   that stops a process reading kernel memory by passing a
		   kernel address to a system call. */
		return -HAVEN_EFAULT;
	}

	user_access_begin();
	int rc = copy_bytes(dst, user_src, len);
	user_access_end();
	return rc;
}

int arch_copy_to_user(void *user_dst, const void *src, size_t len)
{
	if (!user_range_ok((vaddr_t)user_dst, len))
		return -HAVEN_EFAULT;

	user_access_begin();
	int rc = copy_bytes(user_dst, src, len);
	user_access_end();
	return rc;
}

int arch_strncpy_from_user(char *dst, const char *user_src, size_t len)
{
	if (len == 0)
		return -HAVEN_EINVAL;
	if (!user_range_ok((vaddr_t)user_src, 1))
		return -HAVEN_EFAULT;

	/* Copy one byte at a time, stopping at the terminator.  The length
	   is rechecked each time because the string's extent is not known
	   in advance: a string that starts in user space may run off its
	   end, and that must be -EFAULT rather than a kernel read. */
	for (size_t i = 0; i < len; i++) {
		if (!user_range_ok((vaddr_t)(user_src + i), 1)) {
			dst[0] = '\0';
			return -HAVEN_EFAULT;
		}

		char c;
		user_access_begin();
		int rc = copy_bytes(&c, user_src + i, 1);
		user_access_end();

		if (rc != 0) {
			dst[0] = '\0';
			return rc;
		}

		dst[i] = c;
		if (c == '\0')
			return (int)i;
	}

	/* No terminator within the bound.  Terminate what we have and say
	   the name was too long, rather than returning an unterminated
	   buffer the caller will read past. */
	dst[len - 1] = '\0';
	return -HAVEN_ENAMETOOLONG;
}
