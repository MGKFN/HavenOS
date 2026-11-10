/* SPDX-License-Identifier: MIT
 *
 * The x86_64 system-call instruction sequence for user programs.
 *
 *   rax            call number, and the result on return
 *   rdi rsi rdx    arguments 1-3
 *   r10 r8 r9      arguments 4-6 (r10, not rcx: SYSCALL overwrites rcx)
 *   rcx r11        destroyed by the instruction itself
 */
#ifndef _HAVEN_USER_X86_64_SYSCALL_H
#define _HAVEN_USER_X86_64_SYSCALL_H

static inline long __syscall6(long nr, long a, long b, long c, long d,
                              long e, long f)
{
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	register long r9  __asm__("r9")  = f;
	long ret;

	__asm__ volatile("syscall"
	                 : "=a"(ret)
	                 : "a"(nr), "D"(a), "S"(b), "d"(c),
	                   "r"(r10), "r"(r8), "r"(r9)
	                 : "rcx", "r11", "memory");
	return ret;
}

#endif
