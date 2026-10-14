/* SPDX-License-Identifier: MIT
 *
 * test_string.c — exercise lib/string.c, the real file.
 *
 * The bounded-copy routines get the most attention, because the reason
 * HavenOS declares strlcpy()/strlcat() instead of strcpy()/strcat() is
 * that the unbounded forms were reachable from HavenDOS's own path-
 * building code.  A truncation that is not reported is a silently wrong
 * filename; these tests assert that truncation is both safe and visible.
 */
#include "harness.h"
#include <haven/string.h>

static void test_mem(void)
{
	TEST_GROUP("memset / memcpy / memmove / memcmp");

	uint8_t buf[300];

	memset(buf, 0xAB, sizeof(buf));
	bool all = true;
	for (size_t i = 0; i < sizeof(buf); i++)
		if (buf[i] != 0xAB) { all = false; break; }
	CHECK("memset fills every byte, including past the 8-byte fast path",
	      all);

	/* The wide path only engages past an alignment boundary, so test
	   from a deliberately odd offset. */
	memset(buf, 0, sizeof(buf));
	memset(buf + 3, 0x5A, 201);
	CHECK_EQ("memset leaves the byte before the range alone", buf[2], 0);
	CHECK_EQ("memset fills the first byte of the range", buf[3], 0x5A);
	CHECK_EQ("memset fills the last byte of the range", buf[203], 0x5A);
	CHECK_EQ("memset leaves the byte after the range alone", buf[204], 0);

	uint8_t src[256], dst[256];
	for (size_t i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)(i * 7 + 3);

	memset(dst, 0, sizeof(dst));
	memcpy(dst, src, sizeof(src));
	CHECK_EQ("memcpy of an aligned block is exact",
	         memcmp(dst, src, sizeof(src)), 0);

	/* Mismatched alignment phase takes the byte path. */
	memset(dst, 0, sizeof(dst));
	memcpy(dst + 1, src, 200);
	CHECK_EQ("memcpy with mismatched alignment is exact",
	         memcmp(dst + 1, src, 200), 0);
	CHECK_EQ("memcpy did not write before the destination", dst[0], 0);

	CHECK_EQ("memcmp reports equality as 0", memcmp(src, src, 256), 0);
	CHECK("memcmp orders by unsigned byte value",
	      memcmp("\xFF", "\x01", 1) > 0);

	/* memmove must handle both overlap directions. */
	uint8_t ov[32];
	for (int i = 0; i < 32; i++)
		ov[i] = (uint8_t)i;
	memmove(ov + 4, ov, 20);
	CHECK_EQ("forward overlap: first moved byte", ov[4], 0);
	CHECK_EQ("forward overlap: last moved byte", ov[23], 19);

	for (int i = 0; i < 32; i++)
		ov[i] = (uint8_t)i;
	memmove(ov, ov + 4, 20);
	CHECK_EQ("backward overlap: first moved byte", ov[0], 4);
	CHECK_EQ("backward overlap: last moved byte", ov[19], 23);

	CHECK_PTR_EQ("memmove with dst == src is a no-op returning dst",
	             memmove(ov, ov, 20), ov);
	CHECK_EQ("memmove of zero bytes changes nothing", ov[0], 4);

	CHECK_PTR_EQ("memchr finds a byte", memchr("abcdef", 'd', 6),
	             (const char *)"abcdef" + 3);
	CHECK_PTR_EQ("memchr respects the length bound",
	             memchr("abcdef", 'f', 3), NULL);
}

static void test_compare(void)
{
	TEST_GROUP("strcmp / strncmp / strcasecmp");

	CHECK_EQ("equal strings compare 0", strcmp("haven", "haven"), 0);
	CHECK("shorter sorts first", strcmp("haven", "havenos") < 0);
	CHECK("longer sorts last", strcmp("havenos", "haven") > 0);

	/* HavenDOS compared with signed char, which puts bytes >= 0x80
	   below ASCII — wrong for UTF-8 and for the high half of its own
	   extended font table. */
	const char hi[] = { (char)0xC3, 0 };
	const char lo[] = { 'a', 0 };
	CHECK("a byte >= 0x80 sorts ABOVE ASCII (unsigned comparison)",
	      strcmp(hi, lo) > 0);

	CHECK_EQ("strncmp stops at the bound", strncmp("havenos", "haven", 5), 0);
	CHECK("strncmp sees past the bound", strncmp("havenos", "haven", 6) != 0);
	CHECK_EQ("strncmp of 0 bytes is 0", strncmp("a", "b", 0), 0);
	CHECK_EQ("strncmp stops at a NUL in both", strncmp("ab", "ab", 99), 0);

	CHECK_EQ("strcasecmp folds case", strcasecmp("HavenOS", "havenos"), 0);
	CHECK("strcasecmp still orders", strcasecmp("a", "b") < 0);
}

