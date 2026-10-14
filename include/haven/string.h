/* SPDX-License-Identifier: MIT
 *
 * string.h — freestanding string and memory routines.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS exported strcpy() and strcat() with no bound on the
 * destination, and its own code used them on fixed stack buffers
 * (setup_user_home() built paths in char[48] with strncat).  HavenOS
 * keeps the unbounded names out of the kernel entirely: only the bounded
 * forms are declared, plus strlcpy()/strlcat(), which always terminate and
 * return the length they wanted to write so truncation is detectable.
 */
#ifndef _HAVEN_STRING_H
#define _HAVEN_STRING_H

#include <haven/types.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strtok_r(char *str, const char *delim, char **saveptr);

/* Bounded copy/concat.  Always NUL-terminate dst (when size > 0) and
   return the total length the operation wanted, so
   `if (strlcpy(d, s, sizeof d) >= sizeof d)` detects truncation. */
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

/* Numeric conversion.  base 2..16; buf must hold at least 65 bytes for
   base 2 of a 64-bit value plus sign and terminator. */
void   itoa64(int64_t val, char *buf, size_t bufsz, int base);
void   utoa64(uint64_t val, char *buf, size_t bufsz, int base);
long   strtol_simple(const char *s, const char **end, int base);
int    atoi(const char *s);

/* ASCII case folding, in place, bounded. */
void   str_upper(char *s, size_t n);
void   str_lower(char *s, size_t n);

#endif /* _HAVEN_STRING_H */
