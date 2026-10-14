/* SPDX-License-Identifier: MIT
 *
 * harness.h — minimal test framework for the HavenOS host test suite.
 *
 * WHY HOST TESTS THAT COMPILE THE REAL KERNEL SOURCES
 * HavenDOS's tests/test_pmm.c did exactly this — "compile the REAL
 * kernel/pmm.c natively and attack the allocator", with the reasoning
 * that "a bug here hands out memory that belongs to the kernel, so it is
 * worth testing directly rather than inferring from a successful boot".
 * That is the single best idea in the HavenDOS test suite and HavenOS
 * builds on it rather than reinventing it.
 *
 * The rule: a test links the actual .c file from kernel/, fs/ or lib/,
 * never a copy.  If the file changes, the test compiles against the
 * change.  Architecture dependencies are satisfied by tests/host/stubs.c,
 * which simulates physical memory with a plain allocation — which is
 * possible only because the kernel reaches physical memory exclusively
 * through arch_phys_to_virt().
 */
#ifndef HAVEN_TEST_HARNESS_H
#define HAVEN_TEST_HARNESS_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Counters ────────────────────────────────────────────────────────── */

extern int haven_test_total;
extern int haven_test_failed;
extern const char *haven_test_group;

/* ── Panic interception ──────────────────────────────────────────────── */

/* Kernel code under test calls panic() on an invariant violation.  A test
   that wants to prove the invariant is enforced needs to survive that, so
   the stub panic() longjmps here when a test has armed it. */
extern jmp_buf  haven_panic_jmp;
extern bool     haven_panic_armed;
extern bool     haven_panic_fired;
extern char     haven_panic_msg[512];

#define TEST_GROUP(name)                                                 \
	do {                                                             \
		haven_test_group = (name);                               \
		printf("\n%s\n", (name));                                \
		for (size_t _i = 0; _i < strlen(name); _i++)             \
			putchar('-');                                    \
		putchar('\n');                                           \
	} while (0)

static inline void haven_check(const char *what, bool cond,
                               const char *detail)
{
	haven_test_total++;
	if (cond) {
		printf("  ok    %s\n", what);
	} else {
		haven_test_failed++;
		if (detail && detail[0])
			printf("  FAIL  %s  (%s)\n", what, detail);
		else
			printf("  FAIL  %s\n", what);
	}
}

#define CHECK(what, cond) haven_check((what), (cond), "")

#define CHECK_EQ(what, got, want)                                        \
	do {                                                             \
		long long _g = (long long)(got);                         \
		long long _w = (long long)(want);                        \
		char _d[96];                                             \
		snprintf(_d, sizeof(_d), "got %lld, want %lld", _g, _w); \
		haven_check((what), _g == _w, _d);                       \
	} while (0)

#define CHECK_NE(what, got, bad)                                         \
	do {                                                             \
		long long _g = (long long)(got);                         \
		char _d[96];                                             \
		snprintf(_d, sizeof(_d), "got %lld", _g);                \
		haven_check((what), _g != (long long)(bad), _d);         \
	} while (0)

#define CHECK_STR(what, got, want)                                       \
	do {                                                             \
		const char *_g = (got);                                  \
		const char *_w = (want);                                 \
		char _d[256];                                            \
		snprintf(_d, sizeof(_d), "got \"%s\", want \"%s\"",      \
		         _g ? _g : "(null)", _w ? _w : "(null)");        \
		haven_check((what), _g && _w && strcmp(_g, _w) == 0, _d);\
	} while (0)

#define CHECK_PTR_EQ(what, got, want)                                    \
	do {                                                             \
		const void *_g = (const void *)(got);                    \
		const void *_w = (const void *)(want);                   \
		char _d[96];                                             \
		snprintf(_d, sizeof(_d), "got %p, want %p", _g, _w);     \
		haven_check((what), _g == _w, _d);                       \
	} while (0)

/* Run `stmt` expecting it to panic.  Passes when it does. */
#define CHECK_PANICS(what, stmt)                                         \
	do {                                                             \
		haven_panic_armed = true;                                \
		haven_panic_fired = false;                               \
		if (setjmp(haven_panic_jmp) == 0) {                      \
			stmt;                                            \
		}                                                        \
		haven_panic_armed = false;                               \
		haven_check((what), haven_panic_fired,                   \
		            "expected a panic, none occurred");          \
	} while (0)

/* Run `stmt` expecting it NOT to panic. */
#define CHECK_NO_PANIC(what, stmt)                                       \
	do {                                                             \
		haven_panic_armed = true;                                \
		haven_panic_fired = false;                               \
		if (setjmp(haven_panic_jmp) == 0) {                      \
			stmt;                                            \
		}                                                        \
		haven_panic_armed = false;                               \
		haven_check((what), !haven_panic_fired,                  \
		            haven_panic_msg);                            \
	} while (0)

/* ── Simulated physical memory, provided by stubs.c ──────────────────── */

/* Create a simulated machine with `bytes` of RAM.  Returns a boot_info
   describing it, with a memory map shaped like real firmware reports:
   a low region below 1 MiB, a reserved EGA/BIOS hole, and the main
   region above 1 MiB. */
struct boot_info;
struct boot_info *haven_test_make_machine(uint64_t bytes);
void              haven_test_free_machine(void);

/* The base of the simulated physical address space. */
void    *haven_test_ram_base(void);
uint64_t haven_test_ram_size(void);

/* Collect kernel log output instead of printing it, so a test can assert
   on what the code reported. */
void        haven_test_capture_log(bool on);
const char *haven_test_log(void);
void        haven_test_clear_log(void);

int haven_test_report(const char *suite);

#endif /* HAVEN_TEST_HARNESS_H */

/* ── Provided by stubs_mm.c, for tests of layers above memory management ── */

struct arch_pagetable;

void     haven_test_reset_pmm(void);
uint64_t haven_test_pages_handed_out(void);
uint64_t haven_test_live_allocations(void);
uint32_t haven_test_pt_prot(struct arch_pagetable *pt, uint64_t va);
size_t   haven_test_pt_count(struct arch_pagetable *pt);
int      haven_test_irq_depth(void);
