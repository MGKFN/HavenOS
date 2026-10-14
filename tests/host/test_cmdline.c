/* SPDX-License-Identifier: MIT
 *
 * test_cmdline.c — exercise kernel/cmdline.c.
 *
 * The command line is the only input the kernel accepts before it has a
 * single device, and it comes from a bootloader configuration a user can
 * edit.  Two properties matter, and both are easy to get wrong by hand:
 *
 *   - matching is whole-word, so "nosmp" is not found inside "nosmpfoo"
 *     and "selftest" is not found inside "/boot/selftest.elf";
 *   - a value longer than the caller's buffer is truncated, not written
 *     past the end.
 *
 * HavenDOS's apply_cmdline() had the same whole-word intent and no test;
 * this is the test.
 */
#include "harness.h"
#include <haven/cmdline.h>
#include <haven/string.h>

static struct boot_info bi;

static void set_cmdline(const char *s)
{
	strlcpy(bi.cmdline, s, sizeof(bi.cmdline));
}

static void test_flags(void)
{
	TEST_GROUP("whole-word flag matching");

	set_cmdline("nosmp selftest");
	CHECK("a flag at the start is found", cmdline_has(&bi, "nosmp"));
	CHECK("a flag at the end is found", cmdline_has(&bi, "selftest"));
	CHECK("an absent flag is not found", !cmdline_has(&bi, "quiet"));

	set_cmdline("");
	CHECK("an empty command line finds nothing",
	      !cmdline_has(&bi, "nosmp"));

	/* The whole point of the token walk. */
	set_cmdline("nosmpfoo");
	CHECK("a flag is not found as a prefix of a longer token",
	      !cmdline_has(&bi, "nosmp"));
	set_cmdline("foonosmp");
	CHECK("nor as a suffix", !cmdline_has(&bi, "nosmp"));
	set_cmdline("/boot/selftest.elf");
	CHECK("nor inside a path", !cmdline_has(&bi, "selftest"));

	set_cmdline("   nosmp    selftest   ");
	CHECK("leading, trailing and repeated spaces are tolerated (first)",
	      cmdline_has(&bi, "nosmp"));
	CHECK("leading, trailing and repeated spaces are tolerated (second)",
	      cmdline_has(&bi, "selftest"));

	set_cmdline("loglevel=debug");
	CHECK("a key=value token is not a flag match for the key",
	      !cmdline_has(&bi, "loglevel"));
	CHECK("but is a flag match for the whole token",
	      cmdline_has(&bi, "loglevel=debug"));
}

static void test_values(void)
{
	TEST_GROUP("key=value extraction");

	char buf[16];

	set_cmdline("loglevel=debug nosmp");
	CHECK("a present key is reported present",
	      cmdline_value(&bi, "loglevel", buf, sizeof(buf)));
	CHECK_STR("and its value is returned", buf, "debug");

	set_cmdline("nosmp console=serial");
	CHECK("a key later in the line is found",
	      cmdline_value(&bi, "console", buf, sizeof(buf)));
	CHECK_STR("with the right value", buf, "serial");

	CHECK("an absent key is reported absent",
	      !cmdline_value(&bi, "loglevel", buf, sizeof(buf)));
	CHECK_STR("and the output is emptied, never left stale", buf, "");

	/* A bare key with no '=' is a flag, not a value. */
	set_cmdline("loglevel");
	CHECK("a key with no '=' yields no value",
	      !cmdline_value(&bi, "loglevel", buf, sizeof(buf)));

	set_cmdline("loglevel=");
	CHECK("an empty value yields no value either",
	      !cmdline_value(&bi, "loglevel", buf, sizeof(buf)));

	/* The matching must not be a substring search here either. */
	set_cmdline("myloglevel=debug");
	CHECK("a key is not matched as the tail of a longer key",
	      !cmdline_value(&bi, "loglevel", buf, sizeof(buf)));

	set_cmdline("loglevelx=debug");
	CHECK("nor as the head of a longer key",
	      !cmdline_value(&bi, "loglevel", buf, sizeof(buf)));
}

static void test_truncation(void)
{
	TEST_GROUP("a hostile value cannot overrun the caller's buffer");

	/* The buffer is bracketed by guard bytes so an overrun is detected
	   rather than inferred from a crash that may not happen. */
	struct {
		char guard_before[8];
		char buf[8];
		char guard_after[8];
	} s;

	memset(&s, 0x5A, sizeof(s));

	set_cmdline("console=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	CHECK("an over-long value is still reported present",
	      cmdline_value(&bi, "console", s.buf, sizeof(s.buf)));
	CHECK_EQ("truncated to the buffer size", strlen(s.buf), 7);
	CHECK_STR("keeping the leading characters", s.buf, "aaaaaaa");

	bool before_intact = true, after_intact = true;
	for (size_t i = 0; i < sizeof(s.guard_before); i++) {
		if ((unsigned char)s.guard_before[i] != 0x5A)
			before_intact = false;
		if ((unsigned char)s.guard_after[i] != 0x5A)
			after_intact = false;
	}
	CHECK("nothing was written before the buffer", before_intact);
	CHECK("nothing was written after the buffer", after_intact);

	/* A zero-sized buffer is the degenerate case a caller can reach by
	   passing sizeof() of a pointer rather than of an array. */
	CHECK("a zero-sized buffer is refused, not written to",
	      !cmdline_value(&bi, "console", s.buf, 0));
}

static void test_apply_early(void)
{
	TEST_GROUP("the early options take effect");

	/* console= is observable through its own accessor; loglevel= is
	   not, because the host stub for console_set_level() has nowhere
	   to put it.  The in-kernel path is covered by booting with
	   loglevel=debug, which is one of the GRUB menu entries. */
	set_cmdline("console=serial");
	cmdline_apply_early(&bi);
	CHECK("console=serial is remembered", cmdline_console_serial_only());

	set_cmdline("console=both");
	cmdline_apply_early(&bi);
	CHECK("console=both clears it again", !cmdline_console_serial_only());

	set_cmdline("console=nonsense loglevel=nonsense");
	CHECK_NO_PANIC("unrecognised values warn rather than fail",
	               cmdline_apply_early(&bi));
	CHECK("an unrecognised console= leaves every console enabled",
	      !cmdline_console_serial_only());

	set_cmdline("loglevel=4");
	CHECK_NO_PANIC("a numeric log level is accepted",
	               cmdline_apply_early(&bi));
	set_cmdline("loglevel=99");
	CHECK_NO_PANIC("an out-of-range numeric level is rejected safely",
	               cmdline_apply_early(&bi));
}

int main(void)
{
	printf("HavenOS host tests: kernel command line\n");
	printf("=======================================\n");
	printf("exercising kernel/cmdline.c\n");

	test_flags();
	test_values();
	test_truncation();
	test_apply_early();

	return haven_test_report("cmdline");
}
