# HavenOS architecture

HavenOS is a 64-bit, preemptively multitasking, virtual-memory kernel for
x86_64, with an AArch64 port skeleton. It is derived from
[HavenDOS](HAVENDOS_HERITAGE.md), a 32-bit single-tasking kernel with no
paging and no processes.

This document describes the shape of the system. For *why* each shape was
chosen — including the alternatives rejected — see
[DESIGN_DECISIONS.md](DESIGN_DECISIONS.md).

---

## 1. The layering rule

```
                        ┌───────────────────────────────┐
   ring 3               │   user processes (ELF64)      │
                        └───────────────┬───────────────┘
                          SYSCALL / SYSRET, 30 numbered calls
   ─────────────────────────────────────┼─────────────────────────────
                        ┌───────────────┴───────────────┐
   ring 0, portable     │  kernel/   fs/   lib/         │
                        │  • memory management          │
                        │  • scheduler, threads         │
                        │  • VFS, ramfs, devfs, FAT     │
                        │  • processes, ELF loader      │
                        │  • syscall dispatch           │
                        └───────────────┬───────────────┘
                              include/haven/arch.h
                          (62 functions, one per capability)
                        ┌───────────────┴───────────────┐
   ring 0, per-arch     │  arch/x86_64/   arch/arm64/   │
                        └───────────────────────────────┘
                        ┌───────────────────────────────┐
   ring 0, per-device   │  drivers/                     │
                        └───────────────────────────────┘
```

**The rule:** nothing in `kernel/`, `fs/` or `lib/` may contain inline
assembly, an architecture conditional, an architecture-private header, or
a port-I/O call. `scripts/check-portability.sh` enforces it on every
`make check`, and it has already caught one violation that was committed
by mistake.

This is the single most important structural difference from HavenDOS,
which had no hardware abstraction at all: its `include/io.h` — raw `outb`
and `inb` — was included directly by filesystem code, which is why its
sources could not be built for any other architecture.

---

## 2. Boot

```
firmware (BIOS or UEFI)
   │
   ├── GRUB 2, from one ISO carrying both an El Torito BIOS image
   │   and an EFI System Partition with bootx64.efi
   │
   └── Multiboot2 handover ──► arch/x86_64/boot/boot.S
                                 │  32-bit protected mode, paging off
                                 │
                                 ├── 1. bank EAX/EBX (magic, info pointer)
                                 ├── 2. prove long mode exists (CPUID)
                                 ├── 3. build boot page tables:
                                 │      4 GiB identity  (to keep executing)
                                 │      4 GiB direct map (to reach memory)
                                 │      1 GiB kernel window
                                 ├── 4. CR4.PAE → CR3 → EFER.LME → CR0.PG
                                 ├── 5. 64-bit GDT, far jump, then an
                                 │      absolute jump to the higher half
                                 └── 6. x86_boot_main()
                                          │
                                          ▼
                                 arch/x86_64/boot/main.c
                                   serial console first, so every later
                                   failure is reportable
                                          │
                                          ▼
                                 haven_kmain()  ← portable from here on
```

The kernel links at `0xFFFFFFFF80200000` and loads at physical
`0x200000`. GRUB is told the 32-bit physical entry point through a
Multiboot2 *entry address tag*; without it GRUB would jump to the ELF's
`e_entry`, which is a higher-half address it cannot reach with paging off.

`tools/verify_multiboot2.py` and `tools/verify_image.py` run on every
link and check the invariants a successful link does not guarantee — the
header checksum, the entry point landing inside a `PT_LOAD` segment, the
image loading below 4 GiB, and the bounds symbols the PMM depends on
existing and being correctly ordered.

### Numbered boot stages

`haven_kmain()` is written as fifteen numbered stages, each stating what
it provides and what it requires. That is a direct response to HavenDOS,
whose `kernel_main()` grew to a flat sequence of initialisation calls
whose order nobody could see — and which, as its own source comments
record, drifted wrong: `net_init()` ran *after* the screen that reported
whether a network card had been found, so every boot printed "NIC .....
Not detected" regardless of the hardware.

| Stage | Provides | Requires |
|---|---|---|
| 1 | banner, image description | console |
| 2 | per-CPU state, `this_cpu()` | — |
| 3 | physical allocator | firmware memory map |
| 4 | virtual memory, direct map, arena | PMM |
| 5 | object allocator | VMM arena |
| 6 | **verification of 3–5** | — |
| 7 | W^X on the kernel image | 4 KiB image mapping |
| 8 | GDT, TSS, IDT, IRQ registry, PIC, LAPIC | allocator, MMIO window |
| 9 | tick, timers, blocking sleep | IRQ layer |
| 10 | scheduler, threads, preemption | allocator, per-CPU |
| 10b | the other CPUs | scheduler, timers, page tables |
| 11 | system calls | user segments, TSS, scheduler |
| 12 | VFS, `/dev`, mounted volumes | allocator, mutexes |
| 13 | processes, ring-3 execution | 8, 10, 11, 12 |
| 14 | devices | everything above |
| 15 | the shell | scheduler, VFS, input |

