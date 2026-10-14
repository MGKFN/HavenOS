/* SPDX-License-Identifier: MIT
 *
 * cpu.c — AArch64 CPU control.
 *
 * STATUS
 * This is a port SKELETON, not a working port.  The functions below fall
 * into two groups, and which group each is in is stated rather than left
 * to be discovered:
 *
 *   IMPLEMENTED   the operations whose AArch64 form is short and
 *                 unambiguous: barriers, interrupt masking through DAIF,
 *                 the idle and pause hints, and CPU identification from
 *                 MIDR_EL1.  These are correct as written.
 *
 *   NOT YET       everything that needs a translation-table walker, an
 *                 exception vector table, a context switcher or PSCI.
 *                 Each one panics with a message naming what is missing,
 *                 rather than returning a plausible wrong answer -- a
 *                 stub that returns 0 from arch_pt_translate() would
 *                 produce a kernel that boots and then corrupts memory.
 *
 * WHY THIS FILE EXISTS AT ALL
 * It is the evidence that the HAL works.  Every function here appears in
 * include/haven/arch.h and nowhere else: the generic kernel in kernel/,
 * fs/ and lib/ compiles against this file without a single change, which
 * is checked by scripts/check-portability.sh.  See
 * docs/PORTING_ARM64.md for the order the remaining work should be done
 * in and why.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/string.h>
#include <haven/arch/arm64/memlayout.h>

#define NOT_YET(what) \
	panic("arm64: " what " is not implemented; see docs/PORTING_ARM64.md")

/* ── System-register access ──────────────────────────────────────────── */

#define read_sysreg(reg) ({                                   \
	uint64_t _v;                                          \
	__asm__ volatile("mrs %0, " #reg : "=r"(_v));         \
	_v;                                                   \
})

