/* SPDX-License-Identifier: MIT
 *
 * fbcon.h — the display console.
 *
 * One driver covers both display types firmware hands over (an EGA text
 * buffer from BIOS, a linear framebuffer from UEFI), so callers never ask
 * which one they have.  HavenDOS needed three parallel display drivers
 * for the same job; see the design note in drivers/video/fbcon.c.
 */
#ifndef _HAVEN_FBCON_H
#define _HAVEN_FBCON_H

#include <haven/types.h>
#include <haven/boot.h>

/* Bring up the display described by `bfb` and register it as a console
   sink.  Returns 0 on success, -1 when firmware gave us nothing usable —
   which is not fatal: the serial console carries on alone. */
int  fbcon_init(const struct boot_framebuffer *bfb);

/* Character-grid geometry, 0 when no display is up. */
void fbcon_geometry(uint32_t *cols, uint32_t *rows);
bool fbcon_available(void);

#endif /* _HAVEN_FBCON_H */
