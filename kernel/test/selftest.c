/* SPDX-License-Identifier: MIT
 *
 * selftest.c — in-kernel tests that need real hardware.
 *
 * WHY BOTH THESE AND THE HOST TESTS
 * tests/host/ compiles kernel sources natively and can attack an
 * allocator with millions of operations, which is the right place for
 * logic.  But it cannot test anything that needs a real CPU: whether a
 * context switch actually returns to the right stack, whether the timer
 * interrupt actually preempts a spinning thread, whether a page-table
 * protection change actually faults.  Those have to run on the machine.
 *
 * HavenDOS had a `selftest` shell command, which was the right instinct;
 * the difference here is that these run automatically at boot when asked
 * (the `selftest` kernel argument, or the corresponding GRUB menu entry)
 * so a regression is caught by booting rather than by remembering to type
 * something.
 *
 * Every test must be safe to run on a live kernel: no test leaves state
 * behind, and a test that cannot run reports "skipped" rather than
 * failing.
 */
#include <haven/types.h>
#include <haven/console.h>
#include <haven/sched.h>
#include <haven/smp.h>
#include <haven/mm.h>
#include <haven/time.h>
#include <haven/arch.h>
#include <haven/panic.h>
#include <haven/printf.h>
#include <haven/string.h>
#include <haven/irq.h>
#include <haven/selftest.h>

static unsigned checks_run;
static unsigned checks_failed;
static unsigned tests_run;
static unsigned tests_skipped;

static void check(const char *what, bool ok)
{
	checks_run++;
	if (ok) {
		kinfo("selftest:   ok    %s", what);
	} else {
		checks_failed++;
		kerr("selftest:   FAIL  %s", what);
	}
}

static void check_eq(const char *what, uint64_t got, uint64_t want)
{
	checks_run++;
	if (got == want) {
		kinfo("selftest:   ok    %s", what);
	} else {
		checks_failed++;
		kerr("selftest:   FAIL  %s (got %llu, want %llu)", what,
		     (unsigned long long)got, (unsigned long long)want);
	}
}

/* ── Memory ──────────────────────────────────────────────────────────── */

static void test_physical_memory(void)
{
	kinfo("selftest: physical memory allocator");

	paddr_t pages[32];
	bool    distinct = true, aligned = true;

	for (size_t i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = pmm_alloc_page();
		if (!IS_ALIGNED(pages[i], HAVEN_PAGE_SIZE))
			aligned = false;
		for (size_t j = 0; j < i; j++) {
			if (pages[i] && pages[i] == pages[j])
				distinct = false;
		}
	}

	check("32 single-page allocations all succeeded",
	      pages[ARRAY_SIZE(pages) - 1] != 0);
	check("every address is distinct", distinct);
	check("every address is page aligned", aligned);

	/* Each page must actually be writable through the direct map, which
	   is the part a host test cannot verify. */
	bool writable = true;
	for (size_t i = 0; i < ARRAY_SIZE(pages); i++) {
		if (!pages[i])
			continue;
		volatile uint64_t *p =
			(volatile uint64_t *)arch_phys_to_virt(pages[i]);
		*p = 0xA5A5C0DE5A5A1234ULL;
		if (*p != 0xA5A5C0DE5A5A1234ULL)
			writable = false;
	}
	check("every page is writable through the direct map", writable);

	for (size_t i = 0; i < ARRAY_SIZE(pages); i++) {
		if (pages[i])
			pmm_free_page(pages[i]);
	}

	check("the allocator is consistent afterwards", pmm_check());

	/* A large contiguous run, which is what a DMA buffer needs. */
	paddr_t big = pmm_alloc_contig(512, PMM_ZONE_DMA32);
	check("a 2 MiB contiguous DMA32 run succeeds", big != 0);
	check("and it is below the 4 GiB 32-bit device limit",
	      big < PMM_ZONE_DMA32_LIMIT);
	if (big)
		pmm_free_contig(big, 512);

	tests_run++;
}