---

## 3. Memory

### Physical: a buddy allocator with zones

```
struct page[]  one per page of RAM, giving each page an order,
               a zone, flags and a reference count

free lists     [zone][order], orders 0..10 (4 KiB .. 4 MiB)

zones          DMA     < 16 MiB    ISA devices
               DMA32   < 4 GiB     32-bit PCI devices
               NORMAL  everything else
```

Zones exist because a 32-bit device handed a page above 4 GiB will DMA to
a truncated address and corrupt whatever is there — a failure that
presents as data corruption, not as an allocation error, so the
constraint belongs in the allocator.

**The reserve is computed, never stated.** Every reserved page comes from
the firmware memory map or from the linker's own
`__kernel_phys_start`/`__kernel_phys_end`. HavenDOS used a hard-coded
constant and its own source records it being wrong twice — 1 MiB, then
4 MiB, against a 6.31 MiB image — each time leaving live kernel BSS
inside the allocator's free pool.

### Virtual: four-level paging and a direct map

```
0x0000000000000000  user space (128 TiB)
      ── non-canonical hole ──
0xFFFF800000000000  HHDM: direct map of all physical RAM (64 TiB)
0xFFFFC00000000000  kernel heap arena (16 TiB)
0xFFFFD00000000000  MMIO / device window (16 TiB)
0xFFFFE00000000000  per-CPU stacks, each with a guard page
0xFFFFFFFF80000000  kernel image
```

The direct map makes `phys_to_virt()` one addition that cannot fail,
which is what lets the PMM and every DMA-capable driver touch a page they
know only by physical address. The kernel image is mapped with **4 KiB
pages on purpose**, even though 2 MiB would be cheaper: per-section W^X
needs the 4 KiB boundaries the linker script produces, and a 2 MiB
mapping would force each region to the weakest permission of its
contents, quietly making `.text` writable.

### Objects: size-class slabs over the arena

Caches at 16, 32, 64, 128, 256, 512, 1024 and 2048 bytes; anything larger
gets whole pages. Every allocation carries a header magic checked before
any field is trusted, and a trailing redzone checked on free. Those are
HavenDOS's ideas, kept — including its actual magic values, `0xA110CA7E`
and `0xFEEBDAED`, so a HavenOS memory dump is readable by anyone who has
debugged HavenDOS. What changes is that the arena grows, where HavenDOS's
256 KiB static array could not.

### DMA

`dma_alloc_coherent()` returns memory that is physically contiguous,
within a device's addressing limit, and reachable through the direct map.
It exists because paging made driver authorship harder in exactly one
way: HavenDOS had no virtual memory, so every buffer was DMA-safe by
accident.

---

## 4. Execution

### Threads and the scheduler

Fixed-priority round-robin, 8 bands, 4-tick slices (40 ms at 100 Hz),
with **per-CPU run queues** so the common case — picking the next thread
— touches no shared state.

The four rules that make it correct are written at the top of
`kernel/sched/sched.c`:

1. A context switch must happen on the interrupted thread's **own**
   kernel stack, never from a nested handler or an IST stack.
2. A spinlock may never be held across a context switch.
3. A wake-up that arrives before the block must not be lost
   (`thread->wake_pending`).
4. Every CPU always has something to run (a per-CPU idle thread).

Rule 1's first version said "never from interrupt context", and the
in-kernel self-test caught it immediately: a thread spinning in a tight
loop reaches no other switch point, so four spinning threads produced one
that ran forever and three that never started. Preemption now happens at
the end of `irq_dispatch()`, once the nesting count is back to zero.

### Processes

A process is the unit of isolation; a thread is the unit of scheduling.
A process has its own page tables, its own file-descriptor table,
credentials the kernel checks, and runs in ring 3. HavenDOS's "users"
were entries in a login table and their programs were kernel functions —
`shell.c`, `desktop.c`, `tetris.c` all ran in ring 0.

### System calls

`SYSCALL`/`SYSRET`, 30 numbered calls, arguments in
`rdi, rsi, rdx, r10, r8, r9`. `r10` rather than `rcx` because the
`SYSCALL` instruction overwrites `rcx` with the return address.

