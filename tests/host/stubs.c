/* SPDX-License-Identifier: MIT
 *
 * stubs.c — the architecture layer, simulated for host tests.
 *
 * The kernel reaches physical memory only through arch_phys_to_virt(), so
 * "physical memory" here is one large host allocation and the translation
 * is an offset into it.  That is the whole trick that lets
 * kernel/mm/pmm.c — the real file, unmodified — be exercised natively.
 *
 * Everything else is the smallest implementation that preserves the
 * semantics the kernel relies on:
 *   - interrupt save/restore nests (a counter, so a missing restore is
 *     detectable rather than invisible)
 *   - panic() longjmps when a test has armed it, so error paths can be
 *     asserted instead of killing the runner
 *   - the console writes to stdout, or to a capture buffer when a test
 *     wants to assert on what the kernel reported
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <haven/boot.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/printf.h>

#include "harness.h"

int         haven_test_total;
int         haven_test_failed;
const char *haven_test_group = "";

jmp_buf haven_panic_jmp;
bool    haven_panic_armed;
bool    haven_panic_fired;
char    haven_panic_msg[512];

/* ── Simulated physical memory ───────────────────────────────────────── */

static uint8_t          *ram;
static uint64_t          ram_size;
static struct boot_info  machine;

void *haven_test_ram_base(void)   { return ram; }
uint64_t haven_test_ram_size(void) { return ram_size; }

vaddr_t arch_phys_to_virt(paddr_t pa)
{
	if (pa >= ram_size) {
		fprintf(stderr,
		        "stub: phys_to_virt(0x%llx) is outside the %llu MiB "
		        "simulated machine\n",
		        (unsigned long long)pa,
		        (unsigned long long)(ram_size >> 20));
		abort();
	}
	return (vaddr_t)(uintptr_t)(ram + pa);
}

paddr_t arch_virt_to_phys(vaddr_t va)
{
	uintptr_t p = (uintptr_t)va;
	uintptr_t b = (uintptr_t)ram;
	if (p < b || p >= b + ram_size) {
		fprintf(stderr, "stub: virt_to_phys(%p) is outside the "
		        "simulated machine\n", (void *)p);
		abort();
	}
	return (paddr_t)(p - b);
}

size_t arch_page_size_for(vaddr_t va, paddr_t pa, size_t len)
{
	/* The host simulation has no hardware large-page support to model,
	   so every mapping is 4 KiB.  Tests that care about large pages
	   exercise arch/x86_64/mm/paging.c directly. */
	(void)va;
	(void)pa;
	(void)len;
	return 4096;
}

struct boot_info *haven_test_make_machine(uint64_t bytes)
{
	haven_test_free_machine();

	ram_size = bytes;
	ram = calloc(1, (size_t)bytes);
	if (!ram) {
		fprintf(stderr, "stub: cannot allocate a %llu MiB simulated "
		        "machine\n", (unsigned long long)(bytes >> 20));
		exit(2);
	}

	memset(&machine, 0, sizeof(machine));
	machine.protocol_magic = 0x36D76289u;
	machine.firmware       = BOOT_FW_BIOS;
	strcpy(machine.loader_name, "host-test");

	/* A memory map shaped like what firmware actually reports: usable
	   low memory, the BIOS/EGA hole reserved, then the main block.
	   Tests exercise the real parsing path rather than a tidy
	   single-region fiction. */
	machine.memmap[0] = (struct boot_memmap_entry){
		.base = 0x0, .length = 0x9FC00, .type = BOOT_MEM_USABLE };
	machine.memmap[1] = (struct boot_memmap_entry){
		.base = 0x9FC00, .length = 0x60400, .type = BOOT_MEM_RESERVED };
	machine.memmap[2] = (struct boot_memmap_entry){
		.base = 0x100000, .length = bytes - 0x100000,
		.type = BOOT_MEM_USABLE };
	machine.memmap_count = 3;
	machine.ram_total    = 0x9FC00 + (bytes - 0x100000);
	machine.ram_highest  = bytes;

	/* A plausible kernel image: 2 MiB load address, 1 MiB long. */
	machine.kernel_phys_start = 0x200000;
	machine.kernel_phys_end   = 0x300000;
	machine.memmap[machine.memmap_count++] = (struct boot_memmap_entry){
		.base = machine.kernel_phys_start,
		.length = machine.kernel_phys_end - machine.kernel_phys_start,
		.type = BOOT_MEM_KERNEL };

	return &machine;
}

void haven_test_free_machine(void)
{
	free(ram);
	ram = NULL;
	ram_size = 0;
}

/* ── Interrupt state ─────────────────────────────────────────────────── */

static int irq_depth;
static bool irqs_on = true;

irqflags_t arch_irq_save(void)
{
	irqflags_t prev = irqs_on ? 1 : 0;
	irqs_on = false;
	irq_depth++;
	return prev;
}

