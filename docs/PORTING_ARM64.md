# Porting HavenOS to AArch64

HavenOS reaches hardware through 62 functions declared in
`include/haven/arch.h` and nothing else. `arch/arm64/` implements 30 of
them; the other 32 panic with a message naming what is missing and a
pointer to this document. This guide says what each group needs, in the
order it has to be done, and what to watch out for at each step.

The skeleton is not a claim to a working port. It exists for two reasons:
to prove the HAL is real — the entire generic kernel compiles against
`arch/arm64/` unchanged — and to make the remaining work a list rather
than a research project.

## Why the skeleton is in the tree at all

HavenDOS could not be built for another architecture, and the reason is
worth stating because it is subtle: its `include/io.h` was inline x86
assembly, and filesystem code included it. No single decision made it
unportable; the absence of a boundary did, one include at a time.

A HAL that nothing tests decays the same way. So the HAL has two
enforcement mechanisms: `scripts/check-portability.sh`, which fails
`make check` if anything in `kernel/`, `fs/` or `lib/` contains inline
assembly, architecture-conditional code, or an include of an
architecture-private header; and this skeleton, which is a second
implementation that has to keep compiling. The checker has already caught
a real violation — `kernel/main.c` had grown an
`#include <haven/arch/x86_64/acpi.h>` and a direct `acpi_init()` call,
which moved into `smp_start_secondary()` where it belongs.

```
$ ./scripts/check-portability.sh
portability: checking that kernel fs lib contain no architecture-specific code
portability: checked 62 HAL entry points across 2 architectures
  PASS       the generic kernel contains no architecture-specific code
```

## Current state

| Area | Implemented | Remaining | File |
|---|---|---|---|
| Barriers, interrupt masking, idle, CPU identity | 19 | 8 | `cpu/cpu.c` |
| Timekeeping | 5 | 0 | `time/time.c` |
| Address translation and TLB maintenance | 6 | 13 | `mm/paging.c` |
| Context switching and user access | 0 | 11 | `cpu/context.c` |
| Boot path | — | all | `boot/README.md` |

(`arch_context_size()` is implemented but reports the size of a structure
nothing fills in yet, so it is counted with the remaining work.)

What is already real, and why it was worth doing first: the parts where
AArch64 is *simpler* than x86_64, so they document the shape of the port
without the bulk.

- **Barriers** map directly: `arch_mb()` is `dsb sy`, `arch_rmb()` is
  `dsb ld`, `arch_wmb()` is `dsb st`. x86_64's strong ordering lets it
  get away with less; AArch64 does not, which is why the generic kernel
  calls them explicitly even where x86 would not need to.
- **Interrupt masking** is `DAIF`, via `msr daifset, #2` and
  `msr daifclr, #2`, with `mrs x0, daif` to save.
- **Timekeeping is complete and needs no calibration at all.**
  `CNTFRQ_EL0` reports the counter frequency and `CNTVCT_EL0` is the
  counter. Compare this with the x86_64 path, which calibrates the TSC
  against PIT channel 2 and takes the median of three runs because no
  register will tell it the frequency. This is the clearest case where
  the second architecture is less work than the first.
- **The direct map arithmetic and TLB maintenance** — `arch_phys_to_virt`
  and back, page-size selection, `tlbi vaae1is` / `tlbi vmalle1is` with
  the `dsb`/`isb` either side.

## The address space

`include/haven/arch/arm64/memlayout.h` already fixes the layout, and it
differs from x86_64 in one structural way: AArch64 has **two** translation
base registers, so the kernel and user halves are selected by hardware
rather than by convention.

| Region | Base | Notes |
|---|---|---|
| User | `0x0000000000000000` | `TTBR0_EL1`, up to `0x0001000000000000` |
| Direct map (HHDM) | `0xFFFF000000000000` | `TTBR1_EL1` |
| Kernel heap arena | `0xFFFF400000000000` | 16 TiB |
| MMIO window | `0xFFFF500000000000` | Device-nGnRnE |
| Kernel image | `0xFFFF800000000000` | |

A canonical address on AArch64 is one whose top 16 bits are all 0 or all
1, which `is_canonical()` in that header checks by exactly that rule
rather than by x86_64's "bit 63:47 are copies of bit 47".

Because `TTBR0_EL1` holds only the user half, `arch_pt_switch()` writes
`TTBR0_EL1` alone and the kernel half never changes — so unlike x86_64,
where a process switch rewrites `CR3` and the kernel's entries have to be
present in every table, there is no kernel-entry duplication to maintain.
Set `TCR_EL1.A1` so the ASID comes from `TTBR1_EL1`, or you will
invalidate more than you mean to.