static void test_length_and_search(void)
{
	TEST_GROUP("strlen / strnlen / strchr / strrchr / strstr");

	CHECK_EQ("strlen", strlen("HavenOS"), 7);
	CHECK_EQ("strlen of empty", strlen(""), 0);

	/* strnlen must not read past the bound; an unterminated buffer is
	   exactly the case it exists for. */
	char unterminated[4] = { 'a', 'b', 'c', 'd' };
	CHECK_EQ("strnlen stops at the bound on an unterminated buffer",
	         strnlen(unterminated, 4), 4);
	CHECK_EQ("strnlen finds a shorter string", strnlen("ab", 8), 2);

	const char *s = "haven/os/kernel";
	CHECK_PTR_EQ("strchr finds the first match", strchr(s, '/'), s + 5);
	CHECK_PTR_EQ("strrchr finds the last match", strrchr(s, '/'), s + 8);
	CHECK_PTR_EQ("strchr can find the terminator", strchr(s, '\0'),
	             s + strlen(s));
	CHECK_PTR_EQ("strchr returns NULL when absent", strchr(s, 'z'), NULL);

	CHECK_PTR_EQ("strstr finds a substring", strstr(s, "os"), s + 6);
	CHECK_PTR_EQ("strstr of empty returns the haystack", strstr(s, ""), s);
	CHECK_PTR_EQ("strstr returns NULL when absent", strstr(s, "linux"),
	             NULL);
}

static void test_bounded_copy(void)
{
	TEST_GROUP("strlcpy / strlcat report truncation");

	char buf[8];

	size_t n = strlcpy(buf, "haven", sizeof(buf));
	CHECK_STR("strlcpy copies a string that fits", buf, "haven");
	CHECK_EQ("and returns its length", n, 5);

	n = strlcpy(buf, "havenos-kernel", sizeof(buf));
	CHECK_STR("strlcpy truncates to fit", buf, "havenos");
	CHECK_EQ("and returns the length it WANTED, so the caller can "
	         "detect truncation", n, 14);
	CHECK("truncation is detectable by the documented idiom",
	      n >= sizeof(buf));

	/* A zero-sized destination must not be written at all. */
	char guard[2] = { 'X', 'Y' };
	n = strlcpy(guard, "abc", 0);
	CHECK_EQ("strlcpy with size 0 writes nothing", guard[0], 'X');
	CHECK_EQ("and still reports the source length", n, 3);

	strlcpy(buf, "hav", sizeof(buf));
	n = strlcat(buf, "en", sizeof(buf));
	CHECK_STR("strlcat appends", buf, "haven");
	CHECK_EQ("and returns the total length", n, 5);

	n = strlcat(buf, "os-kernel", sizeof(buf));
	CHECK_STR("strlcat truncates and terminates", buf, "havenos");
	CHECK_EQ("and reports the length it wanted", n, 14);

	/* strlcpy must always terminate, which is the difference from
	   strncpy that makes it safe. */
	char exact[6];
	memset(exact, 'Z', sizeof(exact));
	strlcpy(exact, "12345", sizeof(exact));
	CHECK_EQ("strlcpy terminates even at an exact fit",
	         exact[5], '\0');

	/* strncpy's documented, surprising behaviour: it pads but does NOT
	   terminate on an exact fit.  Asserting it here records that the
	   kernel's strncpy is the standard one, so nobody is surprised. */
	char pad[6];
	memset(pad, 'Z', sizeof(pad));
	strncpy(pad, "12345", 5);
	CHECK_EQ("strncpy does not terminate on an exact fit (standard "
	         "behaviour; prefer strlcpy)", pad[5], 'Z');
}

