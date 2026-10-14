/* SPDX-License-Identifier: MIT
 *
 * multiboot2.h — the Multiboot2 handover parser's only public entry point.
 *
 * The structure definitions stay private to arch/x86_64/boot/multiboot2.c
 * on purpose: the generic kernel reads struct boot_info and must never
 * acquire a dependency on a firmware-specific layout.  That separation is
 * the fix for the class of bug HavenDOS hit when it parsed these tags
 * inline in kernel/main.c with literal field offsets.
 */
#ifndef _HAVEN_MULTIBOOT2_H
#define _HAVEN_MULTIBOOT2_H

#include <haven/boot.h>

/* Translate the Multiboot2 information block at physical address
   `info_pa` into `bi`.  Returns 0 on success, -1 when the handover is
   unusable (wrong magic, malformed tags, or no usable RAM reported). */
int multiboot2_parse(uint32_t magic, uint32_t info_pa, struct boot_info *bi);

#endif /* _HAVEN_MULTIBOOT2_H */
