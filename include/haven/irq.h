/* SPDX-License-Identifier: MIT
 *
 * irq.h — architecture-neutral interrupt handler registry.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS kept one global `irq_handlers[16]` array of void(void) function
 * pointers, with registration by index and no way to know which device had
 * claimed a line.  Three consequences:
 *
 *   - a handler received no context at all, so a driver with two devices
 *     needed a global to tell them apart;
 *   - only 16 lines existed, matching the two 8259s, so an APIC-based
 *     system with 24 or more lines could not be expressed;
 *   - registering twice silently replaced the first handler, which on a
 *     shared PCI line means one of the two devices stops working with no
 *     diagnostic.
 *
 * HavenOS allows several handlers per line (which is what sharing a PCI
 * interrupt actually requires), passes each one its own context pointer,
 * and reports who is on a line.
 */
#ifndef _HAVEN_IRQ_H
#define _HAVEN_IRQ_H

#include <haven/types.h>
#include <haven/list.h>

/* What a handler tells the kernel about the interrupt it just saw.
   Returning HANDLED vs NONE is what makes sharing work: the dispatcher
   can tell whether anybody claimed the interrupt, and report a line that
   nobody claims instead of silently acknowledging it forever. */
enum irq_result {
	IRQ_NONE = 0,     /* not my device */
	IRQ_HANDLED,      /* dealt with */
	IRQ_WAKE_THREAD,  /* dealt with the urgent part; wake the handler
	                     thread for the rest */
};

typedef enum irq_result (*irq_handler_fn)(uint32_t irq, void *context);

struct irq_action {
	struct list_head node;
	irq_handler_fn   handler;
	void            *context;
	const char      *name;
	uint64_t         count;
	uint64_t         spurious;
};

/* The generic layer's line number IS the interrupt vector; see the
   comment at the top of arch/x86_64/irq/arch_irq.c.  A driver that wants
   a legacy 8259 line writes IRQ_LINE_LEGACY(n), so the mapping is visible
   where it is used rather than hidden in a translation table. */
#define IRQ_LINE_LEGACY(n) (32u + (n))

void irq_init(void);

/* Register `action` on line `irq`.  Several actions may share a line.
   Returns 0 or -HAVEN_E*. */
int  irq_register(uint32_t irq, struct irq_action *action);
int  irq_unregister(uint32_t irq, struct irq_action *action);

/* Convenience form that allocates the action. */
int  irq_request(uint32_t irq, irq_handler_fn handler, void *context,
                 const char *name);

void irq_enable_line(uint32_t irq);
void irq_disable_line(uint32_t irq);

/* Called by the architecture's trap handler for a hardware interrupt.
   Walks the line's actions, counts what happened, and reports a line
   that nobody claims — which is how a wrongly routed interrupt gets
   found instead of wedging the machine with an unacknowledged line. */
void irq_dispatch(uint32_t irq);

void irq_dump(void);

struct irq_stats {
	uint64_t total;
	uint64_t spurious;
	uint64_t unhandled;
};

void irq_get_stats(struct irq_stats *out);
uint64_t irq_count_for(uint32_t irq);

#endif /* _HAVEN_IRQ_H */
