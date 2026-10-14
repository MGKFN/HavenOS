/* SPDX-License-Identifier: MIT
 *
 * lapic.c — the Local APIC, and the scheduler's timer.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS used the 8253 PIT for everything, and its own source documents
 * what that cost: the tick-to-microsecond conversion truncated to zero and
 * destroyed ticks, so sleep_ms() slept unboundedly longer than asked, and
 * the intermediate product overflowed 32 bits.  Both were fixed with
 * fixed-point arithmetic, but the deeper problem was structural — the PIT
 * is a single global device, so on a multi-processor system there is one
 * timer for every CPU and no way to give each one its own tick.
 *
 * The Local APIC solves that: every CPU has one, built into the core.
 * Each gets its own scheduler tick with no contention and no shared
 * state, which is a prerequisite for per-CPU run queues.
 *
 * CALIBRATION
 * The LAPIC timer counts at the bus or core crystal frequency, which is
 * not discoverable on older parts.  So it is measured against the PIT
 * once, the same way the TSC is.  Measuring beats assuming: a hard-coded
 * divisor is how a scheduler ends up running at 40 Hz on one machine and
 * 4000 Hz on another.
 */
#include <haven/arch.h>
#include <haven/mm.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/smp.h>
#include <haven/arch/x86_64/io.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/idt.h>
#include <haven/arch/x86_64/lapic.h>

#define MSR_APIC_BASE        0x1B
#define APIC_BASE_ENABLE     (1ULL << 11)
#define APIC_BASE_X2APIC     (1ULL << 10)
#define APIC_BASE_BSP        (1ULL << 8)
#define APIC_BASE_ADDR_MASK  0xFFFFFFFFFFFFF000ULL

/* Register offsets, in bytes from the APIC base. */
#define APIC_ID              0x020
#define APIC_VERSION         0x030
#define APIC_TPR             0x080   /* task priority */
#define APIC_EOI             0x0B0
#define APIC_SPURIOUS        0x0F0
#define APIC_ESR             0x280   /* error status */
#define APIC_ICR_LOW         0x300   /* interrupt command */
#define APIC_ICR_HIGH        0x310
#define APIC_LVT_TIMER       0x320
#define APIC_LVT_THERMAL     0x330
#define APIC_LVT_PERF        0x340
#define APIC_LVT_LINT0       0x350
#define APIC_LVT_LINT1       0x360
#define APIC_LVT_ERROR       0x370
#define APIC_TIMER_INITCNT   0x380
#define APIC_TIMER_CURRCNT   0x390
#define APIC_TIMER_DIV       0x3E0

#define APIC_SPURIOUS_ENABLE (1u << 8)
#define LVT_MASKED           (1u << 16)
#define LVT_TIMER_PERIODIC   (1u << 17)
#define LVT_TIMER_TSCDEADLINE (2u << 17)
#define LVT_DELIVERY_NMI     (4u << 8)
#define LVT_DELIVERY_EXTINT  (7u << 8)

#define APIC_DIV_16          0x3

#define ICR_DELIVERY_INIT    (5u << 8)
#define ICR_DELIVERY_STARTUP (6u << 8)
#define ICR_LEVEL_ASSERT     (1u << 14)
#define ICR_TRIGGER_LEVEL    (1u << 15)
#define ICR_DEST_SELF        (1u << 18)
#define ICR_PENDING          (1u << 12)

static vaddr_t  lapic_base;
static bool     lapic_enabled;
static uint32_t lapic_timer_ticks_per_hz;