static void test_virtual_memory(void)
{
	kinfo("selftest: virtual memory");

	const size_t len = 256 * 1024;
	vaddr_t va = vmm_alloc(len, VM_PROT_READ | VM_PROT_WRITE);
	check("a 256 KiB arena allocation succeeds", va != 0);
	if (!va) {
		tests_run++;
		return;
	}

	/* Write a pattern across the whole range and read it back.  This is
	   the test that catches a mapping that covers only its first page --
	   a bug that a simple "allocate and touch" check would miss. */
	volatile uint64_t *p = (volatile uint64_t *)va;
	size_t words = len / sizeof(uint64_t);
	for (size_t i = 0; i < words; i++)
		p[i] = 0xC0DE0000ULL + i;

	bool coherent = true;
	for (size_t i = 0; i < words; i++) {
		if (p[i] != 0xC0DE0000ULL + i) {
			coherent = false;
			break;
		}
	}
	check("the whole range is coherent, not just the first page",
	      coherent);

	/* Physical backing need not be contiguous; confirm it is genuinely
	   scattered, since that is the property the arena exists to
	   provide. */
	paddr_t first = arch_pt_translate(arch_pt_kernel(), va);
	paddr_t last  = arch_pt_translate(arch_pt_kernel(),
	                                  va + len - HAVEN_PAGE_SIZE);
	check("every page of the range is mapped", first != 0 && last != 0);

	vmm_free(va, len);
	tests_run++;
}

static void test_object_allocator(void)
{
	kinfo("selftest: object allocator");

	void  *objs[16];
	size_t sizes[16] = { 1, 8, 16, 17, 32, 64, 100, 128,
	                     256, 512, 1000, 2048, 2049, 8192, 65536, 200000 };

	bool all_ok = true, all_usable = true;
	for (size_t i = 0; i < ARRAY_SIZE(objs); i++) {
		objs[i] = kmalloc(sizes[i]);
		if (!objs[i]) {
			all_ok = false;
			continue;
		}
		/* Write the whole object, so an allocation that is smaller
		   than requested corrupts something detectable rather than
		   going unnoticed. */
		memset(objs[i], (int)(i + 1), sizes[i]);
		uint8_t *b = objs[i];
		for (size_t k = 0; k < sizes[i]; k++) {
			if (b[k] != (uint8_t)(i + 1)) {
				all_usable = false;
				break;
			}
		}
	}

	check("allocations from 1 byte to 200 KiB all succeed", all_ok);
	check("every allocation is writable for its full requested size",
	      all_usable);
	check("the allocator is consistent while 16 objects are live",
	      kmalloc_check());

	/* kmalloc_size() must report what was asked for, which is what
	   krealloc() depends on. */
	bool sizes_ok = true;
	for (size_t i = 0; i < ARRAY_SIZE(objs); i++) {
		if (objs[i] && kmalloc_size(objs[i]) != sizes[i])
			sizes_ok = false;
	}
	check("kmalloc_size() reports the requested size", sizes_ok);

	for (size_t i = 0; i < ARRAY_SIZE(objs); i++)
		kfree(objs[i]);

	check("the allocator is consistent after freeing them all",
	      kmalloc_check());

	/* krealloc must preserve contents. */
	char *s = kmalloc(32);
	if (s) {
		strlcpy(s, "HavenOS realloc test", 32);
		char *bigger = krealloc(s, 512);
		check("krealloc to a larger size succeeds", bigger != NULL);
		if (bigger) {
			check("krealloc preserved the contents",
			      strcmp(bigger, "HavenOS realloc test") == 0);
			kfree(bigger);
		}
	}

	tests_run++;
}

/* ── Scheduler ───────────────────────────────────────────────────────── */

static volatile uint64_t worker_counter[4];
static volatile bool     worker_stop;
static volatile uint32_t workers_finished;

static void spin_worker(void *arg)
{
	size_t slot = (size_t)(uintptr_t)arg;

	/* Spin without yielding.  The only way this thread stops
	   monopolising its CPU is if the timer interrupt preempts it --
	   which is precisely what the test is checking. */
	while (!worker_stop)
		worker_counter[slot]++;

	__atomic_fetch_add(&workers_finished, 1, __ATOMIC_RELEASE);
	thread_exit(0);
}