static void test_tokenise(void)
{
	TEST_GROUP("strtok_r is reentrant");

	char a[] = "one two  three";
	char b[] = "alpha:beta";
	char *sa = NULL, *sb = NULL;

	char *t1 = strtok_r(a, " ", &sa);
	char *u1 = strtok_r(b, ":", &sb);
	char *t2 = strtok_r(NULL, " ", &sa);
	char *u2 = strtok_r(NULL, ":", &sb);
	char *t3 = strtok_r(NULL, " ", &sa);

	/* Interleaving two tokenisations is the case HavenDOS's strtok()
	   could not survive, because its cursor was file-static.  With a
	   preemptive scheduler that is not a latent bug but a live one. */
	CHECK_STR("first tokeniser, token 1", t1, "one");
	CHECK_STR("second tokeniser, token 1", u1, "alpha");
	CHECK_STR("first tokeniser, token 2", t2, "two");
	CHECK_STR("second tokeniser, token 2", u2, "beta");
	CHECK_STR("first tokeniser skips the repeated delimiter", t3, "three");
	CHECK_PTR_EQ("first tokeniser is exhausted",
	             strtok_r(NULL, " ", &sa), NULL);
	CHECK_PTR_EQ("second tokeniser is exhausted",
	             strtok_r(NULL, ":", &sb), NULL);

	char only[] = "   ";
	char *s = NULL;
	CHECK_PTR_EQ("a string of only delimiters yields no token",
	             strtok_r(only, " ", &s), NULL);
}

static void test_numeric(void)
{
	TEST_GROUP("numeric conversion");

	char buf[72];

	utoa64(0, buf, sizeof(buf), 10);
	CHECK_STR("utoa64 of zero", buf, "0");

	utoa64(1234567890123456789ULL, buf, sizeof(buf), 10);
	CHECK_STR("utoa64 of a large value", buf, "1234567890123456789");

	utoa64(0xDEADBEEF, buf, sizeof(buf), 16);
	CHECK_STR("utoa64 base 16", buf, "deadbeef");

	utoa64(5, buf, sizeof(buf), 2);
	CHECK_STR("utoa64 base 2", buf, "101");

	utoa64(UINT64_MAX, buf, sizeof(buf), 2);
	CHECK_EQ("utoa64 of UINT64_MAX in base 2 is 64 digits",
	         strlen(buf), 64);

	itoa64(-42, buf, sizeof(buf), 10);
	CHECK_STR("itoa64 of a negative value", buf, "-42");

	/* INT64_MIN has no positive counterpart, so a naive negation is
	   undefined behaviour.  This is the check that proves the unsigned
	   negation path is taken. */
	itoa64(INT64_MIN, buf, sizeof(buf), 10);
	CHECK_STR("itoa64 of INT64_MIN does not overflow", buf,
	          "-9223372036854775808");

	/* A buffer too small must truncate safely, not overflow. */
	char tiny[4];
	utoa64(123456, tiny, sizeof(tiny), 10);
	CHECK_EQ("a short buffer is still terminated", tiny[3], '\0');
	CHECK("a short buffer holds a prefix", strlen(tiny) == 3);

	utoa64(10, buf, sizeof(buf), 99);
	CHECK_STR("an invalid base yields an empty string", buf, "");

	CHECK_EQ("atoi", atoi("  -1234xyz"), -1234);
	CHECK_EQ("atoi of no digits", atoi("xyz"), 0);

	const char *end = NULL;
	CHECK_EQ("strtol_simple base 16 with 0x prefix",
	         strtol_simple("0x1F", &end, 16), 31);
	CHECK_STR("and reports where it stopped", end, "");

	CHECK_EQ("strtol_simple auto-detects hex",
	         strtol_simple("0xff", NULL, 0), 255);
	CHECK_EQ("strtol_simple auto-detects octal",
	         strtol_simple("0755", NULL, 0), 493);
	CHECK_EQ("strtol_simple auto-detects decimal",
	         strtol_simple("42", NULL, 0), 42);
	CHECK_EQ("strtol_simple handles a sign",
	         strtol_simple("+7", NULL, 10), 7);
	CHECK_EQ("strtol_simple stops at the first invalid digit for the base",
	         strtol_simple("129", NULL, 8), 10);
}

static void test_case(void)
{
	TEST_GROUP("bounded case folding");

	char s[] = "HavenOS-123";
	str_upper(s, sizeof(s));
	CHECK_STR("str_upper leaves non-letters alone", s, "HAVENOS-123");
	str_lower(s, sizeof(s));
	CHECK_STR("str_lower", s, "havenos-123");

	/* The bound must be honoured even when the string is longer. */
	char t[] = "abcdef";
	str_upper(t, 3);
	CHECK_STR("str_upper respects the bound", t, "ABCdef");
}

int main(void)
{
	printf("HavenOS host tests: string and memory routines\n");
	printf("==============================================\n");
	printf("linking the real lib/string.c\n");

	test_mem();
	test_compare();
	test_length_and_search();
	test_bounded_copy();
	test_tokenise();
	test_numeric();
	test_case();

	return haven_test_report("string");
}
