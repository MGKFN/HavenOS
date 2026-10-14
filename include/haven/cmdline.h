/* SPDX-License-Identifier: MIT
 *
 * cmdline.h — the kernel command line.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS parsed its command line in apply_cmdline(), inside the kernel's
 * main function, with a token comparison per option.  The approach was
 * sound; the placement was not, because an option that had to take effect
 * before main() ran — anything about the log or the console — could not be
 * honoured at all.
 *
 * Here the parser is its own file with no dependencies beyond the boot
 * information block and the console, so the earliest architecture code can
 * call cmdline_apply_early() as soon as it has a struct boot_info, and
 * every later subsystem can ask about its own options with cmdline_has()
 * and cmdline_value().
 *
 * The matching is whole-word by construction.  A substring test would find
 * "nosmp" inside "nosmpfoo" and, worse, "selftest" inside a path.
 */
#ifndef _HAVEN_CMDLINE_H
#define _HAVEN_CMDLINE_H

#include <haven/types.h>
#include <haven/boot.h>

/* Is `flag` present as a whole word? */
bool cmdline_has(const struct boot_info *bi, const char *flag);

/* Copy the value of `key=value` into `out`.  `key` is given without the
   '=' sign.  Returns false and leaves `out` an empty string when the key
   is absent, so a caller can treat "missing" and "empty" alike. */
bool cmdline_value(const struct boot_info *bi, const char *key,
                   char *out, size_t outsz);

/* Options that must take effect before the rest of the kernel starts
   logging: `loglevel=` and `console=`.  Safe to call once the console
   exists and boot_info is filled in; harmless to call twice. */
void cmdline_apply_early(const struct boot_info *bi);

/* True when `console=serial` was given, meaning a framebuffer found by
   the firmware should not be taken over as a console.  Asked by the
   architecture's display bring-up. */
bool cmdline_console_serial_only(void);

#endif /* _HAVEN_CMDLINE_H */
