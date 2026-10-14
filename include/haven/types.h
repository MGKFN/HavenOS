/* SPDX-License-Identifier: MIT
 *
 * types.h — fundamental kernel types.
 *
 * HavenDOS typedef'd bool to uint8_t and defined true/false by macro
 * because it predates its own use of C99 headers.  HavenOS targets C11 and
 * uses the real <stdbool.h>, so kernel code and host unit tests agree on
 * what a bool is; the host tests link the same sources natively and a
 * mismatched bool would have been a silent ABI difference.
 */
#ifndef _HAVEN_TYPES_H
#define _HAVEN_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include <haven/compiler.h>

/* Physical and virtual addresses are distinct types on purpose.  The single
   most common class of bug in HavenDOS-era code was passing one where the
   other was meant; with paging enabled that is no longer merely sloppy, it
   is a fault.  These stay integer typedefs (not pointers) so arithmetic on
   them is legal and so a physical address is never accidentally
   dereferenced without going through phys_to_virt(). */
typedef uint64_t paddr_t;   /* physical address */
typedef uint64_t vaddr_t;   /* kernel/user virtual address */

typedef int64_t  ssize_t_k; /* signed size, named to avoid host ssize_t clash */
typedef uint64_t haven_off_t;

/* Process, thread and user identifiers. */
typedef int32_t  pid_t_k;
typedef int32_t  tid_t;
typedef uint32_t uid_t_k;
typedef uint32_t gid_t_k;

/* Monotonic time, nanoseconds since boot. */
typedef uint64_t nstime_t;

#define HAVEN_PAGE_SHIFT 12
#define HAVEN_PAGE_SIZE  (1UL << HAVEN_PAGE_SHIFT)   /* 4 KiB base page */
#define HAVEN_PAGE_MASK  (HAVEN_PAGE_SIZE - 1)

HAVEN_STATIC_ASSERT(sizeof(paddr_t) == 8, "HavenOS targets 64-bit hosts");

#endif /* _HAVEN_TYPES_H */
