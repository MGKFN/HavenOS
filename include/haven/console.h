/* SPDX-License-Identifier: MIT
 *
 * console.h — kernel log and console sinks.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS logged by calling vga_printf(), which formatted directly into
 * video memory.  Three consequences, all of which cost it real debugging
 * time: nothing could be logged before vga_init() ran, nothing could be
 * logged on a machine whose display was not working (its own UEFI
 * framebuffer bring-up being exactly that case), and the log existed only
 * on screen, where it scrolled away.
 *
 * HavenOS separates the log from the display.  klog() formats once into a
 * ring buffer and then fans the result out to every registered sink.  The
 * serial sink registers before anything else in the boot sequence, so the
 * very first line of a boot is already captured on a machine with no
 * working display — which is the only practical way to debug the early
 * boot on real hardware.
 */
#ifndef _HAVEN_CONSOLE_H
#define _HAVEN_CONSOLE_H

#include <haven/types.h>

enum log_level {
	LOG_PANIC = 0,   /* the system is going down now */
	LOG_ERR,         /* a real failure, system continues */
	LOG_WARN,        /* something unexpected but handled */
	LOG_INFO,        /* normal progress reporting */
	LOG_DEBUG,       /* development detail, off by default */
	LOG_LEVEL_COUNT
};

/* A console sink.  write() receives already-formatted bytes and must not
   allocate, take the console lock, or call klog() — it runs inside the
   log path, including from panic context with interrupts off. */
struct console {
	const char       *name;
	void            (*write)(const char *buf, size_t len);
	void            (*flush)(void);     /* optional */
	struct console   *next;
	enum log_level    min_level;
	bool              enabled;
};

void console_register(struct console *con);
void console_unregister(struct console *con);
void console_flush_all(void);

/* Global threshold.  Messages above this level are still recorded in the
   ring buffer but are not written to sinks, so `dmesg` can show detail the
   live console suppressed. */
void           console_set_level(enum log_level level);
enum log_level console_get_level(void);

void klog(enum log_level level, const char *fmt, ...) HAVEN_PRINTF(2, 3);
void kvlog(enum log_level level, const char *fmt, va_list ap);

/* Raw, unformatted, unprefixed output — for the panic path and for
   anything that must appear with no decoration. */
void kputs_raw(const char *s);
void kwrite_raw(const char *buf, size_t len);

#define kerr(...)   klog(LOG_ERR,   __VA_ARGS__)
#define kwarn(...)  klog(LOG_WARN,  __VA_ARGS__)
#define kinfo(...)  klog(LOG_INFO,  __VA_ARGS__)
#define kdebug(...) klog(LOG_DEBUG, __VA_ARGS__)

/* Ring-buffer access, for the `dmesg` command and for crash dumps. */
size_t console_ring_read(char *out, size_t outsz);
size_t console_ring_size(void);

/* Early serial bring-up, called before any allocator exists. */
void serial_early_init(void);
bool serial_available(void);

/* Non-blocking read of one byte from the serial console, or -1 if none is
   waiting.  This is the kernel's input of last resort: it works on a
   machine with no keyboard driver and no working display, which is how a
   headless or half-initialised board is debugged. */
int  serial_getc_nonblock(void);

#endif /* _HAVEN_CONSOLE_H */
