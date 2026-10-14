/* SPDX-License-Identifier: MIT
 *
 * list.h — intrusive doubly-linked circular list.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS used fixed-size static arrays everywhere: ramfs had
 * MAX_FILES 64, the IRQ table was irq_handlers[16], the heap was a
 * 256 KiB static pool.  Every one of those limits was a hard ceiling that
 * silently dropped work when exceeded (ramfs_alloc() returning -1 with no
 * caller checking it, for instance).  HavenOS stores collections in
 * intrusive lists so capacity is bounded by available memory, not by a
 * compile-time constant, and so an object can sit on several lists at once
 * (a thread is simultaneously on its process's thread list and on a
 * run queue or wait queue).
 *
 * All operations are O(1).  None of them allocate.
 */
#ifndef _HAVEN_LIST_H
#define _HAVEN_LIST_H

#include <haven/types.h>

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name)      struct list_head name = LIST_HEAD_INIT(name)

static inline void list_init(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *new_node,
                              struct list_head *prev,
                              struct list_head *next)
{
	new_node->next = next;
	new_node->prev = prev;
	next->prev = new_node;
	prev->next = new_node;
}

/* Insert immediately after head (LIFO / stack order). */
static inline void list_add(struct list_head *new_node, struct list_head *head)
{
	__list_add(new_node, head, head->next);
}

/* Insert immediately before head (FIFO / queue order). */
static inline void list_add_tail(struct list_head *new_node,
                                 struct list_head *head)
{
	__list_add(new_node, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	/* Poison so a second list_del() on the same entry faults in a
	   debuggable way instead of quietly corrupting a neighbour.  This is
	   the list-level equivalent of the heap magic values HavenDOS added
	   in its 0.6.8 heap rewrite. */
	entry->next = NULL;
	entry->prev = NULL;
}

/* Remove and re-initialise, so the entry is immediately reusable. */
static inline void list_del_init(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	list_init(entry);
}

static inline bool list_empty(const struct list_head *head)
{
	return head->next == head;
}

/* Move an entry that is already on some list to the tail of another. */
static inline void list_move_tail(struct list_head *entry,
                                  struct list_head *head)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	list_add_tail(entry, head);
}

static inline size_t list_length(const struct list_head *head)
{
	size_t n = 0;
	const struct list_head *p;
	for (p = head->next; p != head; p = p->next)
		n++;
	return n;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_first_entry(head, type, member) \
	list_entry((head)->next, type, member)

#define list_last_entry(head, type, member) \
	list_entry((head)->prev, type, member)

#define list_for_each(pos, head) \
	for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/* Safe against list_del() of the current element. */
#define list_for_each_safe(pos, n, head)                     \
	for ((pos) = (head)->next, (n) = (pos)->next;        \
	     (pos) != (head);                                \
	     (pos) = (n), (n) = (pos)->next)

#define list_for_each_entry(pos, head, member)                              \
	for ((pos) = list_entry((head)->next, __typeof__(*(pos)), member);   \
	     &(pos)->member != (head);                                       \
	     (pos) = list_entry((pos)->member.next, __typeof__(*(pos)), member))

#define list_for_each_entry_safe(pos, n, head, member)                       \
	for ((pos) = list_entry((head)->next, __typeof__(*(pos)), member),   \
	     (n) = list_entry((pos)->member.next, __typeof__(*(pos)), member); \
	     &(pos)->member != (head);                                       \
	     (pos) = (n),                                                    \
	     (n) = list_entry((n)->member.next, __typeof__(*(n)), member))

#endif /* _HAVEN_LIST_H */
