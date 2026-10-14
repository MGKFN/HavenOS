/* SPDX-License-Identifier: MIT
 *
 * panic.h — unrecoverable error handling and assertions.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's isr_handler() painted a red screen, printed the register
 * dump and halted — a good panic screen, but it was reachable only from a
 * CPU exception.  Nothing in the kernel could *declare* a state
 * unrecoverable, so impossible conditions in C code either continued with
 * bad data (ramfs_alloc() returning -1 to callers that ignored it) or were
 * reported into the log and stepped over.
 *
 * HavenOS makes the invariant explicit.  panic() is the single path to a
 * controlled stop and always produces the same artefact: the message, the
 * faulting location, a backtrace, and the kernel log tail — on every
 * registered sink, so a serial capture survives even when the display does
 * not.
 */
#ifndef _HAVEN_PANIC_H
#define _HAVEN_PANIC_H

#include <haven/types.h>

void panic(const char *fmt, ...) HAVEN_PRINTF(1, 2) HAVEN_NORETURN;

/* Print `count` frames of the current call chain through klog(). */
void backtrace(unsigned count);

void panic_at(const char *file, int line, const char *func,
              const char *fmt, ...) HAVEN_PRINTF(4, 5) HAVEN_NORETURN;

#define PANIC(...) panic_at(__FILE__, __LINE__, __func__, __VA_ARGS__)

/* Assertions are always compiled in.  A kernel that keeps running after an
   invariant breaks corrupts data silently, which is strictly worse than
   stopping: the cost of the branch is not worth the class of bug that
   NDEBUG would hide. */
#define HAVEN_ASSERT(cond)                                            \
	do {                                                          \
		if (unlikely(!(cond)))                                \
			panic_at(__FILE__, __LINE__, __func__,        \
			         "assertion failed: %s", #cond);      \
	} while (0)

#define HAVEN_ASSERT_MSG(cond, ...)                                   \
	do {                                                          \
		if (unlikely(!(cond)))                                \
			panic_at(__FILE__, __LINE__, __func__,        \
			         __VA_ARGS__);                        \
	} while (0)

/* For switch defaults and branches that are structurally impossible. */
#define HAVEN_BUG()                                                   \
	panic_at(__FILE__, __LINE__, __func__, "unreachable code reached")

bool panic_in_progress(void);

#endif /* _HAVEN_PANIC_H */