The first three instructions of the entry path are the most
safety-critical in the kernel, and `arch/x86_64/cpu/entry.S` says so:
`SYSCALL` does not switch stacks, does not disable interrupts, and leaves
`GS` pointing at user memory.

### Hardware-enforced separation

| Mechanism | Effect |
|---|---|
| ring 3 | user code cannot execute a privileged instruction |
| per-process page tables | one process cannot see another's memory |
| NX | a data page cannot be executed |
| W^X on the kernel image | `.text` read-only, `.rodata`/`.data` non-executable |
| SMEP | ring 0 cannot execute a user page |
| SMAP | ring 0 cannot even read one outside a STAC/CLAC window |
| guard pages | a stack overflow faults instead of corrupting a neighbour |
| `copy_from_user()` | a hostile pointer becomes `-EFAULT`, not a dead kernel |

---

## 5. Filesystems

```
                  ┌──────────────────────────────┐
                  │  open / read / write / stat  │
                  └──────────────┬───────────────┘
                     fs/vfs.c — mount table, path walking
                  ┌──────────────┼───────────────┐
                  ▼              ▼               ▼
              ramfs          devfs            FAT12/16/32
            on-demand      devices as     over a block device
            pages,         files
            real dirs
```

**The VFS walks paths, not the filesystem.** `vfs_resolve()` splits a
path and calls `->lookup()` once per component, so a filesystem never
sees a separator. That is the structural fix for HavenDOS's documented
bug: it passed whole paths to `fat16_write()`, which handed them to
`name_to_83()`, so `boot/grub.cfg` became the 8.3 name `BOOT/GRU` in the
root directory — a file with a slash in its name that no other FAT
implementation will find.

An unimplemented operation is a NULL pointer and returns `-ENOSYS`, which
a caller can tell apart from success. HavenDOS's `vfs_mkdir()` returned 0
for FAT12 with a "not implemented" comment.

---

## 6. Devices

Drivers register into a link-time table with `HAVEN_DRIVER()`, declare
which bus and identifiers they match, and are probed by the bus that
finds a matching device. **Adding a driver is adding a file**; nothing in
`kernel/main.c` names a device.

A device no driver claims is reported as such. That makes "is this
supported" answerable — the question HavenDOS's hardware list could not
answer, and which its own documentation has to disclaim: *"Recognizing a
controller in a hardware list does not mean it has a working storage
driver."*

```
device framework
  ├── platform bus ── PS/2 keyboard, CMOS RTC, PIT, serial
  └── PCI bus ─────── recursive enumeration through bridges
                      ├── virtio-blk ──┐
                      └── (unclaimed)  │
                                       ▼
  ATA/SATA (PIO, LBA48) ──────────► block layer
                                       │  MBR partitions as
                                       │  their own devices
                                       ▼
                                  FAT / raw /dev/<name>
```

---

## 7. What is not here

Stated plainly, because a roadmap that implies more than exists is worse
than a short one:

- **No networking.** HavenDOS had ARP, ICMP, UDP, TCP, DHCP, DNS, HTTP
  and TLS. None of it is ported yet; the socket layer and drivers are
  Phase 3 work.
- **No I/O APIC.** Device interrupts arrive through the 8259 pair in
  virtual-wire mode, so they are delivered to the boot CPU only. The
  LAPIC timer is per-CPU, so every CPU is still preempted.
- **No `fork()`.** `-ENOSYS`, deliberately: a `fork()` without
  copy-on-write would copy an address space that `exec` immediately
  discards. `proc_spawn()` covers the case programs actually use.
- **No graphics beyond a framebuffer console**, no window manager, no
  desktop.
- **No USB, no NVMe, no AHCI.**
- **No ACPI beyond the MADT**, no AML, no power management.
- **`/dev/random` is not cryptographic.** It says so where it is
  registered.

See [the roadmap](../README.md#development-roadmap) and
[CHANGELOG.md](../CHANGELOG.md).

---

## 8. Size

| Area | Lines |
|---|---|
| `arch/x86_64/` | 5,915 |
| `arch/arm64/` (skeleton) | 604 |
| `kernel/` | 8,843 |
| `fs/` | 3,547 |
| `drivers/` | 3,352 |
| `lib/` | 699 |
| `include/` | 4,227 |
| `tests/` | 3,237 |
| **total C, headers and assembly** | **30,254** |

Plus 1,327 lines of build and release tooling in `tools/` and
`scripts/`.

Built image: 185 KiB of code and constants, 2.3 MiB of BSS (mostly
statically reserved page tables and per-CPU blocks), 8.3 MiB ISO.

For comparison, HavenDOS 0.7.18 is about 39,400 lines — larger, because
most of it is the shell, desktop and applications that in HavenOS belong
in user space.
