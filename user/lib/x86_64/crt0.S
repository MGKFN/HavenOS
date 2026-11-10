/* SPDX-License-Identifier: MIT
 *
 * crt0 -- the first instructions of every HavenOS program.
 *
 * The kernel enters here with every general register zeroed and RSP
 * pointing at:
 *
 *      [rsp]        argc
 *      [rsp+8]      argv[0] ... argv[argc-1], NULL
 *      then         envp[0] ... NULL
 *
 * RSP is 16-byte aligned at this point, which is what the ABI requires
 * immediately BEFORE a call, so main() can be called directly.
 */
#include <haven/abi/syscall_nr.h>

	.section .text
	.global _start
	.type _start, @function
_start:
	xorl %ebp, %ebp                /* end of the frame-pointer chain */
	movq (%rsp), %rdi              /* argc */
	leaq 8(%rsp), %rsi             /* argv */
	leaq 16(%rsp,%rdi,8), %rdx     /* envp = argv + argc + 1 */
	call main

	movl %eax, %edi                /* exit(main's return value) */
	movl $SYS_exit, %eax
	syscall
1:	hlt                            /* exit does not return; if it does, */
	jmp 1b                         /* #GP here and be killed, visibly   */
	.size _start, . - _start

.section .note.GNU-stack, "", @progbits
