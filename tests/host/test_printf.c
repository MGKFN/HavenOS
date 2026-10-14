/* SPDX-License-Identifier: MIT
 *
 * test_printf.c — exercise lib/printf.c, the real file.
 *
 * The kernel log is the primary debugging instrument, so a format bug
 * here corrupts the evidence for every other bug.  The truncation
 * behaviour gets particular attention: HavenDOS's vga_printf() had no
 * return value, so a message that did not fit simply lost its tail with
 * nothing to detect.
 */
#include "harness.h"
#include <haven/printf.h>
#include <haven/string.h>

static char out[512];

static const char *fmt(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	vsnprintf(out, sizeof(out), f, ap);
	va_end(ap);
	return out;
}

static void test_basics(void)
{
	TEST_GROUP("plain conversions");

	CHECK_STR("literal text", fmt("hello"), "hello");
	CHECK_STR("%% is a literal percent", fmt("100%%"), "100%");
	CHECK_STR("%c", fmt("[%c]", 'H'), "[H]");
	CHECK_STR("%s", fmt("%s-%s", "Haven", "OS"), "Haven-OS");
	CHECK_STR("a null %s does not fault", fmt("%s", (char *)NULL),
	          "(null)");

	CHECK_STR("%d", fmt("%d", 42), "42");
	CHECK_STR("%d negative", fmt("%d", -42), "-42");
	CHECK_STR("%d zero", fmt("%d", 0), "0");
	CHECK_STR("%u", fmt("%u", 4294967295u), "4294967295");
	CHECK_STR("%x", fmt("%x", 0xDEADBEEF), "deadbeef");
	CHECK_STR("%X", fmt("%X", 0xDEADBEEF), "DEADBEEF");
	CHECK_STR("%o", fmt("%o", 8), "10");
	CHECK_STR("%b", fmt("%b", 5), "101");
}

static void test_edge_integers(void)
{
	TEST_GROUP("integer edges");

	/* INT_MIN and LLONG_MIN have no positive counterpart, which is
	   where a naive negation is undefined behaviour. */
	CHECK_STR("INT32_MIN", fmt("%d", (int)INT32_MIN), "-2147483648");
	CHECK_STR("INT64_MIN", fmt("%lld", (long long)INT64_MIN),
	          "-9223372036854775808");
	CHECK_STR("UINT64_MAX", fmt("%llu", (unsigned long long)UINT64_MAX),
	          "18446744073709551615");
	CHECK_STR("UINT64_MAX in hex", fmt("%llx",
	          (unsigned long long)UINT64_MAX), "ffffffffffffffff");

	/* The length modifiers must select the right argument width;
	   getting this wrong reads half an argument and shifts every
	   subsequent one, which is the classic varargs corruption. */
	CHECK_STR("%ld", fmt("%ld", (long)-1234567890L), "-1234567890");
	CHECK_STR("%zu with a size_t", fmt("%zu", (size_t)123456789), "123456789");
	CHECK_STR("%lx", fmt("%lx", (unsigned long)0x1234ABCD), "1234abcd");
	CHECK_STR("mixed widths keep their order",
	          fmt("%d/%lld/%d", 1, (long long)2, 3), "1/2/3");
	CHECK_STR("%hd is accepted (already promoted to int)",
	          fmt("%hd", 7), "7");
}

static void test_width_precision(void)
{
	TEST_GROUP("width, precision and flags");

	CHECK_STR("right-aligned width", fmt("[%5d]", 42), "[   42]");
	CHECK_STR("left-aligned width", fmt("[%-5d]", 42), "[42   ]");
	CHECK_STR("zero padding", fmt("[%05d]", 42), "[00042]");
	CHECK_STR("zero padding with a sign", fmt("[%05d]", -42), "[-0042]");
	CHECK_STR("explicit plus", fmt("[%+d]", 42), "[+42]");
	CHECK_STR("space flag", fmt("[% d]", 42), "[ 42]");
	CHECK_STR("plus has no effect on a negative", fmt("[%+d]", -42), "[-42]");

	CHECK_STR("alt form on hex", fmt("[%#x]", 255), "[0xff]");
	CHECK_STR("alt form on zero prints no prefix", fmt("[%#x]", 0), "[0]");
	CHECK_STR("alt form on binary", fmt("[%#b]", 5), "[0b101]");

	CHECK_STR("width wider than the value", fmt("[%10s]", "hi"),
	          "[        hi]");
	CHECK_STR("precision bounds a string", fmt("[%.3s]", "havenos"),
	          "[hav]");
	CHECK_STR("precision on an integer sets minimum digits",
	          fmt("[%.5d]", 42), "[00042]");
	CHECK_STR("width is not exceeded by a longer value",
	          fmt("[%2d]", 123456), "[123456]");

	CHECK_STR("star width", fmt("[%*d]", 6, 42), "[    42]");
	CHECK_STR("negative star width means left align",
	          fmt("[%*d]", -6, 42), "[42    ]");
	CHECK_STR("star precision", fmt("[%.*s]", 4, "havenos"), "[have]");
	CHECK_STR("negative star precision is as if omitted",
	          fmt("[%.*s]", -1, "haven"), "[haven]");

	/* An explicit precision overrides the zero flag, per C99. */
	CHECK_STR("precision overrides the zero flag",
	          fmt("[%08.3d]", 5), "[     005]");
}