#define write_sysreg(reg, val)                                \
	__asm__ volatile("msr " #reg ", %0" :: "r"((uint64_t)(val)))

/* ── Implemented: barriers ───────────────────────────────────────────
   AArch64's memory model is weakly ordered, so these are not optional
   the way x86's mostly are.  dsb sy is a full barrier; dmb ish orders
   accesses within the inner-shareable domain, which is what SMP needs. */

void arch_mb(void)  { __asm__ volatile("dsb sy"  ::: "memory"); }
void arch_rmb(void) { __asm__ volatile("dsb ld"  ::: "memory"); }
void arch_wmb(void) { __asm__ volatile("dsb st"  ::: "memory"); }

/* ── Implemented: interrupt state ────────────────────────────────────
   DAIF holds the Debug, SError, IRQ and FIQ masks.  Saving and restoring
   the whole register makes nesting correct for free, which is the
   property HavenDOS's bare cli/sti pairs lacked. */

irqflags_t arch_irq_save(void)
{
	uint64_t daif = read_sysreg(daif);
	__asm__ volatile("msr daifset, #2" ::: "memory");   /* mask IRQ */
	return daif;
}

void arch_irq_restore(irqflags_t flags)
{
	write_sysreg(daif, flags);
	__asm__ volatile("" ::: "memory");
}

void arch_irq_enable(void)
{
	__asm__ volatile("msr daifclr, #2" ::: "memory");
}

void arch_irq_disable(void)
{
	__asm__ volatile("msr daifset, #2" ::: "memory");
}

bool arch_irq_enabled(void)
{
	/* Bit 7 of DAIF is the IRQ mask: set means masked. */
	return (read_sysreg(daif) & (1u << 7)) == 0;
}

/* ── Implemented: idle and spin hints ────────────────────────────────── */

void arch_idle(void)
{
	/* wfi waits for an interrupt with interrupts unmasked at the
	   processor; unlike x86's sti/hlt pair there is no one-instruction
	   window to get wrong, because wfi wakes on a pending interrupt
	   even while it is masked. */
	__asm__ volatile("msr daifclr, #2");
	__asm__ volatile("wfi");
}

void arch_pause(void)
{
	/* yield is the spin-loop hint; on a single-threaded core it is a
	   no-op, and on one with SMT it releases issue slots. */
	__asm__ volatile("yield");
}

void arch_halt(void)
{
	for (;;) {
		__asm__ volatile("msr daifset, #3");   /* mask IRQ and FIQ */
		__asm__ volatile("wfi");
	}
}

/* ── Implemented: identification ─────────────────────────────────────── */

static char model_str[64];
static char vendor_str[32];

static const char *implementer_name(uint8_t id)
{
	switch (id) {
	case 0x41: return "ARM";
	case 0x42: return "Broadcom";
	case 0x43: return "Cavium";
	case 0x4E: return "NVIDIA";
	case 0x51: return "Qualcomm";
	case 0x61: return "Apple";
	case 0xC0: return "Ampere";
	default:   return "unknown";
	}
}

void arch_early_init(void)
{
	uint64_t midr = read_sysreg(midr_el1);

	uint8_t  implementer = (uint8_t)((midr >> 24) & 0xFF);
	uint16_t part        = (uint16_t)((midr >> 4) & 0xFFF);
	uint8_t  variant     = (uint8_t)((midr >> 20) & 0xF);
	uint8_t  revision    = (uint8_t)(midr & 0xF);

	strlcpy(vendor_str, implementer_name(implementer),
	        sizeof(vendor_str));
	snprintf(model_str, sizeof(model_str),
	         "%s part 0x%03x r%up%u", vendor_str, part, variant, revision);
}

void arch_init(void)
{
	kinfo("cpu: %s", model_str);
	kinfo("cpu: AArch64 port skeleton -- see docs/PORTING_ARM64.md for "
	      "what remains");
}

const char *arch_cpu_vendor(void) { return vendor_str; }
const char *arch_cpu_model(void)  { return model_str; }

uint32_t arch_cpu_id(void)
{
	/* TPIDR_EL1 is AArch64's equivalent of the x86_64 GS base: a
	   register the kernel owns, holding the per-CPU pointer.  Until the
	   per-CPU area exists, report the boot CPU. */
	uint64_t tpidr = read_sysreg(tpidr_el1);
	if (!tpidr)
		return 0;
	return *(const uint32_t *)tpidr;
}

void arch_reboot(void)
{
	/* A reset on AArch64 goes through PSCI (a firmware call) or a
	   platform-specific watchdog; there is no architectural equivalent
	   of the 8042 pulse.  Halting is the honest alternative to
	   pretending. */
	kerr("arm64: reset needs a PSCI SYSTEM_RESET call, which is not "
	     "implemented; halting instead");
	arch_halt();
}

/* ── Not yet implemented ─────────────────────────────────────────────
   Each of these panics rather than returning a plausible value.  A stub
   arch_pt_translate() that returned 0 would produce a kernel that boots
   and then silently corrupts memory, which is far worse than one that
   stops and says what is missing. */

void arch_cpu_init_tables(void)
{
	NOT_YET("the EL1 exception vector table (VBAR_EL1)");
}

void arch_syscall_init(void)
{
	NOT_YET("the SVC exception path");
}

void arch_irq_init(void)       { NOT_YET("the GIC driver"); }
void arch_irq_mask(uint32_t irq)   { (void)irq; NOT_YET("the GIC driver"); }
void arch_irq_unmask(uint32_t irq) { (void)irq; NOT_YET("the GIC driver"); }
void arch_irq_eoi(uint32_t irq)    { (void)irq; NOT_YET("the GIC driver"); }

void arch_timer_init(uint32_t hz)
{
	(void)hz;
	NOT_YET("the generic timer (CNTP_TVAL_EL0) interrupt");
}

uint32_t arch_smp_cpu_count(void) { return 1; }

int arch_smp_start_cpus(void)
{
	kwarn("arm64: bringing up secondary CPUs needs PSCI CPU_ON; running "
	      "on the boot CPU only");
	return 0;
}

void arch_smp_send_ipi(uint32_t cpu, uint32_t vector)
{
	(void)cpu;
	(void)vector;
	NOT_YET("GIC software-generated interrupts");
}
