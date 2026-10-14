/* SPDX-License-Identifier: MIT
 *
 * percpu.c — per-CPU state anchored in the GS base register.
 *
 * WHY GS AND NOT AN ARRAY INDEXED BY APIC ID
 * Reading the APIC ID costs a CPUID (serialising, ~100 cycles) or an MMIO
 * read, and the ID is sparse so it needs a translation table.  Both are
 * paid on every this_cpu() call, and this_cpu() is on the path of every
 * interrupt, every scheduler decision and every preempt_disable().
 *
 * The GS base is a register the kernel owns.  this_cpu() becomes one
 * segment-relative load with no serialisation and no table.  AArch64 has
 * the same facility in TPIDR_EL1, which is why include/haven/smp.h can
 * declare this_cpu() without saying how it is implemented.
 *
 * SWAPGS AND THE KERNEL_GS_BASE PAIR
 * While user code runs, GS belongs to user space (it is where thread-local
 * storage lives) and the kernel's value is parked in KERNEL_GS_BASE.  The
 * syscall and interrupt entry paths issue SWAPGS to exchange them, and the
 * exit paths swap back.  Getting that wrong in one direction leaks a
 * kernel pointer to user space; getting it wrong in the other makes the
 * kernel read user-controlled memory as its own per-CPU block.  Both
 * halves live in arch/x86_64/cpu/entry.S next to each other for exactly
 * that reason.
 */
#include <haven/smp.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/string.h>
#include <haven/arch/x86_64/io.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/arch/x86_64/percpu.h>

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

/* One block per possible CPU.  Statically allocated because the first
   this_cpu() call happens before any allocator exists, and because a
   per-CPU block must never move. */
static struct haven_cpu cpu_blocks[HAVEN_MAX_CPUS];
static uint32_t         cpu_count = 1;

/* Before smp_init_bsp() runs, GS base is whatever the firmware left, so
   dereferencing it would fault or return garbage.  Until then this_cpu()
   answers from this block, which lets klog() and panic() call
   arch_cpu_id() from the very first line of the boot. */
static bool percpu_ready;

struct haven_cpu *this_cpu(void)
{
	if (unlikely(!percpu_ready))
		return &cpu_blocks[0];

	struct haven_cpu *cpu;
	/* %gs:0 holds a self-pointer, so one load yields the block address
	   without needing to know its layout. */
	__asm__ volatile("movq %%gs:0, %0" : "=r"(cpu));
	return cpu;
}

struct haven_cpu *cpu_by_id(uint32_t cpu_id)
{
	if (cpu_id >= HAVEN_MAX_CPUS)
		return NULL;
	return &cpu_blocks[cpu_id];
}

uint32_t arch_cpu_id(void)
{
	return this_cpu()->cpu_id;
}

uint32_t smp_cpu_count(void)
{
	return cpu_count;
}

uint32_t smp_online_count(void)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < cpu_count; i++) {
		if (cpu_blocks[i].online)
			n++;
	}
	return n;
}

void percpu_set_count(uint32_t n)
{
	if (n == 0)
		n = 1;
	if (n > HAVEN_MAX_CPUS) {
		kwarn("smp: %u CPUs reported, using the first %u",
		      n, HAVEN_MAX_CPUS);
		n = HAVEN_MAX_CPUS;
	}
	cpu_count = n;
}

/* The self-pointer that this_cpu() loads lives at offset 0 of the block.
   struct haven_cpu's first member is cpu_id, a uint32_t, so the pointer
   cannot live inside the struct itself without disturbing the layout the
   generic kernel sees.  Instead each CPU gets an 8-byte anchor word
   immediately below its block, and GS base points at the anchor. */
struct percpu_anchor {
	struct haven_cpu *self;
	struct haven_cpu *block;
};

static struct percpu_anchor anchors[HAVEN_MAX_CPUS];

void percpu_install(uint32_t cpu_id)
{
	HAVEN_ASSERT(cpu_id < HAVEN_MAX_CPUS);

	anchors[cpu_id].self  = &cpu_blocks[cpu_id];
	anchors[cpu_id].block = &cpu_blocks[cpu_id];

	/* Both GS bases are set to the same value.  The kernel's own value
	   must be live in GS now, and KERNEL_GS_BASE must hold it too so
	   that the first SWAPGS on entry from user mode finds it. */
	wrmsr(MSR_GS_BASE,        (uint64_t)(vaddr_t)&anchors[cpu_id]);
	wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(vaddr_t)&anchors[cpu_id]);

	percpu_ready = true;
}

void percpu_init_block(uint32_t cpu_id, uint32_t hw_id, bool is_bsp)
{
	HAVEN_ASSERT(cpu_id < HAVEN_MAX_CPUS);

	struct haven_cpu *c = &cpu_blocks[cpu_id];
	memset(c, 0, sizeof(*c));

	c->cpu_id = cpu_id;
	c->hw_id  = hw_id;
	c->is_bsp = is_bsp;
	c->online = false;

	for (int i = 0; i < SCHED_PRIO_LEVELS; i++)
		list_init(&c->runqueue[i]);
}

void smp_init_bsp(void)
{
	percpu_init_block(0, 0, true);
	percpu_install(0);
	cpu_blocks[0].online = true;
	kinfo("smp: boot CPU per-CPU area installed at %p",
	      (void *)&cpu_blocks[0]);
}

/* ── Current-thread accessors used by the entry paths ───────────────── */

void arch_set_current_thread(void *thread)
{
	this_cpu()->current = (struct thread *)thread;
}

void *arch_get_current_thread(void)
{
	return this_cpu()->current;
}

void arch_set_kernel_stack(vaddr_t stack_top)
{
	struct haven_cpu *c = this_cpu();
	c->kernel_stack_top = stack_top;
	tss_set_kstack(c->cpu_id, stack_top);
}

/* ── Preemption nesting ─────────────────────────────────────────────── */

void preempt_disable(void)
{
	/* Interrupts off across the increment: the counter is per-CPU, so
	   another CPU cannot race, but an interrupt on *this* CPU that
	   itself disables preemption would. */
	irqflags_t f = arch_irq_save();
	this_cpu()->preempt_count++;
	arch_irq_restore(f);
}

void preempt_enable(void)
{
	irqflags_t f = arch_irq_save();
	struct haven_cpu *c = this_cpu();
	HAVEN_ASSERT_MSG(c->preempt_count > 0,
	                 "preempt_enable() without a matching disable");
	c->preempt_count--;
	arch_irq_restore(f);
}

bool preempt_enabled(void)
{
	return this_cpu()->preempt_count == 0;
}

bool in_interrupt(void)
{
	return this_cpu()->irq_nesting > 0;
}

uint32_t arch_smp_cpu_count(void)
{
	return cpu_count;
}
