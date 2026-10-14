/* SPDX-License-Identifier: MIT
 *
 * percpu.h — x86_64-private per-CPU bring-up.
 */
#ifndef _HAVEN_ARCH_X86_64_PERCPU_H
#define _HAVEN_ARCH_X86_64_PERCPU_H

#include <haven/types.h>

/* Initialise CPU `cpu_id`'s block.  Does not install it. */
void percpu_init_block(uint32_t cpu_id, uint32_t hw_id, bool is_bsp);

/* Point this CPU's GS base at block `cpu_id`.  After this, this_cpu()
   answers from the register rather than from the fallback block. */
void percpu_install(uint32_t cpu_id);

/* Record how many CPUs the firmware tables describe. */
void percpu_set_count(uint32_t n);

#endif /* _HAVEN_ARCH_X86_64_PERCPU_H */
