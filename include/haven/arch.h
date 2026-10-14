/* SPDX-License-Identifier: MIT
 *
 * arch.h — the hardware abstraction layer HavenOS is portable through.
 *
 * DESIGN NOTE (HavenDOS → HavenOS, and the ARM64 requirement)
 * HavenDOS had no HAL.  `outb()` was included from include/io.h by
 * filesystem code, the scheduler did not exist, and kernel/main.c spoke
 * directly to the 8259 PIC and the 8253 PIT.  Porting it to another
 * architecture would have meant rewriting every file that touched
 * hardware, which is most of them.
 *
 * HavenOS declares the complete set of operations the generic kernel needs
 * from a CPU architecture here, and nothing else.  Every function below is
 * implemented once per architecture under arch/<arch>/.  The rule the tree
 * enforces is simple and checked by scripts/check-portability.sh:
 *
 *     Nothing outside arch/<arch>/ and drivers/ may contain inline
 *     assembly or architecture-conditional code.
 *
 * An ARM64 port therefore consists of implementing this header — see
 * docs/PORTING_ARM64.md and the skeleton in arch/arm64/ — without
 * touching kernel/, fs/ or lib/ at all.
 */
#ifndef _HAVEN_ARCH_H
#define _HAVEN_ARCH_H

#include <haven/types.h>
#include <haven/boot.h>

/* ───────────────────────── CPU control ───────────────────────── */

void     arch_early_init(void);        /* before any allocation */
void     arch_init(void);              /* after MM is up */
void     arch_halt(void) HAVEN_NORETURN;
void     arch_reboot(void) HAVEN_NORETURN;
void     arch_idle(void);              /* wait for the next interrupt */
void     arch_pause(void);             /* spin-loop yield hint */
uint32_t arch_cpu_id(void);            /* logical ID of the running CPU */
const char *arch_cpu_vendor(void);
const char *arch_cpu_model(void);

/* ──────────────────────── Interrupt state ───────────────────────
   arch_irq_save()/arch_irq_restore() nest correctly: save returns the
   previous state as an opaque word and restore puts exactly that back.
   HavenDOS used bare `cli`/`sti` pairs, so any nesting re-enabled
   interrupts at the end of the inner region — the exact failure mode that
   makes a spinlock not actually a lock. */
typedef uint64_t irqflags_t;

irqflags_t arch_irq_save(void);
void       arch_irq_restore(irqflags_t flags);
void       arch_irq_enable(void);
void       arch_irq_disable(void);
bool       arch_irq_enabled(void);

/* ─────────────────────── Memory barriers ─────────────────────── */

void arch_mb(void);    /* full barrier */
void arch_rmb(void);   /* load barrier */
void arch_wmb(void);   /* store barrier */

/* ───────────────────── Paging / address space ─────────────────── */

/* Page-table permission and attribute bits, in architecture-neutral form.
   arch/<arch>/mm translates these to hardware bits; nothing in kernel/mm
   knows what a PTE looks like. */
#define VM_PROT_READ    (1U << 0)
#define VM_PROT_WRITE   (1U << 1)
#define VM_PROT_EXEC    (1U << 2)
#define VM_PROT_USER    (1U << 3)   /* reachable from CPL 3 / EL0 */
#define VM_ATTR_NOCACHE (1U << 4)   /* device / MMIO memory */
#define VM_ATTR_WRITETHRU (1U << 5)
#define VM_ATTR_GLOBAL  (1U << 6)   /* not flushed on address-space switch */

/* Opaque per-address-space page-table root. */
struct arch_pagetable;

struct arch_pagetable *arch_pt_create(void);
void  arch_pt_destroy(struct arch_pagetable *pt);
struct arch_pagetable *arch_pt_kernel(void);
void  arch_pt_switch(struct arch_pagetable *pt);

/* Map/unmap a single page.  Returns 0 or -HAVEN_E*.
   arch_pt_map() allocates intermediate tables through pmm_alloc_page(). */
