/* SPDX-License-Identifier: MIT
 *
 * smp.c — starting the application processors.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS was single-processor by construction, so "Multi-core processor
 * support" in the HavenOS goals is entirely new work -- and it is the
 * requirement that shaped the rest of the kernel: per-CPU run queues,
 * per-CPU LAPIC timers, spinlocks, reentrant library routines and the
 * per-CPU GS base all exist because of this file.
 *
 * WHAT STARTING A CPU INVOLVES
 *   1. learn the local APIC ids from the ACPI MADT
 *   2. copy the real-mode trampoline to a page in the first megabyte,
 *      because the startup IPI carries its address as a single byte
 *   3. for each processor: allocate a stack, fill the handoff block,
 *      send INIT then two startup IPIs, and WAIT for the processor to
 *      say it arrived
 *   4. the processor climbs to long mode, installs its per-CPU area,
 *      loads its own GDT and the shared IDT, starts its LAPIC timer, and
 *      enters the scheduler
 *
 * WAITING, NOT ASSUMING
 * Step 3's wait is the part that matters.  A processor that fails to
 * start must be reported and skipped, not counted -- a kernel that
 * believes it has four CPUs and has one will schedule threads onto run
 * queues nothing is draining, and they will simply never run.  Booting
 * on three of four cores is far better than refusing to boot, and far
 * better than claiming four.
 */
#include <haven/smp.h>
#include <haven/arch.h>
#include <haven/mm.h>
#include <haven/sched.h>
#include <haven/time.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/string.h>
#include <haven/printf.h>
#include <haven/errno.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/gdt.h>
#include <haven/arch/x86_64/idt.h>
#include <haven/arch/x86_64/lapic.h>
#include <haven/arch/x86_64/acpi.h>
#include <haven/boot.h>
#include <haven/arch/x86_64/paging.h>
#include <haven/arch/x86_64/percpu.h>

/* Must match TRAMPOLINE_BASE in arch/x86_64/smp/trampoline.S.
   0x8000 is inside the first megabyte, which the PMM reserves in full,
   and page-aligned, which the startup vector requires. */
#define TRAMPOLINE_PHYS 0x8000

extern const uint8_t tramp_start[];
extern const uint8_t tramp_end[];
extern const uint64_t tramp_cr3;
extern const uint64_t tramp_stack;
extern const uint64_t tramp_entry;
extern const uint32_t tramp_started;
extern const uint32_t tramp_cpu_id;

/* How long to wait for a processor to report in.  Generous: a real
   processor answers in microseconds, and a virtual one under a loaded
   host can take milliseconds. */
#define AP_START_TIMEOUT_MS 500

static uint32_t started_count;

/* The copy of the trampoline the APs actually execute, reached through
   the direct map so the BSP can write its handoff block. */
static volatile uint8_t *tramp;

static volatile uint64_t *tramp_field(const void *field)
{
	size_t offset = (const uint8_t *)field - tramp_start;
	return (volatile uint64_t *)(tramp + offset);
}

static volatile uint32_t *tramp_field32(const void *field)
{
	size_t offset = (const uint8_t *)field - tramp_start;
	return (volatile uint32_t *)(tramp + offset);
}

/*
 * Entry point for a freshly started processor.
 *
 * Reached from the trampoline's 64-bit stage with the CPU index in the
 * first argument register and nothing else established.  The order below
 * is not interchangeable: nothing may call this_cpu() -- which klog()
 * does, through arch_cpu_id() -- until the per-CPU area is installed, so
 * the install comes before the first log line.
 */
void smp_ap_entry(uint32_t cpu_id)
{
	/* 1. The per-CPU area.  Must be first: the GS base is zero on a
	      processor out of reset, so this_cpu() before this point would
	      dereference a null pointer. */
	percpu_install(cpu_id);

	/* 2. This processor's own GDT and TSS, and the shared IDT.  Until
	      the IDT is loaded a fault here triple-faults with no output. */
	arch_cpu_init_tables_ap(cpu_id);

	/* 3. CPU features and protections, which are per-processor
	      registers: CR4's SMEP and SMAP bits and EFER's NX bit are not
	      inherited from the boot CPU. */
	arch_early_init();

	kinfo("smp: cpu %u online (APIC id %u)", cpu_id, lapic_id());

	/* 4. This processor's local APIC and its own scheduler tick.  The
	      tick must be per-CPU: the PIT is a single global device, so a
	      processor relying on it could never be preempted. */
	lapic_init();
	arch_syscall_init();

	/* 5. Join the scheduler.  The CPU gets its own idle thread and run
	      queue, then starts taking work. */
	sched_init_ap();
	arch_timer_init_ap(HZ);

	this_cpu()->online = true;
	__atomic_fetch_add(&started_count, 1, __ATOMIC_RELEASE);

	arch_irq_enable();

	/* 6. Idle.  The scheduler takes over from here: anything placed on
	      this CPU's run queue will preempt the idle loop at the next
	      tick. */
	for (;;) {
		sched_preempt_if_needed();
		arch_idle();
	}
}

