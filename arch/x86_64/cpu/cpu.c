/* SPDX-License-Identifier: MIT
 *
 * cpu.c — x86_64 CPU identification and control.
 */
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/string.h>
#include <haven/printf.h>
#include <haven/arch/x86_64/io.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/memlayout.h>

/* CPUID feature bits HavenOS cares about. */
#define CPUID_1_EDX_PAE      (1u << 6)
#define CPUID_1_EDX_APIC     (1u << 9)
#define CPUID_1_EDX_PGE      (1u << 13)
#define CPUID_1_EDX_PAT      (1u << 16)
#define CPUID_1_EDX_SSE2     (1u << 26)
#define CPUID_1_ECX_X2APIC   (1u << 21)
#define CPUID_1_ECX_TSCDEAD  (1u << 24)
#define CPUID_1_ECX_XSAVE    (1u << 26)
#define CPUID_7_EBX_SMEP     (1u << 7)
#define CPUID_7_EBX_SMAP     (1u << 20)
#define CPUID_EXT_EDX_NX     (1u << 20)
#define CPUID_EXT_EDX_1G     (1u << 26)
#define CPUID_EXT_EDX_RDTSCP (1u << 27)
#define CPUID_EXT_EDX_LM     (1u << 29)

#define MSR_EFER             0xC0000080
#define EFER_NXE             (1ull << 11)

#define CR4_PGE              (1ull << 7)
#define CR4_PCE              (1ull << 8)
#define CR4_OSFXSR           (1ull << 9)
#define CR4_OSXMMEXCPT       (1ull << 10)
#define CR4_UMIP             (1ull << 11)
#define CR4_SMEP             (1ull << 20)
#define CR4_SMAP             (1ull << 21)

#define CR0_WP               (1ull << 16)
#define CR0_NW               (1ull << 29)
#define CR0_CD               (1ull << 30)

struct cpu_features {
	bool nx, pge, pat, sse2, apic, x2apic, tsc_deadline;
	bool smep, smap, gib_pages, rdtscp, xsave;
	bool invariant_tsc;
	uint32_t max_leaf, max_ext_leaf;
};

static struct cpu_features features;
static char vendor_str[13];
static char model_str[49];

static inline uint64_t read_cr0(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr0, %0" : "=r"(v));
	return v;
}
static inline void write_cr0(uint64_t v)
{
	__asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory");
}
static inline uint64_t read_cr4(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr4, %0" : "=r"(v));
	return v;
}
static inline void write_cr4(uint64_t v)
{
	__asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory");
}

static void detect_features(void)
{
	uint32_t a, b, c, d;

	cpuid_raw(0, 0, &a, &b, &c, &d);
	features.max_leaf = a;
	/* The vendor string arrives as EBX, EDX, ECX — in that order, which
	   is not the register order and is a classic transcription slip. */
	memcpy(vendor_str + 0, &b, 4);
	memcpy(vendor_str + 4, &d, 4);
	memcpy(vendor_str + 8, &c, 4);
	vendor_str[12] = '\0';

	if (features.max_leaf >= 1) {
		cpuid_raw(1, 0, &a, &b, &c, &d);
		features.apic         = d & CPUID_1_EDX_APIC;
		features.pge          = d & CPUID_1_EDX_PGE;
		features.pat          = d & CPUID_1_EDX_PAT;
		features.sse2         = d & CPUID_1_EDX_SSE2;
		features.x2apic       = c & CPUID_1_ECX_X2APIC;
		features.tsc_deadline = c & CPUID_1_ECX_TSCDEAD;
		features.xsave        = c & CPUID_1_ECX_XSAVE;
	}

	if (features.max_leaf >= 7) {
		cpuid_raw(7, 0, &a, &b, &c, &d);
		features.smep = b & CPUID_7_EBX_SMEP;
		features.smap = b & CPUID_7_EBX_SMAP;
	}

	cpuid_raw(0x80000000, 0, &a, &b, &c, &d);
	features.max_ext_leaf = a;

	if (features.max_ext_leaf >= 0x80000001) {
		cpuid_raw(0x80000001, 0, &a, &b, &c, &d);
		features.nx        = d & CPUID_EXT_EDX_NX;
		features.gib_pages = d & CPUID_EXT_EDX_1G;
		features.rdtscp    = d & CPUID_EXT_EDX_RDTSCP;
	}

	/* Brand string, leaves 0x80000002..4. */
	model_str[0] = '\0';
	if (features.max_ext_leaf >= 0x80000004) {
		uint32_t regs[12];
		for (int i = 0; i < 3; i++)
			cpuid_raw(0x80000002 + (uint32_t)i, 0,
			          &regs[i * 4 + 0], &regs[i * 4 + 1],
			          &regs[i * 4 + 2], &regs[i * 4 + 3]);
		memcpy(model_str, regs, 48);
		model_str[48] = '\0';

		/* Intel pads the brand string with leading spaces. */
		char *p = model_str;
		while (*p == ' ')
			p++;
		if (p != model_str)
			memmove(model_str, p, strlen(p) + 1);
	}
	if (!model_str[0])
		strlcpy(model_str, vendor_str, sizeof(model_str));

	/* Invariant TSC: the TSC ticks at a constant rate regardless of
	   power state.  Without it, TSC-based timekeeping drifts whenever
	   the CPU changes frequency — which is why arch_time_ns() falls
	   back to the PIT when this bit is absent rather than reporting
	   confidently wrong times. */
	if (features.max_ext_leaf >= 0x80000007) {
		cpuid_raw(0x80000007, 0, &a, &b, &c, &d);
		features.invariant_tsc = d & (1u << 8);
	}
}