static void test_pointer(void)
{
	TEST_GROUP("%p");

	int x = 0;
	snprintf(out, sizeof(out), "%p", (void *)&x);
	CHECK("%p is 0x-prefixed, because kernel logs are read next to "
	      "disassembly", strncmp(out, "0x", 2) == 0);

	CHECK_STR("a null pointer is legible", fmt("%p", (void *)NULL),
	          "(nil)");
}

static void test_truncation(void)
{
	TEST_GROUP("truncation is safe and reported");

	char small[8];
	int n = snprintf(small, sizeof(small), "%s", "havenos-kernel");

	CHECK_STR("the buffer holds a NUL-terminated prefix", small, "havenos");
	CHECK_EQ("the return value is the length it WANTED to write", n, 14);
	CHECK("so truncation is detectable", (size_t)n >= sizeof(small));

	/* A zero-sized buffer must not be written at all — including not
	   writing the terminator. */
	char guard[2] = { 'X', 'Y' };
	n = snprintf(guard, 0, "abc");
	CHECK_EQ("size 0 writes nothing", guard[0], 'X');
	CHECK_EQ("size 0 still returns the wanted length", n, 3);

	/* A one-byte buffer holds only the terminator. */
	char one[1] = { 'Z' };
	n = snprintf(one, 1, "abc");
	CHECK_EQ("size 1 writes only the terminator", one[0], '\0');
	CHECK_EQ("and reports the wanted length", n, 3);

	/* Exactly-fitting output must still be terminated. */
	char exact[4];
	n = snprintf(exact, sizeof(exact), "abc");
	CHECK_STR("an exact fit is terminated", exact, "abc");
	CHECK_EQ("and returns its length", n, 3);
}

static void test_malformed(void)
{
	TEST_GROUP("malformed formats stay visible");

	/* An unknown conversion must appear in the output, so the bad
	   format string is discoverable from the log rather than silently
	   swallowing its argument. */
	CHECK_STR("an unknown conversion is echoed", fmt("[%q]"), "[%q]");
	CHECK_STR("a trailing percent does not run off the end",
	          fmt("value: %"), "value: ");
	CHECK_STR("%f is not supported and says so", fmt("[%f]"), "[%f]");
}

static void test_realistic(void)
{
	TEST_GROUP("the shapes the kernel log actually uses");

	CHECK_STR("a memory report line",
	          fmt("pmm: %llu MiB RAM in %llu pages",
	              (unsigned long long)512, (unsigned long long)131072),
	          "pmm: 512 MiB RAM in 131072 pages");

	CHECK_STR("a timestamp prefix",
	          fmt("[%5llu.%06llu]", (unsigned long long)12,
	              (unsigned long long)345678),
	          "[   12.345678]");

	CHECK_STR("an address range",
	          fmt("0x%016llx..0x%016llx",
	              (unsigned long long)0x100000,
	              (unsigned long long)0x20000000),
	          "0x0000000000100000..0x0000000020000000");

	CHECK_STR("a feature line",
	          fmt("cpu: nx=%d pge=%d smep=%d", 1, 1, 0),
	          "cpu: nx=1 pge=1 smep=0");
}

int main(void)
{
	printf("HavenOS host tests: format engine\n");
	printf("=================================\n");
	printf("linking the real lib/printf.c\n");

	test_basics();
	test_edge_integers();
	test_width_precision();
	test_pointer();
	test_truncation();
	test_malformed();
	test_realistic();

	return haven_test_report("printf");
}