int smp_start_secondary(void)
{
	/* Finding the processor list is this architecture's business, not
	   the generic kernel's: the MADT is an x86 firmware table and
	   kernel/main.c must not know it exists.
	   scripts/check-portability.sh caught an earlier version that had
	   kernel/main.c calling acpi_init() directly. */
	const struct acpi_info *acpi = acpi_init(&haven_boot_info);

	if (!lapic_available()) {
		kwarn("smp: no local APIC; running on the boot CPU only");
		return 0;
	}
	if (acpi->cpu_count <= 1) {
		kinfo("smp: firmware reports a single processor");
		return 0;
	}

	/* Copy the trampoline into low memory.  It is linked into the
	   kernel's .rodata at a higher-half address and must execute from a
	   page the startup vector can name. */
	size_t tramp_bytes = (size_t)(tramp_end - tramp_start);
	if (tramp_bytes > HAVEN_PAGE_SIZE) {
		kerr("smp: the trampoline is %zu bytes, over one page",
		     tramp_bytes);
		return 0;
	}

	tramp = (volatile uint8_t *)arch_phys_to_virt(TRAMPOLINE_PHYS);
	memcpy((void *)tramp, tramp_start, tramp_bytes);

	/*
	 * Identity-map the trampoline page, executable.
	 *
	 * This is required and its absence is spectacular.  An application
	 * processor runs the trampoline at its PHYSICAL address: the
	 * startup vector names a low page and the code executes there with
	 * paging off.  The moment it sets CR0.PG with the kernel's CR3, its
	 * instruction pointer -- still around 0x8000 -- has to resolve in
	 * the kernel page tables.
	 *
	 * vmm_init() drops the boot identity map on purpose, so that a
	 * kernel pointer bug producing a small address faults instead of
	 * reading physical memory.  The first version of this file
	 * inherited that and the AP triple-faulted the instant it enabled
	 * paging, which reset the whole machine: the boot log simply
	 * stopped after this message, with the BSP's 500 ms timeout never
	 * getting a chance to report anything.
	 *
	 * So the one page the trampoline occupies is mapped identically,
	 * and unmapped again once every processor has climbed into the
	 * higher half.
	 */
	int rc = vmm_map(TRAMPOLINE_PHYS, TRAMPOLINE_PHYS, HAVEN_PAGE_SIZE,
	                 VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC);
	if (rc != 0) {
		kerr("smp: cannot identity-map the trampoline page: %s",
		     haven_strerror(rc));
		return 0;
	}

	kinfo("smp: trampoline (%zu bytes) copied to physical 0x%x and "
	      "identity-mapped for the climb to long mode",
	      tramp_bytes, TRAMPOLINE_PHYS);

	/* Every processor shares the kernel address space, so they all use
	   the BSP's page-table root. */
	paddr_t cr3 = arch_pt_root(arch_pt_kernel());

	uint32_t bsp_apic = lapic_id();
	uint32_t brought_up = 0;

	percpu_set_count(acpi->cpu_count);

	for (uint32_t i = 0; i < acpi->cpu_count; i++) {
		uint8_t apic_id = acpi->apic_ids[i];

		if (apic_id == bsp_apic)
			continue;   /* that is us */

		if (brought_up + 1 >= HAVEN_MAX_CPUS)
			break;

		uint32_t cpu_id = brought_up + 1;

		vaddr_t stack = vmm_alloc_kstack();
		if (!stack) {
			kerr("smp: cannot allocate a stack for cpu %u", cpu_id);
			continue;
		}

		percpu_init_block(cpu_id, apic_id, false);

		/* Fill the handoff block, then clear the arrival flag LAST:
		   a processor that somehow started early must not see a stale
		   flag and have the BSP believe it arrived before its stack
		   was published. */
		*tramp_field(&tramp_cr3)     = cr3;
		*tramp_field(&tramp_stack)   = stack;
		*tramp_field(&tramp_entry)   = (uint64_t)(vaddr_t)smp_ap_entry;
		*tramp_field32(&tramp_cpu_id) = cpu_id;
		arch_wmb();
		*tramp_field32(&tramp_started) = 0;
		arch_wmb();

		lapic_send_init_sipi(apic_id, TRAMPOLINE_PHYS);

		/* Wait for the processor to say it arrived.  A timeout here
		   is a report, not a panic: three working cores beat
		   refusing to boot. */
		bool arrived = false;
		for (uint32_t ms = 0; ms < AP_START_TIMEOUT_MS; ms++) {
			arch_rmb();
			if (*tramp_field32(&tramp_started)) {
				arrived = true;
				break;
			}
			time_delay_us(1000);
		}

		if (!arrived) {
			kerr("smp: cpu %u (APIC id %u) did not start within "
			     "%u ms; continuing without it",
			     cpu_id, apic_id, AP_START_TIMEOUT_MS);
			vmm_free_kstack(stack);
			continue;
		}

		brought_up++;

		/* Serialise bring-up: two processors running this trampoline
		   at once would share one handoff block, and the second
		   would take the first's stack. */
		for (uint32_t ms = 0; ms < 100; ms++) {
			arch_rmb();
			if (__atomic_load_n(&started_count, __ATOMIC_ACQUIRE)
			    >= brought_up)
				break;
			time_delay_us(1000);
		}
	}

	/* Record what actually came up, not what firmware promised. */
	percpu_set_count(brought_up + 1);

	/* Every processor is in the higher half now, so the identity
	   mapping has done its job.  Removing it restores the property that
	   a small address faults. */
	vmm_unmap(TRAMPOLINE_PHYS, HAVEN_PAGE_SIZE);

	kinfo("smp: %u of %u processors online",
	      brought_up + 1, acpi->cpu_count);

	return (int)brought_up;
}

int arch_smp_start_cpus(void)
{
	return smp_start_secondary();
}