void arch_irq_restore(irqflags_t flags)
{
	if (irq_depth <= 0) {
		fprintf(stderr, "stub: arch_irq_restore() without a matching "
		        "save\n");
		abort();
	}
	irq_depth--;
	irqs_on = (flags & 1) != 0;
}

void arch_irq_enable(void)  { irqs_on = true; }
void arch_irq_disable(void) { irqs_on = false; }
bool arch_irq_enabled(void) { return irqs_on; }

int haven_test_irq_depth(void) { return irq_depth; }

void arch_mb(void)  { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
void arch_rmb(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
void arch_wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }

void arch_pause(void) { }

uint32_t arch_cpu_id(void) { return 0; }
const char *arch_cpu_vendor(void) { return "HostTest"; }
const char *arch_cpu_model(void)  { return "HavenOS host test harness"; }

void arch_halt(void)
{
	fprintf(stderr, "stub: arch_halt() called\n");
	exit(haven_test_failed ? 1 : 0);
}

void arch_reboot(void)
{
	fprintf(stderr, "stub: arch_reboot() called\n");
	exit(haven_test_failed ? 1 : 0);
}

void arch_idle(void) { }

nstime_t arch_time_ns(void)
{
	/* Monotonic and cheap; tests that measure elapsed time want it to
	   advance, tests that log want a stable prefix. */
	static nstime_t t;
	return t += 1000;
}

void arch_delay_us(uint64_t us) { (void)us; }

void arch_tlb_flush_page(vaddr_t va) { (void)va; }
void arch_tlb_flush_all(void) { }

/* ── Console ─────────────────────────────────────────────────────────── */

static bool  capturing;
static char  log_buf[1 << 16];
static size_t log_len;

void haven_test_capture_log(bool on) { capturing = on; }
const char *haven_test_log(void)     { return log_buf; }

void haven_test_clear_log(void)
{
	log_len = 0;
	log_buf[0] = '\0';
}

static void sink(const char *buf, size_t len)
{
	if (capturing) {
		for (size_t i = 0; i < len && log_len < sizeof(log_buf) - 1; i++)
			log_buf[log_len++] = buf[i];
		log_buf[log_len] = '\0';
		return;
	}
	fwrite(buf, 1, len, stdout);
}

void klog(enum log_level level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	kvlog(level, fmt, ap);
	va_end(ap);
}

void kvlog(enum log_level level, const char *fmt, va_list ap)
{
	static const char *tag[] = { "PANIC", "ERROR", " WARN", " INFO",
	                             "DEBUG" };
	char line[1024];
	int  n = vsnprintf(line, sizeof(line), fmt, ap);
	if (n < 0)
		return;

	char out[1200];
	int  m = snprintf(out, sizeof(out), "        %s  %s\n",
	                  level < 5 ? tag[level] : "DEBUG", line);
	if (m > 0)
		sink(out, (size_t)m < sizeof(out) ? (size_t)m : sizeof(out) - 1);
}

void kwrite_raw(const char *buf, size_t len) { sink(buf, len); }
void kputs_raw(const char *s) { sink(s, strlen(s)); }

void console_register(struct console *con) { (void)con; }
void console_unregister(struct console *con) { (void)con; }
void console_flush_all(void) { fflush(stdout); }
void console_set_level(enum log_level level) { (void)level; }
enum log_level console_get_level(void) { return LOG_DEBUG; }

size_t console_ring_read(char *out, size_t outsz)
{
	size_t n = log_len < outsz ? log_len : outsz;
	memcpy(out, log_buf, n);
	return n;
}

size_t console_ring_size(void) { return log_len; }

/* ── Panic ───────────────────────────────────────────────────────────── */

void backtrace(unsigned count) { (void)count; }

bool panic_in_progress(void) { return false; }

static void panic_tail(const char *where, const char *fmt, va_list ap)
{
	char msg[400];
	vsnprintf(msg, sizeof(msg), fmt, ap);

	snprintf(haven_panic_msg, sizeof(haven_panic_msg), "panic: %s%s%s",
	         msg, where ? " at " : "", where ? where : "");

	haven_panic_fired = true;

	if (haven_panic_armed)
		longjmp(haven_panic_jmp, 1);

	fprintf(stderr, "\n*** unexpected kernel panic in test ***\n  %s\n",
	        haven_panic_msg);
	fflush(NULL);
	exit(3);
}

void panic(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	panic_tail(NULL, fmt, ap);
	abort();
}

void panic_at(const char *file, int line, const char *func,
              const char *fmt, ...)
{
	char where[256];
	snprintf(where, sizeof(where), "%s:%d in %s()", file, line, func);

	va_list ap;
	va_start(ap, fmt);
	panic_tail(where, fmt, ap);
	abort();
}

/* ── Reporting ───────────────────────────────────────────────────────── */

int haven_test_report(const char *suite)
{
	printf("\n%s: %d/%d checks passed",
	       suite, haven_test_total - haven_test_failed, haven_test_total);
	if (haven_test_failed)
		printf(", %d FAILED", haven_test_failed);
	printf("\n");
	return haven_test_failed ? 1 : 0;
}
