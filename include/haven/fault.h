/* SPDX-License-Identifier: MIT
 *
 * fault.h — the architecture-neutral fault interface.
 *
 * An architecture's exception handler decodes its own fault registers and
 * then asks these two questions, both of which are answered by portable
 * code:
 *
 *   vm_handle_fault()  is this fault part of normal memory management --
 *                      demand paging, stack growth, copy-on-write?
 *   proc_fault_kill()  the fault is genuine; is there a process to blame,
 *                      so the machine can survive it?
 *
 * Keeping the trap frame out of this interface is deliberate.  An earlier
 * version passed `struct trapframe *`, which is an x86_64 type, so the VM
 * and process layers would have become architecture-dependent through
 * their own fault entry point -- the exact coupling the HAL exists to
 * prevent.  The flags below carry everything a portable handler needs.
 */
#ifndef _HAVEN_FAULT_H
#define _HAVEN_FAULT_H

#include <haven/types.h>

#define VM_FAULT_WRITE   (1u << 0)   /* the access was a write */
#define VM_FAULT_EXEC    (1u << 1)   /* the access was an instruction fetch */
#define VM_FAULT_USER    (1u << 2)   /* the access came from user mode */
#define VM_FAULT_PRESENT (1u << 3)   /* the page was present: a permission
                                        violation, not a missing mapping */

/* Returns true when the fault was resolved and the faulting instruction
   should be retried.  The weak default returns false, so a kernel built
   without the VM fault handler still reports faults correctly instead of
   calling into an uninitialised subsystem. */
bool vm_handle_fault(vaddr_t addr, uint32_t flags);

/* Terminate the process responsible for an unrecoverable user-mode fault
   and schedule something else.  Returns true when a process was killed —
   false means there is no process layer or no current process, in which
   case the architecture handler must treat the fault as fatal, because a
   user-mode fault that nothing can account for means the kernel has lost
   track of what is running. */
bool proc_fault_kill(const char *reason, int signal);

#endif /* _HAVEN_FAULT_H */
