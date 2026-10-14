/* SPDX-License-Identifier: MIT
 *
 * printf.h — formatted output.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's vga_printf() formatted straight into video memory, so there
 * was no way to format a string without drawing it, and no way to log
 * before the display was up.  HavenOS splits formatting (here) from
 * output sinks (console.h): vsnprintf() is the single formatting engine,
 * always NUL-terminates, never writes past the buffer, and returns the
 * length it *wanted* to write, which is what the callers that must detect
 * truncation need.
 */
#ifndef _HAVEN_PRINTF_H
#define _HAVEN_PRINTF_H

#include <haven/types.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) HAVEN_PRINTF(3, 4);

#endif /* _HAVEN_PRINTF_H */
