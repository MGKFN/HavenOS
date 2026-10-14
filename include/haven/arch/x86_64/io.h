/* SPDX-License-Identifier: MIT
 *
 * io.h — x86 port I/O, MSR and MMIO access.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS put these inlines in include/io.h, which filesystem and shell
 * code included freely.  That is why its sources could not be built for
 * any other architecture: `outb()` was effectively part of the kernel's
 * general vocabulary.  In HavenOS this header lives under
 * include/haven/arch/x86_64/ and may only be included from arch/x86_64/
 * and from drivers that are themselves x86-only; the rule is enforced by
 * scripts/check-portability.sh.
 */
#ifndef _HAVEN_ARCH_X86_64_IO_H
#define _HAVEN_ARCH_X86_64_IO_H

#include <haven/types.h>

static HAVEN_ALWAYS_INLINE void outb(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static HAVEN_ALWAYS_INLINE void outw(uint16_t port, uint16_t val)
{
	__asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static HAVEN_ALWAYS_INLINE void outl(uint16_t port, uint32_t val)
{
	__asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static HAVEN_ALWAYS_INLINE uint8_t inb(uint16_t port)
{
	uint8_t v;
	__asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
	return v;
}

static HAVEN_ALWAYS_INLINE uint16_t inw(uint16_t port)
{
	uint16_t v;
	__asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
	return v;
}

static HAVEN_ALWAYS_INLINE uint32_t inl(uint16_t port)
{
	uint32_t v;
	__asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
	return v;
}

/* One I/O-bus delay, for legacy devices (8259, 8042) that need settling
   time between consecutive accesses.  Port 0x80 is the POST diagnostic
   port and is harmless to write. */
static HAVEN_ALWAYS_INLINE void io_wait(void)
{
	outb(0x80, 0);
}

static HAVEN_ALWAYS_INLINE uint64_t rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static HAVEN_ALWAYS_INLINE void wrmsr(uint32_t msr, uint64_t val)
{
	__asm__ volatile("wrmsr"
	                 :: "c"(msr), "a"((uint32_t)val),
	                    "d"((uint32_t)(val >> 32)));
}

static HAVEN_ALWAYS_INLINE uint64_t rdtsc(void)
{
	uint32_t lo, hi;
	/* lfence first: RDTSC is not ordered against earlier loads, and
	   every use here is a measurement whose start must not float. */
	__asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

static HAVEN_ALWAYS_INLINE void cpuid_raw(uint32_t leaf, uint32_t subleaf,
                                          uint32_t *a, uint32_t *b,
                                          uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid"
	                 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
	                 : "a"(leaf), "c"(subleaf));
}

/* MMIO accessors.  volatile is load-bearing: without it the compiler is
   free to coalesce or elide device accesses. */
static HAVEN_ALWAYS_INLINE uint8_t mmio_read8(vaddr_t addr)
{
	return *(volatile uint8_t *)addr;
}
static HAVEN_ALWAYS_INLINE uint16_t mmio_read16(vaddr_t addr)
{
	return *(volatile uint16_t *)addr;
}
static HAVEN_ALWAYS_INLINE uint32_t mmio_read32(vaddr_t addr)
{
	return *(volatile uint32_t *)addr;
}
static HAVEN_ALWAYS_INLINE uint64_t mmio_read64(vaddr_t addr)
{
	return *(volatile uint64_t *)addr;
}
static HAVEN_ALWAYS_INLINE void mmio_write8(vaddr_t addr, uint8_t v)
{
	*(volatile uint8_t *)addr = v;
}
static HAVEN_ALWAYS_INLINE void mmio_write16(vaddr_t addr, uint16_t v)
{
	*(volatile uint16_t *)addr = v;
}
static HAVEN_ALWAYS_INLINE void mmio_write32(vaddr_t addr, uint32_t v)
{
	*(volatile uint32_t *)addr = v;
}
static HAVEN_ALWAYS_INLINE void mmio_write64(vaddr_t addr, uint64_t v)
{
	*(volatile uint64_t *)addr = v;
}

#endif /* _HAVEN_ARCH_X86_64_IO_H */
