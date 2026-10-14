/* SPDX-License-Identifier: MIT
 *
 * compiler.h — compiler attribute and barrier abstractions.
 *
 * HavenDOS sprinkled __attribute__((packed)) and inline asm barriers
 * directly through driver code, which made the sources GCC-only and hid
 * the intent.  HavenOS names each construct once here so the rest of the
 * tree reads as plain C and a Clang build needs no edits elsewhere.
 */
#ifndef _HAVEN_COMPILER_H
#define _HAVEN_COMPILER_H

#define HAVEN_PACKED        __attribute__((packed))
#define HAVEN_ALIGNED(n)    __attribute__((aligned(n)))
#define HAVEN_NORETURN      __attribute__((noreturn))
#define HAVEN_UNUSED        __attribute__((unused))
#define HAVEN_USED          __attribute__((used))
#define HAVEN_PRINTF(f, a)  __attribute__((format(printf, f, a)))
#define HAVEN_SECTION(s)    __attribute__((section(s)))
#define HAVEN_WEAK          __attribute__((weak))
#define HAVEN_ALWAYS_INLINE inline __attribute__((always_inline))
#define HAVEN_NOINLINE      __attribute__((noinline))
#define HAVEN_MAY_ALIAS     __attribute__((may_alias))

#define likely(x)           __builtin_expect(!!(x), 1)
#define unlikely(x)         __builtin_expect(!!(x), 0)

/* Compiler barrier: forbid reordering of memory accesses across this point.
   Not a CPU fence — see arch_mb()/arch_rmb()/arch_wmb() for those. */
#define barrier()           __asm__ volatile("" ::: "memory")

#define ARRAY_SIZE(a)       (sizeof(a) / sizeof((a)[0]))
#define HAVEN_UNREACHABLE() __builtin_unreachable()

/* Static assertion usable at file scope in C11 and later. */
#define HAVEN_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* container_of: recover the enclosing struct from a member pointer.  The
   intrusive list in list.h is built on this, which is why HavenOS needs no
   per-type list code the way HavenDOS's fixed-size arrays did. */
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Rounding helpers.  align must be a power of two. */
#define ALIGN_UP(x, align)   (((x) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define IS_ALIGNED(x, align) (((x) & ((align) - 1)) == 0)
#define DIV_ROUND_UP(n, d)   (((n) + (d) - 1) / (d))

#endif /* _HAVEN_COMPILER_H */
