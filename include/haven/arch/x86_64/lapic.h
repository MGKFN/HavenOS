/* SPDX-License-Identifier: MIT
 *
 * lapic.h — Local APIC interface.
 */
#ifndef _HAVEN_ARCH_X86_64_LAPIC_H
#define _HAVEN_ARCH_X86_64_LAPIC_H

#include <haven/types.h>

bool     lapic_init(void);
bool     lapic_available(void);
uint32_t lapic_id(void);
void     lapic_eoi(void);

void     lapic_timer_init(uint32_t hz);
void     lapic_timer_stop(void);

void     lapic_send_ipi(uint32_t apic_id, uint32_t vector);
void     lapic_send_init_sipi(uint32_t apic_id, paddr_t trampoline);

#endif /* _HAVEN_ARCH_X86_64_LAPIC_H */
