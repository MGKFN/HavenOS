/* SPDX-License-Identifier: MIT
 *
 * input.h — keyboard input.
 */
#ifndef _HAVEN_INPUT_H
#define _HAVEN_INPUT_H

#include <haven/types.h>

void ps2kbd_init(void);
bool kbd_available(void);

/* Block until a character is available.  Returns -1 when no keyboard is
   present or the scheduler is not running (in which case blocking would
   hang the only thread). */
int  kbd_getchar(void);

/* Return a character if one is waiting, or -1.  Safe from any context. */
int  kbd_getchar_nonblock(void);

void kbd_get_stats(uint64_t *seen, uint64_t *dropped);

#endif /* _HAVEN_INPUT_H */