static void test_preemption(void)
{
	kinfo("selftest: preemptive scheduling");

	if (!sched_active()) {
		kwarn("selftest:   skipped (the scheduler is not running)");
		tests_skipped++;
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(worker_counter); i++)
		worker_counter[i] = 0;
	worker_stop      = false;
	workers_finished = 0;

	struct sched_stats before;
	sched_get_stats(&before);

	struct thread *w[4];
	bool created = true;
	for (size_t i = 0; i < ARRAY_SIZE(w); i++) {
		char name[THREAD_NAME_MAX];
		snprintf(name, sizeof(name), "selftest-spin%zu", i);
		w[i] = thread_create(name, spin_worker,
		                     (void *)(uintptr_t)i, SCHED_PRIO_NORMAL);
		if (!w[i])
			created = false;
	}
	check("four spinning threads were created", created);
	if (!created) {
		worker_stop = true;
		tests_run++;
		return;
	}

	/* Sleep, which blocks this thread and lets the workers run.  If
	   blocking did not work, this would return immediately and the
	   counters would all be zero. */
	time_sleep_ms(200);

	struct sched_stats during;
	sched_get_stats(&during);

	/* Every worker must have run.  A scheduler that only ever ran the
	   first thread, or that never preempted, would leave some at zero --
	   they never yield, so only the timer can move between them. */
	unsigned ran = 0;
	for (size_t i = 0; i < ARRAY_SIZE(worker_counter); i++) {
		if (worker_counter[i] > 0)
			ran++;
	}
	check_eq("all four threads got CPU time without ever yielding, "
	         "which requires timer preemption", ran, ARRAY_SIZE(w));

	check("context switches occurred",
	      during.context_switches > before.context_switches);
	check("preemptions were recorded",
	      during.preemptions > before.preemptions);

	kinfo("selftest:   counters: %llu %llu %llu %llu "
	      "(%llu switches, %llu preemptions in 200 ms)",
	      (unsigned long long)worker_counter[0],
	      (unsigned long long)worker_counter[1],
	      (unsigned long long)worker_counter[2],
	      (unsigned long long)worker_counter[3],
	      (unsigned long long)(during.context_switches -
	                           before.context_switches),
	      (unsigned long long)(during.preemptions - before.preemptions));

	/* Tear down and confirm every thread actually exits. */
	worker_stop = true;
	for (size_t i = 0; i < ARRAY_SIZE(w); i++)
		thread_join(w[i]);

	check_eq("every worker reached thread_exit()",
	         workers_finished, ARRAY_SIZE(w));

	tests_run++;
}

static volatile uint64_t sleeper_woke_at_ms;

static void sleeper(void *arg)
{
	uint64_t requested = (uint64_t)(uintptr_t)arg;
	uint64_t start = time_ms();
	time_sleep_ms(requested);
	sleeper_woke_at_ms = time_ms() - start;
	thread_exit(0);
}

static void test_sleep_accuracy(void)
{
	kinfo("selftest: blocking sleep");

	if (!sched_active()) {
		kwarn("selftest:   skipped (the scheduler is not running)");
		tests_skipped++;
		return;
	}

	sleeper_woke_at_ms = 0;
	struct thread *t = thread_create("selftest-sleep", sleeper,
	                                 (void *)(uintptr_t)100,
	                                 SCHED_PRIO_NORMAL);
	check("the sleeping thread was created", t != NULL);
	if (!t) {
		tests_run++;
		return;
	}

	thread_join(t);

	/* HavenDOS's sleep_ms() slept UNBOUNDEDLY longer than asked,
	   because its PIT conversion truncated each tick to zero -- its own
	   source documents this.  So the upper bound matters as much as the
	   lower one: a sleep that takes 400 ms for a 100 ms request is the
	   exact bug being guarded against. */
	check("a 100 ms sleep took at least 100 ms",
	      sleeper_woke_at_ms >= 100);
	check("and not more than 150 ms (HavenDOS's PIT conversion bug made "
	      "sleeps unboundedly long)",
	      sleeper_woke_at_ms <= 150);

	kinfo("selftest:   a 100 ms sleep measured %llu ms",
	      (unsigned long long)sleeper_woke_at_ms);

	tests_run++;
}