## The work, in order

### 1. The boot path — `arch/arm64/boot/`

Nothing of x86_64's `boot.S` carries over. There is no real mode, no
protected mode, no GDT, no Multiboot2 header; firmware enters an AArch64
kernel already in 64-bit mode, at EL1 or EL2, with the MMU off and a
device-tree or ACPI pointer in `x0`.

`arch/arm64/boot/README.md` has the sequence. The step that is easy to
miss and hard to debug is programming `MAIR_EL1` **before** building any
translation table, because a table entry's memory type is an index into
`MAIR_EL1`. Leave it unprogrammed and every mapping gets Device-nGnRnE:
the kernel appears to work, and runs about a hundred times slower.

Also: drop to EL1 if entered at EL2. Firmware entry level varies by
platform, and a kernel that assumes one will fail on the other.

What the boot path must produce is a filled-in `struct boot_info`
(`include/haven/boot.h`) and a call to `haven_kmain()`. On AArch64 the
memory map comes from the device tree's `/memory` nodes or from UEFI, not
from Multiboot2 — which is the whole reason `struct boot_info` exists as
a firmware-neutral structure rather than passing the Multiboot2 pointer
down.

### 2. Page tables — `mm/paging.c`, 13 functions

The structure is the same as x86_64 — four levels, 512 entries, 4 KiB
granule — so `arch/x86_64/mm/paging.c` is a usable model, including its
mixed-granularity direct map (1 GiB and 2 MiB blocks for RAM, 4 KiB for
the kernel image so per-section W^X is possible).

The descriptor bits are different and the differences are not cosmetic:

| Intent | x86_64 | AArch64 |
|---|---|---|
| Not executable | `NX` bit set | `UXN`/`PXN` bits set |
| Writable | `RW` bit set | `AP[2]` **clear** |
| User-accessible | `U/S` set | `AP[1]` set |
| Device memory | `PCD`/`PWT` | `AttrIndx` → a Device entry in `MAIR_EL1` |
| Present | bit 0 | bits 1:0 = `11` (table/page) or `01` (block) |

Note the inversion on writability: on AArch64 the bit means *read-only*.
A mechanical translation of the x86 bit handling produces a kernel whose
pages are all read-only, or all writable, depending on which way the
mistake goes. Write `arch_pt_map()` from the ARM ARM, not from
`paging.c`.

Also note that the Access Flag (`AF`) must be set on every entry you
create, or the first access faults.

`arch_pt_map()` must be idempotent for an identical mapping — mapping the
same VA to the same PA with the same permissions has to succeed rather
than return `-EEXIST`. That is not a theoretical nicety: firmware memory
maps contain overlapping regions, and on x86_64 the strict version
silently dropped three regions from the direct map, one of which was the
kernel image.

### 3. Exceptions and interrupts — `cpu/cpu.c`, 5 functions

AArch64 has no IDT. It has `VBAR_EL1` pointing at a **2 KiB-aligned
vector table of 16 entries at 128-byte spacing**, grouped by the exception
level and stack the exception came from. The 128-byte budget per entry is
enough to stack registers and branch, no more.

This is where the ports differ most. x86_64 generates 256 stubs that push
a vector number and jump to common code; AArch64 needs 16 entries that
decode `ESR_EL1` to find out what happened. The generic kernel does not
care — `include/haven/fault.h` describes a fault as an address plus
`VM_FAULT_WRITE`/`EXEC`/`USER`/`PRESENT` flags, deliberately so that no
x86 trap frame leaks into `kernel/mm` or `kernel/proc`. Decode `ESR_EL1`
into those flags and call `vm_handle_fault()`.

For device interrupts: the GIC. GICv2 and GICv3 differ enough to matter
(GICv3 moved the CPU interface into system registers), so read the device
tree and pick. `arch_irq_init/mask/unmask/eoi` are the four functions.
Generic kernel code identifies interrupts by line number only, so the
mapping from a GIC INTID to a HavenOS IRQ line is the architecture's
business — exactly as the 8259/APIC mapping is on x86_64.

The timer interrupt is `CNTP_TVAL_EL0` plus `CNTP_CTL_EL0`, delivered as
a per-CPU PPI through the GIC. `arch_timer_init(hz)` is the only function
the scheduler needs.

### 4. Context switching — `cpu/context.c`, 11 functions

