/* SPDX-License-Identifier: MIT
 *
 * string.c — freestanding string and memory routines.
 *
 * These are deliberately simple byte-at-a-time implementations with one
 * exception: memset and memcpy move 64 bits at a time, because they are on
 * the path of every page clear and every framebuffer blit and the
 * byte loop HavenDOS used was measurably the slowest thing in its
 * console.  Correctness first — each routine is covered by
 * tests/host/test_string.c, which links this exact file.
 */
#include <haven/string.h>

void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	uint8_t  v = (uint8_t)c;

	/* Align to 8 bytes, fill 8 at a time, then the tail. */
	while (n && !IS_ALIGNED((uintptr_t)d, 8)) {
		*d++ = v;
		n--;
	}

	if (n >= 8) {
		uint64_t word = 0x0101010101010101ULL * v;
		uint64_t *dw  = (uint64_t *)d;
		size_t    nw  = n / 8;
		for (size_t i = 0; i < nw; i++)
			dw[i] = word;
		d += nw * 8;
		n -= nw * 8;
	}

	while (n--)
		*d++ = v;

	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t       *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	/* Only take the wide path when both sides share an alignment phase;
	   otherwise the unaligned 64-bit loads cost more than they save. */
	if (n >= 8 && (((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
		while (!IS_ALIGNED((uintptr_t)d, 8)) {
			*d++ = *s++;
			n--;
		}
		uint64_t       *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;
		size_t          nw = n / 8;
		for (size_t i = 0; i < nw; i++)
			dw[i] = sw[i];
		d += nw * 8;
		s += nw * 8;
		n -= nw * 8;
	}

	while (n--)
		*d++ = *s++;

	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	uint8_t       *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	if (d == s || n == 0)
		return dst;

	/* Overlap only matters when dst is inside [src, src+n). */
	if (d < s || d >= s + n)
		return memcpy(dst, src, n);

	/* Backward copy. */
	d += n;
	s += n;
	while (n--)
		*--d = *--s;

	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = (const uint8_t *)a;
	const uint8_t *y = (const uint8_t *)b;

	for (size_t i = 0; i < n; i++) {
		if (x[i] != y[i])
			return (int)x[i] - (int)y[i];
	}
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const uint8_t *p = (const uint8_t *)s;
	uint8_t        v = (uint8_t)c;

	for (size_t i = 0; i < n; i++) {
		if (p[i] == v)
			return (void *)(p + i);
	}
	return NULL;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
	size_t n = 0;
	while (n < max && s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	/* Compare as unsigned: bytes >= 0x80 must sort above ASCII, which the
	   signed char comparison HavenDOS used got backwards for UTF-8 and
	   for the high half of its own extended font table. */
	return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint8_t ca = (uint8_t)a[i];
		uint8_t cb = (uint8_t)b[i];
		if (ca != cb)
			return (int)ca - (int)cb;
		if (ca == 0)
			return 0;
	}
	return 0;
}

static inline char fold(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int strcasecmp(const char *a, const char *b)
{
	while (*a && fold(*a) == fold(*b)) {
		a++;
		b++;
	}
	return (int)(uint8_t)fold(*a) - (int)(uint8_t)fold(*b);
}

char *strncpy(char *dst, const char *src, size_t n)
{
	size_t i = 0;
	for (; i < n && src[i]; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = '\0';
	return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t srclen = strlen(src);

	if (size > 0) {
		size_t copy = (srclen < size - 1) ? srclen : size - 1;
		memcpy(dst, src, copy);
		dst[copy] = '\0';
	}
	/* Returning the source length (not the copied length) is what makes
	   truncation detectable by the caller. */
	return srclen;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
	size_t dlen = strnlen(dst, size);
	size_t slen = strlen(src);

	if (dlen == size)
		return size + slen;   /* dst was not terminated within size */

	size_t room = size - dlen - 1;
	size_t copy = (slen < room) ? slen : room;
	memcpy(dst + dlen, src, copy);
	dst[dlen + copy] = '\0';

	return dlen + slen;
}

char *strchr(const char *s, int c)
{
	char target = (char)c;
	for (;; s++) {
		if (*s == target)
			return (char *)s;
		if (*s == '\0')
			return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	char        target = (char)c;
	const char *found  = NULL;

	for (;; s++) {
		if (*s == target)
			found = s;
		if (*s == '\0')
			break;
	}
	return (char *)found;
}

char *strstr(const char *hay, const char *needle)
{
	if (!*needle)
		return (char *)hay;

	size_t nlen = strlen(needle);
	for (; *hay; hay++) {
		if (*hay == *needle && strncmp(hay, needle, nlen) == 0)
			return (char *)hay;
	}
	return NULL;
}

/* Reentrant tokeniser.  HavenDOS used strtok() with a file-static cursor,
   which is a latent bug the moment two call sites interleave — and with a
   preemptive scheduler it is no longer latent, so only the _r form exists. */
char *strtok_r(char *str, const char *delim, char **saveptr)
{
	char *s = str ? str : *saveptr;
	if (!s)
		return NULL;

	while (*s && strchr(delim, *s))
		s++;
	if (!*s) {
		*saveptr = s;
		return NULL;
	}

	char *token = s;
	while (*s && !strchr(delim, *s))
		s++;
	if (*s) {
		*s = '\0';
		s++;
	}
	*saveptr = s;
	return token;
}

static const char digits[] = "0123456789abcdef";

void utoa64(uint64_t val, char *buf, size_t bufsz, int base)
{
	char tmp[65];
	int  i = 0;

	if (bufsz == 0)
		return;
	if (base < 2 || base > 16) {
		buf[0] = '\0';
		return;
	}

	if (val == 0) {
		tmp[i++] = '0';
	} else {
		while (val) {
			tmp[i++] = digits[val % (uint64_t)base];
			val /= (uint64_t)base;
		}
	}

	size_t out = 0;
	while (i > 0 && out < bufsz - 1)
		buf[out++] = tmp[--i];
	buf[out] = '\0';
}

void itoa64(int64_t val, char *buf, size_t bufsz, int base)
{
	if (bufsz == 0)
		return;

	if (val < 0 && base == 10) {
		buf[0] = '-';
		/* Negate in unsigned space so INT64_MIN does not overflow —
		   -(-9223372036854775808) is undefined in signed arithmetic. */
		utoa64((uint64_t)(-(val + 1)) + 1ULL, buf + 1, bufsz - 1, base);
		return;
	}
	utoa64((uint64_t)val, buf, bufsz, base);
}

long strtol_simple(const char *s, const char **end, int base)
{
	bool neg = false;
	long acc = 0;

	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;

	if (*s == '-') {
		neg = true;
		s++;
	} else if (*s == '+') {
		s++;
	}

	if ((base == 16 || base == 0) && s[0] == '0' &&
	    (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = (s[0] == '0' && s[1]) ? 8 : 10;
	}

	for (;; s++) {
		int d;
		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;

		if (d >= base)
			break;
		acc = acc * base + d;
	}

	if (end)
		*end = s;
	return neg ? -acc : acc;
}

int atoi(const char *s)
{
	return (int)strtol_simple(s, NULL, 10);
}

void str_upper(char *s, size_t n)
{
	for (size_t i = 0; i < n && s[i]; i++) {
		if (s[i] >= 'a' && s[i] <= 'z')
			s[i] = (char)(s[i] - 32);
	}
}

void str_lower(char *s, size_t n)
{
	for (size_t i = 0; i < n && s[i]; i++) {
		if (s[i] >= 'A' && s[i] <= 'Z')
			s[i] = (char)(s[i] + 32);
	}
}
