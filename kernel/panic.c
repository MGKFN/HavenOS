/* SPDX-License-Identifier: MIT
 *
 * panic.c — controlled stop with a readable artefact.
 */
#include <haven/panic.h>
#include <haven/console.h>
#include <haven/printf.h>
#include <haven/string.h>
#include <haven/arch.h>

static volatile uint32_t panicking;

bool panic_in_progress(void)
{
	return __atomic_load_n(&panicking, __ATOMIC_RELAXED) != 0;
}

/* Walk the frame-pointer chain.  The kernel is built with
   -fno-omit-frame-pointer precisely so this works; a backtrace that needs
   DWARF unwinding needs a heap and a parser, neither of which can be
   trusted at the moment they are most needed. */
void backtrace(unsigned count)
{
	void **frame = (void **)__builtin_frame_address(0);

	kputs_raw("  backtrace:\n");
	for (unsigned i = 0; i < count && frame; i++) {
		void *ret = frame[1];
		if (!ret)
			break;

		/* Stop at anything that is not a kernel text address rather
		   than chasing a corrupt chain into a fault. */
		if ((vaddr_t)ret < 0xFFFFFFFF80000000ULL)
			break;

		char line[64];
		snprintf(line, sizeof(line), "    [%u] %p\n", i, ret);
		kputs_raw(line);

		void **next = (void **)frame[0];
		/* The chain must move upward in memory; a frame pointer that
		   does not is corrupt, and following it loops forever. */
		if (next <= frame)
			break;
		frame = next;
	}
}

static void panic_common(const char *where, const char *fmt, va_list ap)
         HAVEN_NORETURN;

static void panic_common(const char *where, const char *fmt, va_list ap)
{
	arch_irq_disable();

	/* A second CPU (or a fault inside the panic path itself) must not
	   restart the sequence and recurse forever.  The first caller wins
	   and prints; later ones stop quietly. */
	if (__atomic_exchange_n(&panicking, 1, __ATOMIC_ACQ_REL))
		arch_halt();

	char msg[512];
	vsnprintf(msg, sizeof(msg), fmt, ap);

	kputs_raw("\n");
	kputs_raw("================================================================================\n");
	kputs_raw("                            *** HAVENOS KERNEL PANIC ***\n");
	kputs_raw("================================================================================\n");

	char line[640];
	snprintf(line, sizeof(line), "  %s\n", msg);
	kputs_raw(line);

	if (where) {
		snprintf(line, sizeof(line), "  at %s\n", where);
		kputs_raw(line);
	}

	snprintf(line, sizeof(line), "  cpu %u\n", arch_cpu_id());
	kputs_raw(line);

	backtrace(24);

	kputs_raw("--------------------------------------------------------------------------------\n");
	kputs_raw("  System halted.  The full kernel log is above; capture it from the serial\n");
	kputs_raw("  console (COM1, 115200 8N1) when reporting this.\n");
	kputs_raw("================================================================================\n");

	console_flush_all();
	arch_halt();
}

void panic(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	panic_common(NULL, fmt, ap);
	/* panic_common is noreturn; va_end is unreachable but documents
	   intent for anyone editing this. */
}

void panic_at(const char *file, int line, const char *func,
              const char *fmt, ...)
{
	char where[192];
	snprintf(where, sizeof(where), "%s:%d in %s()", file, line, func);

	va_list ap;
	va_start(ap, fmt);
	panic_common(where, fmt, ap);
}