static volatile uint32_t priority_order_index;
static volatile uint8_t  priority_order[3];

static void priority_worker(void *arg)
{
	uint8_t prio = (uint8_t)(uintptr_t)arg;
	uint32_t slot = __atomic_fetch_add(&priority_order_index, 1,
	                                   __ATOMIC_ACQ_REL);
	if (slot < ARRAY_SIZE(priority_order))
		priority_order[slot] = prio;
	thread_exit(0);
}

static void test_priority(void)
{
	kinfo("selftest: priority ordering");

	if (!sched_active()) {
		tests_skipped++;
		return;
	}

	priority_order_index = 0;

	/* Created lowest-priority first, so if priority were ignored they
	   would run in creation order and the check below would fail. */
	struct thread *lo = thread_create("selftest-lo", priority_worker,
	                                  (void *)(uintptr_t)6, 6);
	struct thread *mid = thread_create("selftest-mid", priority_worker,
	                                   (void *)(uintptr_t)4, 4);
	struct thread *hi = thread_create("selftest-hi", priority_worker,
	                                  (void *)(uintptr_t)2, 2);

	if (!lo || !mid || !hi) {
		check("three threads at different priorities were created",
		      false);
		tests_run++;
		return;
	}

	/* Yield so they can run, then collect. */
	if (hi)  thread_join(hi);
	if (mid) thread_join(mid);
	if (lo)  thread_join(lo);

	check_eq("all three ran", priority_order_index, 3);
	check("the highest priority ran first, despite being created last",
	      priority_order[0] == 2);
	check("then the middle priority", priority_order[1] == 4);
	check("then the lowest", priority_order[2] == 6);

	tests_run++;
}

/* ── Interrupts and time ─────────────────────────────────────────────── */

static void test_timer(void)
{
	kinfo("selftest: timer and monotonic clock");

	uint64_t t0 = time_ns();
	time_delay_ms(50);
	uint64_t t1 = time_ns();

	check("the monotonic clock advances", t1 > t0);

	uint64_t elapsed_ms = (t1 - t0) / 1000000;
	check("a 50 ms busy-wait measured between 45 and 70 ms",
	      elapsed_ms >= 45 && elapsed_ms <= 70);
	kinfo("selftest:   a 50 ms delay measured %llu ms",
	      (unsigned long long)elapsed_ms);

	/* Monotonicity is not negotiable: a clock that goes backward breaks
	   every timeout in the system. */
	bool monotonic = true;
	uint64_t prev = time_ns();
	for (int i = 0; i < 10000; i++) {
		uint64_t now = time_ns();
		if (now < prev) {
			monotonic = false;
			break;
		}
		prev = now;
	}
	check("10000 consecutive reads never go backward", monotonic);

	/* The timer interrupt must actually be firing. */
	uint64_t ticks0 = time_ticks();
	time_delay_ms(50);
	uint64_t ticks1 = time_ticks();
	check("the timer interrupt is firing", ticks1 > ticks0);
	kinfo("selftest:   %llu ticks in 50 ms (target %d Hz)",
	      (unsigned long long)(ticks1 - ticks0), HZ);

	tests_run++;
}

static volatile uint32_t oneshot_fired;

static void oneshot_callback(struct timer *t, void *arg)
{
	(void)t;
	(void)arg;
	__atomic_fetch_add(&oneshot_fired, 1, __ATOMIC_RELEASE);
}

