/* SPDX-License-Identifier: MIT
 *
 * random.h — kernel entropy.
 *
 * Explicitly non-cryptographic.  A caller needing key material must not
 * use this; see the note in drivers/char/mem.c and the security-hardening
 * item in docs/DESIGN_DECISIONS.md.
 */
#ifndef _HAVEN_RANDOM_H
#define _HAVEN_RANDOM_H

#include <haven/types.h>

uint64_t haven_random64(void);
void     mem_devices_init(void);

#endif /* _HAVEN_RANDOM_H */