static inline uint32_t lapic_read(uint32_t reg)
{
	return mmio_read32(lapic_base + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t value)
{
	mmio_write32(lapic_base + reg, value);
}

uint32_t lapic_id(void)
{
	if (!lapic_enabled)
		return 0;
	/* The ID is in the top 8 bits in xAPIC mode. */
	return lapic_read(APIC_ID) >> 24;
}

void lapic_eoi(void)
{
	if (lapic_enabled)
		lapic_write(APIC_EOI, 0);
}

bool lapic_available(void)
{
	return lapic_enabled;
}

bool lapic_init(void)
{
	if (!arch_cpu_has_apic()) {
		kwarn("lapic: this CPU reports no local APIC; staying on the "
		      "8259 PIC (single-processor only)");
		return false;
	}

	uint64_t base_msr = rdmsr(MSR_APIC_BASE);
	paddr_t  phys     = base_msr & APIC_BASE_ADDR_MASK;

	if (!phys) {
		kerr("lapic: APIC base MSR reports address 0");
		return false;
	}

	/* The APIC must be mapped uncached.  A cached mapping would let the
	   CPU serve a stale value for a status register — and the EOI
	   register is write-only with a side effect, which caching would
	   coalesce away entirely. */
	if (!lapic_base) {
		lapic_base = vmm_map_mmio(phys, 0x1000);
		if (!lapic_base) {
			kerr("lapic: cannot map the APIC at 0x%llx",
			     (unsigned long long)phys);
			return false;
		}
	}

	/* Globally enable it, in case firmware left it off. */
	wrmsr(MSR_APIC_BASE, base_msr | APIC_BASE_ENABLE);

	/* The spurious-interrupt vector register also carries the software
	   enable bit.  Writing the vector without the enable bit leaves the
	   APIC off, which looks exactly like an APIC that is not there. */
	lapic_write(APIC_SPURIOUS, VEC_LAPIC_SPURIOUS | APIC_SPURIOUS_ENABLE);

	/* Accept every priority.  A non-zero TPR silently blocks
	   lower-priority interrupts, which presents as a device whose
	   handler is never called. */
	lapic_write(APIC_TPR, 0);

	/* Mask the local vector table entries we do not use yet, so a
	   thermal or performance event cannot arrive on a vector with no
	   handler. */
	lapic_write(APIC_LVT_TIMER,   LVT_MASKED);
	lapic_write(APIC_LVT_THERMAL, LVT_MASKED);
	lapic_write(APIC_LVT_PERF,    LVT_MASKED);
	lapic_write(APIC_LVT_ERROR,   VEC_LAPIC_ERROR);

	/* Virtual-wire mode on the boot CPU.
	 *
	 * HavenOS uses the LAPIC for the per-CPU scheduler tick and for
	 * IPIs, but device interrupts still come from the 8259 pair -- an
	 * I/O APIC is not yet implemented, and a half-configured I/O APIC
	 * is worse than a working PIC.  For the PIC's output to reach the
	 * CPU once the LAPIC is enabled, LINT0 must be configured for
	 * ExtINT delivery: the LAPIC then passes the PIC's interrupt and
	 * vector straight through, and the acknowledgement goes to the PIC
	 * alone.  Without this, enabling the LAPIC silently stops every
	 * legacy device interrupt -- the keyboard simply goes dead.
	 *
	 * LINT1 is wired to NMI by convention on PC hardware.
	 *
	 * Only the BSP does this.  Two CPUs accepting ExtINT would both
	 * take the same PIC interrupt. */
	if (base_msr & APIC_BASE_BSP) {
		lapic_write(APIC_LVT_LINT0, LVT_DELIVERY_EXTINT);
		lapic_write(APIC_LVT_LINT1, LVT_DELIVERY_NMI);
	} else {
		lapic_write(APIC_LVT_LINT0, LVT_MASKED);
		lapic_write(APIC_LVT_LINT1, LVT_MASKED);
	}

	/* Clear a stale error, then read again: the ESR latches and needs a
	   write before a read returns current state. */
	lapic_write(APIC_ESR, 0);
	(void)lapic_read(APIC_ESR);

	lapic_enabled = true;

	uint32_t version = lapic_read(APIC_VERSION);
	kinfo("lapic: id %u, version 0x%x, %u LVT entries, mapped at 0x%llx",
	      lapic_id(), version & 0xFF, ((version >> 16) & 0xFF) + 1,
	      (unsigned long long)phys);

	return true;
}

/*
 * Calibrate the LAPIC timer against the PIT.
 *
 * Programme the APIC's counter to its maximum, wait a known interval
 * measured by the PIT, and see how far it got.  That gives the APIC's
 * tick rate without needing to know the bus frequency — which CPUID
 * reports only on relatively recent parts.
 */
static uint32_t calibrate_lapic_timer(void)
{
	const uint32_t ms = 20;

	lapic_write(APIC_TIMER_DIV, APIC_DIV_16);
	lapic_write(APIC_LVT_TIMER, LVT_MASKED);   /* count, do not interrupt */
	lapic_write(APIC_TIMER_INITCNT, 0xFFFFFFFF);

	arch_delay_us((uint64_t)ms * 1000);

	uint32_t remaining = lapic_read(APIC_TIMER_CURRCNT);
	lapic_write(APIC_TIMER_INITCNT, 0);

	if (remaining == 0 || remaining == 0xFFFFFFFF) {
		/* Either the counter never moved or it wrapped.  Both mean
		   the measurement is useless, and using it would set a
		   wildly wrong tick rate. */
		kerr("lapic: timer calibration produced an unusable result "
		     "(counter %s)", remaining ? "did not move" : "wrapped");
		return 0;
	}

	uint64_t elapsed = 0xFFFFFFFFULL - remaining;
	uint64_t per_sec = (elapsed * 1000ULL) / ms;

	kinfo("lapic: timer runs at %llu.%03llu MHz (divisor 16, calibrated "
	      "against the PIT over %u ms)",
	      (unsigned long long)(per_sec / 1000000),
	      (unsigned long long)((per_sec / 1000) % 1000), ms);

	return (uint32_t)per_sec;
}

void lapic_timer_init(uint32_t hz)
{
	if (!lapic_enabled)
		return;
	if (hz == 0)
		hz = 100;

	/* Calibrate once on the boot CPU; the rate is a property of the
	   bus, so application processors reuse the figure rather than each
	   spending 20 ms measuring it. */
	if (!lapic_timer_ticks_per_hz) {
		uint32_t per_sec = calibrate_lapic_timer();
		if (!per_sec) {
			kerr("lapic: cannot start the timer without a "
			     "calibration; the scheduler will not be preempted "
			     "on this CPU");
			return;
		}
		lapic_timer_ticks_per_hz = per_sec / hz;
		if (lapic_timer_ticks_per_hz == 0)
			lapic_timer_ticks_per_hz = 1;
	}

	lapic_write(APIC_TIMER_DIV, APIC_DIV_16);
	lapic_write(APIC_LVT_TIMER, VEC_LAPIC_TIMER | LVT_TIMER_PERIODIC);
	lapic_write(APIC_TIMER_INITCNT, lapic_timer_ticks_per_hz);

	kinfo("lapic: cpu %u scheduler tick at %u Hz (%u timer counts)",
	      arch_cpu_id(), hz, lapic_timer_ticks_per_hz);
}

void lapic_timer_stop(void)
{
	if (lapic_enabled)
		lapic_write(APIC_LVT_TIMER, LVT_MASKED);
}

/* ── Inter-processor interrupts ──────────────────────────────────────── */

static bool icr_wait_idle(void)
{
	/* The delivery-status bit clears when the previous IPI has been
	   accepted.  Writing a new command before then loses it. */
	for (int spin = 0; spin < 1000000; spin++) {
		if (!(lapic_read(APIC_ICR_LOW) & ICR_PENDING))
			return true;
		arch_pause();
	}
	kerr("lapic: the interrupt command register never went idle");
	return false;
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector)
{
	if (!lapic_enabled)
		return;
	if (!icr_wait_idle())
		return;

	lapic_write(APIC_ICR_HIGH, apic_id << 24);
	lapic_write(APIC_ICR_LOW, vector | ICR_LEVEL_ASSERT);
}

/*
 * The INIT-SIPI-SIPI sequence that starts an application processor.
 *
 * The trampoline address must be below 1 MiB and page aligned, because
 * the startup IPI carries it as a single byte: the CPU begins executing
 * in real mode at (vector << 12).  That is why arch/x86_64/smp reserves
 * a page in low memory rather than allocating one wherever.
 *
 * The delays are from the Intel MP specification.  They are not optional
 * padding: the target CPU needs time to come out of INIT before it will
 * accept the startup message, and sending the second SIPI too early is a
 * well-known way to get a processor that never starts.
 */
void lapic_send_init_sipi(uint32_t apic_id, paddr_t trampoline)
{
	if (!lapic_enabled)
		return;

	uint32_t vector = (uint32_t)(trampoline >> 12);

	/* Assert INIT. */
	if (!icr_wait_idle())
		return;
	lapic_write(APIC_ICR_HIGH, apic_id << 24);
	lapic_write(APIC_ICR_LOW, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT |
	                          ICR_TRIGGER_LEVEL);
	arch_delay_us(10000);

	/* De-assert INIT. */
	if (!icr_wait_idle())
		return;
	lapic_write(APIC_ICR_HIGH, apic_id << 24);
	lapic_write(APIC_ICR_LOW, ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL);
	arch_delay_us(10000);

	/* Two startup IPIs.  The second is a retry the specification
	   requires, because the first can be missed on some parts. */
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!icr_wait_idle())
			return;
		lapic_write(APIC_ESR, 0);
		lapic_write(APIC_ICR_HIGH, apic_id << 24);
		lapic_write(APIC_ICR_LOW, ICR_DELIVERY_STARTUP | vector |
		                          ICR_LEVEL_ASSERT);
		arch_delay_us(200);
	}
}
