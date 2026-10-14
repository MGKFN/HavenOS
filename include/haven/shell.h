/* SPDX-License-Identifier: MIT
 *
 * shell.h — the kernel shell.
 *
 * Deliberately small.  Its job is to inspect and exercise the kernel
 * before any userland exists; ordinary programs are ELF files loaded
 * through the process layer, not commands compiled into the kernel.  That
 * is the structural difference from HavenDOS, whose shell was the only
 * interface and therefore had to contain a text editor, a calculator, a
 * package manager and a game -- 3976 lines of ring-0 code.
 */
#ifndef _HAVEN_SHELL_H
#define _HAVEN_SHELL_H

#include <haven/types.h>

/* Start the shell as a kernel thread.  Returns 0 or -HAVEN_E*. */
int  shell_start(void);

/* Run the shell on the calling thread, returning when it exits. */
void shell_run(void);

#endif /* _HAVEN_SHELL_H */