int   arch_pt_map(struct arch_pagetable *pt, vaddr_t va, paddr_t pa,
                  size_t len, uint32_t prot);
int   arch_pt_unmap(struct arch_pagetable *pt, vaddr_t va, size_t len);
int   arch_pt_protect(struct arch_pagetable *pt, vaddr_t va, size_t len,
                      uint32_t prot);
paddr_t arch_pt_translate(struct arch_pagetable *pt, vaddr_t va);

void  arch_tlb_flush_page(vaddr_t va);
void  arch_tlb_flush_all(void);

/* The direct map of all physical RAM.  Both directions are pure
   arithmetic, which is what makes the PMM architecture-neutral. */
vaddr_t arch_phys_to_virt(paddr_t pa);
paddr_t arch_virt_to_phys(vaddr_t va);

/* Largest page size this architecture supports for the given span, so the
   VMM can use 2 MiB / 1 GiB (or ARM64 block) mappings for the direct map. */
size_t  arch_page_size_for(vaddr_t va, paddr_t pa, size_t len);

/* ────────────────────── Context switching ───────────────────── */

struct arch_context;   /* opaque, defined per architecture */

size_t arch_context_size(void);

/* Build a fresh kernel-thread context that will begin executing
   entry(arg) on the given stack. */
void arch_context_init_kernel(struct arch_context *ctx, vaddr_t stack_top,
                              void (*entry)(void *), void *arg);

/* Build a context that enters user mode at `entry` with stack `ustack`. */
void arch_context_init_user(struct arch_context *ctx, vaddr_t kstack_top,
                            vaddr_t entry, vaddr_t ustack);

/* Save `from`, load `to`.  Returns in the `from` context when it is next
   scheduled.  Implemented in assembly. */
void arch_context_switch(struct arch_context *from, struct arch_context *to);

/* First entry into a context that has never run. */
void arch_context_start(struct arch_context *to) HAVEN_NORETURN;

/* Per-CPU pointer the entry/exit paths use to find the current thread. */
void  arch_set_current_thread(void *thread);
void *arch_get_current_thread(void);
void  arch_set_kernel_stack(vaddr_t stack_top);

/* ───────────────────────── Interrupts ──────────────────────────
   The generic layer in kernel/irq owns the handler table; the arch layer
   only routes a hardware vector to irq_dispatch(). */

#define ARCH_IRQ_MAX 256

void arch_irq_init(void);
void arch_irq_unmask(uint32_t irq);
void arch_irq_mask(uint32_t irq);
void arch_irq_eoi(uint32_t irq);

/* ───────────────────────── Timekeeping ───────────────────────── */

void     arch_timer_init(uint32_t hz);     /* periodic scheduler tick */
nstime_t arch_time_ns(void);               /* monotonic since boot */
void     arch_delay_us(uint64_t us);       /* calibrated busy-wait */

/* ──────────────────────────── SMP ─────────────────────────────── */

uint32_t arch_smp_cpu_count(void);
int      arch_smp_start_cpus(void);        /* bring up application CPUs */
void     arch_smp_send_ipi(uint32_t cpu, uint32_t vector);

/* ─────────────────────── Syscall entry ───────────────────────── */

void arch_syscall_init(void);

/* ───────────────── User/kernel memory boundary ────────────────
   Copying across the privilege boundary must be fault-tolerant: a bad
   user pointer returns -HAVEN_EFAULT instead of panicking the kernel.
   HavenDOS had no user mode and so no equivalent; this is the primitive
   that makes "secure user and kernel separation" real rather than
   nominal. */
int arch_copy_from_user(void *dst, const void *user_src, size_t len);
int arch_copy_to_user(void *user_dst, const void *src, size_t len);
int arch_strncpy_from_user(char *dst, const char *user_src, size_t len);

#endif /* _HAVEN_ARCH_H */
