/* SPDX-License-Identifier: MIT
 *
 * test_list.c — exercise the intrusive list in include/haven/list.h.
 *
 * Lists replace the fixed-size static arrays HavenDOS used throughout
 * (MAX_FILES 64, irq_handlers[16], a 256 KiB heap), so their correctness
 * underwrites every collection in the kernel: run queues, wait queues,
 * the buddy free lists, open-file tables and the driver registry.
 */
#include "harness.h"
#include <haven/list.h>

struct item {
	int              value;
	struct list_head node;
	struct list_head other;   /* a second list, same object */
};

static void test_basic(void)
{
	TEST_GROUP("insertion order and emptiness");

	LIST_HEAD(head);
	CHECK("a fresh list is empty", list_empty(&head));
	CHECK_EQ("and has length 0", list_length(&head), 0);

	struct item a = { .value = 1 };
	struct item b = { .value = 2 };
	struct item c = { .value = 3 };

	list_add_tail(&a.node, &head);
	list_add_tail(&b.node, &head);
	list_add_tail(&c.node, &head);

	CHECK("the list is no longer empty", !list_empty(&head));
	CHECK_EQ("length is 3", list_length(&head), 3);

	/* add_tail is FIFO: the queue order a scheduler run queue needs. */
	int seen[3], n = 0;
	struct item *it;
	list_for_each_entry(it, &head, node)
		seen[n++] = it->value;
	CHECK_EQ("add_tail yields FIFO order: first", seen[0], 1);
	CHECK_EQ("add_tail yields FIFO order: second", seen[1], 2);
	CHECK_EQ("add_tail yields FIFO order: third", seen[2], 3);

	CHECK_EQ("list_first_entry", list_first_entry(&head, struct item,
	                                              node)->value, 1);
	CHECK_EQ("list_last_entry", list_last_entry(&head, struct item,
	                                            node)->value, 3);

	/* add is LIFO: the stack order the buddy free lists use, because
	   handing back the most recently freed block keeps it cache-warm. */
	LIST_HEAD(stack);
	list_add(&a.other, &stack);
	list_add(&b.other, &stack);
	list_add(&c.other, &stack);
	CHECK_EQ("list_add yields LIFO order",
	         list_entry(stack.next, struct item, other)->value, 3);
}

static void test_removal(void)
{
	TEST_GROUP("removal, including of the element being iterated");

	LIST_HEAD(head);
	struct item items[6];
	for (int i = 0; i < 6; i++) {
		items[i].value = i;
		list_add_tail(&items[i].node, &head);
	}

	/* Remove the middle. */
	list_del(&items[3].node);
	CHECK_EQ("length drops after a removal", list_length(&head), 5);

	int sum = 0;
	struct item *it;
	list_for_each_entry(it, &head, node)
		sum += it->value;
	CHECK_EQ("the removed value is gone (0+1+2+4+5)", sum, 12);

	/* list_del poisons the entry, so a second del is a NULL
	   dereference rather than silent neighbour corruption. */
	CHECK_PTR_EQ("list_del poisons next", items[3].node.next, NULL);
	CHECK_PTR_EQ("list_del poisons prev", items[3].node.prev, NULL);

	/* The _safe iterator must survive deleting the current element —
	   this is the pattern every teardown path uses. */
	struct item *tmp;
	int removed = 0;
	list_for_each_entry_safe(it, tmp, &head, node) {
		if (it->value % 2 == 0) {
			list_del(&it->node);
			removed++;
		}
	}
	CHECK_EQ("the safe iterator removed every even element", removed, 3);
	CHECK_EQ("only odd elements remain", list_length(&head), 2);

	/* Remove the rest. */
	list_for_each_entry_safe(it, tmp, &head, node)
		list_del(&it->node);
	CHECK("the list is empty again", list_empty(&head));
}

static void test_del_init_and_move(void)
{
	TEST_GROUP("del_init and move_tail");

	LIST_HEAD(a);
	LIST_HEAD(b);
	struct item x = { .value = 7 };

	list_add_tail(&x.node, &a);
	list_del_init(&x.node);
	CHECK("del_init leaves the entry reusable, not poisoned",
	      x.node.next == &x.node && x.node.prev == &x.node);
	CHECK("so it can be re-added immediately", (list_add_tail(&x.node, &a),
	      list_length(&a) == 1));

	/* move_tail is how a thread migrates between run queues. */
	list_move_tail(&x.node, &b);
	CHECK_EQ("the source list is now empty", list_length(&a), 0);
	CHECK_EQ("the destination list holds it", list_length(&b), 1);
	CHECK_EQ("and the object is intact",
	         list_first_entry(&b, struct item, node)->value, 7);
}

static void test_multiple_lists(void)
{
	TEST_GROUP("one object on two lists at once");

	/* This is the capability a fixed-size array cannot provide and the
	   reason the kernel's collections are intrusive: a thread sits on
	   its process's thread list and on a run queue simultaneously. */
	LIST_HEAD(by_node);
	LIST_HEAD(by_other);

	struct item items[4];
	for (int i = 0; i < 4; i++) {
		items[i].value = i * 10;
		list_add_tail(&items[i].node,  &by_node);
		list_add_tail(&items[i].other, &by_other);
	}

	CHECK_EQ("both lists hold all four", list_length(&by_node), 4);
	CHECK_EQ("both lists hold all four", list_length(&by_other), 4);

	/* Removing from one list must not disturb the other. */
	list_del(&items[2].node);
	CHECK_EQ("removal from one list shortens only that list",
	         list_length(&by_node), 3);
	CHECK_EQ("the other list is untouched", list_length(&by_other), 4);

	int sum = 0;
	struct item *it;
	list_for_each_entry(it, &by_other, other)
		sum += it->value;
	CHECK_EQ("and still sees every object (0+10+20+30)", sum, 60);
}

static void test_container_of(void)
{
	TEST_GROUP("container_of recovers the enclosing object");

	struct item x = { .value = 99 };
	LIST_HEAD(head);
	list_add_tail(&x.node, &head);

	struct item *back = list_entry(head.next, struct item, node);
	CHECK_PTR_EQ("from the first member's list node", back, &x);
	CHECK_EQ("and the object is readable", back->value, 99);

	/* The second list head is at a non-zero offset, which is where a
	   hand-written pointer subtraction goes wrong. */
	list_add_tail(&x.other, &head);
	struct item *back2 = list_entry(head.prev, struct item, other);
	CHECK_PTR_EQ("from a member at a non-zero offset", back2, &x);
}

int main(void)
{
	printf("HavenOS host tests: intrusive list\n");
	printf("==================================\n");
	printf("exercising include/haven/list.h\n");

	test_basic();
	test_removal();
	test_del_init_and_move();
	test_multiple_lists();
	test_container_of();

	return haven_test_report("list");
}