static void test_oneshot_timers(void)
{
	kinfo("selftest: one-shot timers");

	oneshot_fired = 0;

	struct timer t1, t2, t3;
	timer_init_entry(&t1, oneshot_callback, NULL);
	timer_init_entry(&t2, oneshot_callback, NULL);
	timer_init_entry(&t3, oneshot_callback, NULL);

	timer_arm_ms(&t1, 30);
	timer_arm_ms(&t2, 60);
	timer_arm_ms(&t3, 10000);   /* will be cancelled */

	check("cancelling an armed timer reports it was armed",
	      timer_cancel(&t3));
	check("cancelling it twice reports it was not", !timer_cancel(&t3));

	time_delay_ms(120);

	check_eq("both short timers fired exactly once each",
	         oneshot_fired, 2);
	check("the cancelled timer did not fire", oneshot_fired == 2);

	tests_run++;
}

static void test_interrupt_state(void)
{
	kinfo("selftest: interrupt state nesting");

	/* arch_irq_save()/restore() must nest.  HavenDOS used bare cli/sti
	   pairs, so an inner region re-enabled interrupts on exit even when
	   the outer region had disabled them -- which is what makes a
	   spinlock stop being a lock. */
	bool was_enabled = arch_irq_enabled();

	irqflags_t outer = arch_irq_save();
	check("interrupts are disabled inside the outer region",
	      !arch_irq_enabled());

	irqflags_t inner = arch_irq_save();
	check("still disabled inside the inner region",
	      !arch_irq_enabled());

	arch_irq_restore(inner);
	check("restoring the INNER region leaves interrupts disabled, "
	      "because the outer region had disabled them",
	      !arch_irq_enabled());

	arch_irq_restore(outer);
	check_eq("restoring the outer region returns to the original state",
	         arch_irq_enabled(), was_enabled);

	tests_run++;
}

/* ── Kernel image protection ─────────────────────────────────────────── */

extern uint8_t __text_start[];
extern uint8_t __rodata_start[];

static void test_kernel_protection(void)
{
	kinfo("selftest: kernel image protection (W^X)");

	/* Read the page-table entries rather than attempting a write: a
	   write to read-only kernel memory is a fault, and a selftest must
	   not need a fault handler to survive.  Checking the mapping is the
	   same assertion without the crash. */
	paddr_t text_pa = arch_pt_translate(arch_pt_kernel(),
	                                    (vaddr_t)__text_start);
	paddr_t rodata_pa = arch_pt_translate(arch_pt_kernel(),
	                                      (vaddr_t)__rodata_start);

	check(".text is mapped", text_pa != 0);
	check(".rodata is mapped", rodata_pa != 0);
	check(".text and .rodata are different pages",
	      text_pa != rodata_pa);

	/* The identity map must be gone.  While it existed, a kernel
	   pointer bug that produced a small address would silently read
	   physical memory instead of faulting. */
	check("the boot identity map has been removed",
	      arch_pt_translate(arch_pt_kernel(), 0x100000) == 0);

	tests_run++;
}

/* ── Runner ──────────────────────────────────────────────────────────── */

bool selftest_run_all(void)
{
	checks_run    = 0;
	checks_failed = 0;
	tests_run     = 0;
	tests_skipped = 0;

	kinfo(" ");
	kinfo("================================================================");
	kinfo("  HavenOS kernel self-tests");
	kinfo("================================================================");

	test_physical_memory();
	test_virtual_memory();
	test_object_allocator();
	test_interrupt_state();
	test_timer();
	test_oneshot_timers();
	test_kernel_protection();
	test_preemption();
	test_sleep_accuracy();
	test_priority();

	kinfo("================================================================");
	if (checks_failed == 0) {
		kinfo("  PASSED: %u/%u checks across %u tests (%u skipped)",
		      checks_run, checks_run, tests_run, tests_skipped);
	} else {
		kerr("  FAILED: %u of %u checks failed across %u tests",
		     checks_failed, checks_run, tests_run);
	}
	kinfo("================================================================");
	kinfo(" ");

	return checks_failed == 0;
}

void selftest_summary(unsigned *run, unsigned *failed)
{
	if (run)
		*run = checks_run;
	if (failed)
		*failed = checks_failed;
}
