/* SPDX-License-Identifier: MIT
 *
 * syscall_nr.h -- the system-call numbers, and nothing else.
 *
 * This is the one header shared between the kernel and user programs.  It
 * contains no types and no declarations, so it can be included from C or
 * assembly on either side of the boundary; <haven/syscall.h> includes it
 * for the kernel, and user/include/haven/user.h for programs.  Keeping a
 * second copy of these numbers in the user tree would let the two drift,
 * and a drifted number is a program calling the wrong system call.
 *
 * THE NUMBERS ARE AN ABI AND DO NOT MOVE.  Append only; never reuse a gap.
 */
#ifndef _HAVEN_ABI_SYSCALL_NR_H
#define _HAVEN_ABI_SYSCALL_NR_H

#define SYS_exit          0
#define SYS_write         1
#define SYS_read          2
#define SYS_open          3
#define SYS_close         4
#define SYS_lseek         5
#define SYS_stat          6
#define SYS_getpid        7
#define SYS_gettid        8
#define SYS_yield         9
#define SYS_nanosleep    10
#define SYS_uname        11
#define SYS_brk          12
#define SYS_mmap         13
#define SYS_munmap       14
#define SYS_fork         15
#define SYS_execve       16
#define SYS_waitpid      17
#define SYS_kill         18
#define SYS_getcwd       19
#define SYS_chdir        20
#define SYS_mkdir        21
#define SYS_unlink       22
#define SYS_readdir      23
#define SYS_dup          24
#define SYS_pipe         25
#define SYS_time         26
#define SYS_sysinfo      27
#define SYS_thread_create 28
#define SYS_thread_join  29

#define SYS_MAX          30

#endif /* _HAVEN_ABI_SYSCALL_NR_H */