`struct arch_context` for AArch64 is 12 words: `x19`–`x28` (the
callee-saved registers), the frame pointer `x29`, and `SP`. The link
register `x30` is not stored — the switch returns through `ret`, so the
return address is wherever the stack says.

The per-CPU pointer is `TPIDR_EL1`, which is the direct equivalent of
x86_64's `GS` base and `swapgs`, and simpler because there is no second
register to swap: EL0 gets `TPIDR_EL0` and never sees the kernel's.
`arch_set_current_thread()` and `arch_get_current_thread()` are a `msr`
and an `mrs`.

Entering user mode is `eret` with `SPSR_EL1` set to EL0t and `ELR_EL1` to
the entry point, where x86_64 pushes a fake interrupt frame and uses
`iretq`. System calls arrive as `svc #0` through the synchronous
exception vector, with the number in `x8` by Linux convention — there is
no `SYSCALL`/`SYSRET` MSR arrangement and therefore none of the GDT slot
ordering that `MSR_STAR` forces on x86_64.

**`arch_copy_from_user()` and friends need the fixup mechanism, not a
bounds check.** The x86_64 implementation records the faulting
instruction's address in a table between `__uaccess_fixup_start` and
`__uaccess_fixup_end`, and the page-fault handler consults it to resume at
a recovery label returning `-EFAULT`. The same approach is needed here —
`PAN` (Privileged Access Never, the equivalent of SMAP) makes a kernel
access to a user address fault, which is the point, and a port that
checks the address and then dereferences it has a race between the two.

### 5. SMP — `cpu/cpu.c`, 2 functions

x86_64 needs a trampoline at a physical address below 1 MiB because an AP
starts in real mode. AArch64 secondary CPUs are started through PSCI
`CPU_ON` with an entry point and a context ID, already in 64-bit mode, so
there is no trampoline and no mode climbing.

Do read `arch/x86_64/smp/smp.c`'s comment about the one bug that cost the
most time there: the AP executes the trampoline at its *physical*
address, so enabling paging with the kernel's page tables left its
instruction pointer unmapped and the machine triple-faulted and reset —
the log simply stopped. The AArch64 equivalent is enabling the MMU on a
secondary CPU with `SCTLR_EL1.M` before its own `SP` and `TTBR` values
are valid. The symptom will be the same: silence.

## Building it

```bash
make ARCH=arm64                      # with a cross-compiler
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

The build system selects the architecture directory from one variable and
discovers sources by directory, so `make ARCH=arm64` is a real target
rather than a fork of the build. What it will produce today is a kernel
that links and panics at the first unimplemented HAL function — which is
the intended behaviour, and more useful than a build error, because it
tells you exactly which function to write next:

```
PANIC: arm64: the direct map is not implemented yet
       see docs/PORTING_ARM64.md
```

The host test suite is architecture-neutral and already covers
`lib/string.c`, `lib/printf.c`, `include/haven/list.h`,
`kernel/cmdline.c`, `kernel/mm/pmm.c`, the VFS and the ELF loader — 842
checks that will pass on an AArch64 host without modification, because
they link the generic sources and stub the HAL.

## Testing the port

In order, because each step depends on the one before:

1. **It boots and logs.** A serial console (PL011 on most platforms) and
   the first `kinfo()` line. `make run ARCH=arm64` under
   `qemu-system-aarch64 -M virt`.
2. **`pmm_check()` passes.** `make check` runs the host suite against the
   same allocator; this proves the memory map was parsed correctly.
3. **`mm_startup_verify()` passes.** Stage 6 of `kernel/main.c` exercises
   all three allocators before anything depends on them.
4. **The in-kernel self-tests pass.** 82 checks, including four spinning
   threads that must share a CPU through timer preemption, and a 100 ms
   sleep that must take about 100 ms. These are the checks that catch a
   context switch returning to the wrong stack — which is the single most
   likely error in step 4 above, and the hardest to find by inspection.
5. **A user program runs.** `run /mnt/.../hello.elf` exercises page
   tables, `eret`, `svc`, and `copy_from_user` in one go.

An honest port report is the per-step list, not a claim that it works.

## Further reading

- [ARCHITECTURE.md](ARCHITECTURE.md) — what each subsystem expects of the HAL
- [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) — why the HAL is drawn where it is
- [API.md](API.md#havenarchh) — the 62 functions, with their contracts
- `arch/arm64/boot/README.md` — the boot sequence in detail
- `scripts/check-portability.sh` — the rule, enforced
