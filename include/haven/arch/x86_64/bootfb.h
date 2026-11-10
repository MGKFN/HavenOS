/* SPDX-License-Identifier: MIT
 *
 * bootfb.h -- the boot framebuffer window (x86_64).
 */
#ifndef _HAVEN_ARCH_X86_64_BOOTFB_H
#define _HAVEN_ARCH_X86_64_BOOTFB_H

#include <haven/types.h>
#include <haven/boot.h>

/* Map the firmware's framebuffer into the boot page tables and record
   where in fb->mapped_at.  Safe to call for any framebuffer, including
   one that is absent or is EGA text (which it leaves alone).  Returns
   false if the framebuffer could not be mapped, in which case it must
   not be used before the kernel's own page tables are active. */
bool x86_bootfb_map(struct boot_framebuffer *fb);

/* Recreate the same mapping in the kernel's own page tables.  Called
   while they are being built, before they are activated, so the console's
   pointer stays valid across the switch. */
void x86_bootfb_carry_over(void);

#endif /* _HAVEN_ARCH_X86_64_BOOTFB_H */