static void enable_protections(void)
{
	uint64_t cr4 = read_cr4();
	uint64_t cr0 = read_cr0();

	/* WP: make the kernel honour read-only page-table entries.  Without
	   it, ring 0 may write a read-only page, which would make
	   vmm_protect_kernel()'s read-only .text and .rodata decorative. */
	cr0 |= CR0_WP;
	cr0 &= ~(CR0_CD | CR0_NW);   /* caching on; firmware may leave it off */
	write_cr0(cr0);

	if (features.pge)
		cr4 |= CR4_PGE;

	/* SMEP: the CPU refuses to execute user pages while in ring 0.
	   SMAP: ring 0 may not even read user pages outside an explicit
	   STAC/CLAC window.  Both turn whole classes of privilege-escalation
	   bug into faults.  HavenDOS had no user mode at all, so this is new
	   ground rather than a port of anything. */
	if (features.smep)
		cr4 |= CR4_SMEP;
	if (features.smap)
		cr4 |= CR4_SMAP;

	/* SSE is enabled so the compiler may use it for memcpy-style moves
	   and so FPU state has somewhere to live; the kernel itself still
	   avoids floating point. */
	if (features.sse2)
		cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;

	write_cr4(cr4);

	/* NX must be on in EFER for the no-execute bit in a PTE to mean
	   anything.  boot.S already set it to make the early tables valid;
	   set it again here because an AP starts with a fresh EFER. */
	if (features.nx) {
		uint64_t efer = rdmsr(MSR_EFER);
		if (!(efer & EFER_NXE))
			wrmsr(MSR_EFER, efer | EFER_NXE);
	}
}

void arch_early_init(void)
{
	detect_features();
	enable_protections();
}

void arch_init(void)
{
	kinfo("cpu: %s", model_str);
	kinfo("cpu: vendor %s, cpuid leaves 0x%x / ext 0x%x",
	      vendor_str, features.max_leaf, features.max_ext_leaf);
	kinfo("cpu: nx=%d pge=%d pat=%d sse2=%d 1g-pages=%d",
	      features.nx, features.pge, features.pat, features.sse2,
	      features.gib_pages);
	kinfo("cpu: apic=%d x2apic=%d tsc-deadline=%d invariant-tsc=%d",
	      features.apic, features.x2apic, features.tsc_deadline,
	      features.invariant_tsc);
	kinfo("cpu: smep=%d smap=%d (user/kernel hardware separation)",
	      features.smep, features.smap);
}

bool arch_cpu_has_nx(void)        { return features.nx; }
bool arch_cpu_has_1g_pages(void)  { return features.gib_pages; }
bool arch_cpu_has_apic(void)      { return features.apic; }
bool arch_cpu_has_x2apic(void)    { return features.x2apic; }
bool arch_cpu_has_invariant_tsc(void) { return features.invariant_tsc; }
bool arch_cpu_has_tsc_deadline(void)  { return features.tsc_deadline; }

const char *arch_cpu_vendor(void) { return vendor_str; }
const char *arch_cpu_model(void)  { return model_str; }

void arch_halt(void)
{
	for (;;) {
		__asm__ volatile("cli; hlt");
	}
}

void arch_reboot(void)
{
	/* Try the 8042 keyboard-controller reset line first: it is the most
	   widely supported method and is what HavenDOS used.  Then the
	   0xCF9 PCI reset-control register, then a deliberate triple fault
	   by loading a null IDT — which no machine survives. */
	for (int attempt = 0; attempt < 3; attempt++) {
		/* Drain the 8042 input buffer, then pulse the reset line. */
		for (int spin = 0; spin < 10000; spin++) {
			if (!(inb(0x64) & 0x02))
				break;
			io_wait();
		}
		outb(0x64, 0xFE);

		for (int spin = 0; spin < 100000; spin++)
			io_wait();

		outb(0xCF9, 0x02);   /* arm */
		outb(0xCF9, 0x06);   /* full reset */

		for (int spin = 0; spin < 100000; spin++)
			io_wait();
	}

	struct HAVEN_PACKED { uint16_t limit; uint64_t base; } null_idt = { 0, 0 };
	__asm__ volatile("lidt %0; int3" :: "m"(null_idt));

	arch_halt();
}

void arch_idle(void)
{
	/* sti then hlt, as one atomic pair: if an interrupt arrived between
	   the two instructions, hlt would sleep until the *next* one, which
	   on a system whose only timer tick was that interrupt means
	   sleeping forever.  The x86 architecture guarantees interrupts stay
	   blocked for exactly one instruction after sti, which makes this
	   sequence safe. */
	__asm__ volatile("sti; hlt");
}

void arch_pause(void)
{
	__asm__ volatile("pause");
}

irqflags_t arch_irq_save(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
	return flags;
}

void arch_irq_restore(irqflags_t flags)
{
	/* Restore only the interrupt flag's previous value.  Writing the
	   whole saved RFLAGS back would also restore arithmetic flags and,
	   worse, could clear IOPL or set TF from stale state. */
	if (flags & (1ull << 9))
		__asm__ volatile("sti" ::: "memory");
	else
		__asm__ volatile("cli" ::: "memory");
}

void arch_irq_enable(void)
{
	__asm__ volatile("sti" ::: "memory");
}

void arch_irq_disable(void)
{
	__asm__ volatile("cli" ::: "memory");
}

bool arch_irq_enabled(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq; popq %0" : "=r"(flags));
	return (flags & (1ull << 9)) != 0;
}

void arch_mb(void)  { __asm__ volatile("mfence" ::: "memory"); }
void arch_rmb(void) { __asm__ volatile("lfence" ::: "memory"); }
void arch_wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
