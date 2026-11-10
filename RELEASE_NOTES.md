# HavenOS 0.1.2 "Foundation"

**Released 10 November 2026 · MIT licensed · x86_64**

HavenOS is a 64-bit, preemptive, multi-processor operating system derived
from [HavenDOS](https://github.com/MGKFN/HavenDOS), which is 32-bit and
single-tasking.

This is a hobby operating system under active development. It boots, it
runs programs in user mode, it reads and writes FAT filesystems, and it
has no networking. Please read [What is not here](#what-is-not-here)
before planning anything around it.

---

## What 0.1.2 is: the first real-hardware fixes

0.1.1 was booted on a real UEFI laptop for the first time. GRUB printed
`WARNING: no console will be available to OS`, and the screen stayed
exactly as GRUB left it. With no serial port on the machine there was no
way to tell whether the kernel had crashed, hung, or was running.

The warning had one definite cause, fixed here: the kernel never told
GRUB it could use a framebuffer. Why nothing appeared *afterwards* on
that laptop is not known, so 0.1.2 also removes every early-boot
assumption that was found to hold in QEMU and not on a real PC:

| Assumption in 0.1.1 | Reality | Now |
|---|---|---|
| The loader will set a video mode unasked | Only QEMU's firmware did | The kernel requests 1280×800×32; GRUB falls back to any mode it can set |
| The framebuffer is below 4 GiB | Current laptops often put it above | Mapped through its own window, at any address |
| The PIT exists and counts | Firmware on many laptops gates it off | Results are sanity-checked; ACPI PM timer and CPUID are fallbacks |
| Waiting for the PIT is quick | 100 million port reads is minutes on hardware | Bounded to about a second |
| The loader passes the ACPI pointer | Not guaranteed | The legacy search no longer faults |
| The APIC is in xAPIC mode | Some firmware hands over in x2APIC | Detected and reported; legacy path used |
| Firmware reports at most 64 memory regions | Real maps are longer | 256 |

And the display now comes up **first**, before any of the rest, so that
the next failure on a machine like that one is written on the screen.

### What was and was not verified

Under QEMU 8.2: the self-tests pass on 13 display configurations (five
graphics adapters and no adapter at all, under both BIOS and UEFI), with
the PIT removed, with ACPI removed, and on the usual matrix (4 CPUs,
VirtIO, SMEP/SMAP). `make release` passes.

**Not verified: the laptop.** Nothing here has been run on real
hardware. The x2APIC path and a framebuffer physically above 4 GiB could
not be produced in the emulator at all; the code for the second is the
same code that runs for every framebuffer, which is the most that can be
said for it.

### If it still does not boot on your machine

Choose **verbose boot log** in the GRUB menu and photograph the screen
where it stops. The last line names the step that did not finish. If the
screen is still blank after GRUB's menu, say so — that means GRUB could
not set any graphics mode, which is a different problem from a kernel
that hangs.

### Known limits on real hardware

- **The internal keyboard must be PS/2-style** (most laptop keyboards
  are). USB keyboards do not work: there is no USB driver.
- **No disk will be found** on most machines: there is no AHCI or NVMe
  driver. `/bin/hello` and the RAM filesystem work without one.
- **The console scrolls slowly** on real graphics hardware; the
  framebuffer is not mapped write-combining yet.

---

## What 0.1.1 was: a correction

**0.1.0 said it ran programs in user mode. It did not.** These notes
listed "a real user/kernel boundary" and the system-call interface under
"verified", but nothing in the tree had ever run in ring 3: `/bin` was
empty and the self-tests never left the kernel. The first program anyone
tried to run found that every system call returned `-ENOSYS`, that the
first call taking a pointer hung the machine, and that preempting a user
program panicked the kernel.

0.1.1 fixes that, and the eleven other defects found behind it, and makes
it impossible to ship again: the self-test suite now builds a real user
program, embeds it in the kernel and runs it on every boot test. The full
account is in [docs/AUDIT.md](docs/AUDIT.md).

### New in 0.1.1

- **Programs run.** ELF64 executables run in ring 3, make system calls,
  are preempted, are isolated from the kernel and from each other, and are
  completely reclaimed when they exit.
- **`/bin/hello`** is installed at boot. Try `run /bin/hello one two`.
- **`run` is a foreground command**: it passes arguments, waits for the
  program and reports a non-zero exit status.
- **`user/`**: a minimal runtime for writing your own programs.
- **VirtIO disks can be synced.** The flush request was malformed.
- **Compute-bound work gets the whole CPU.** The boot threads no longer
  take every other time slice.
- **CI on every push**: host tests, portability check, and boot tests
  under BIOS, UEFI, 4 CPUs, VirtIO and SMEP/SMAP.

### Verified in 0.1.1

Observed under QEMU 8.2, not inferred:

| | Result |
|---|---|
| Host tests | 842 checks across 7 suites |
| In-kernel self-tests, BIOS + IDE | 91 checks across 14 tests |
| In-kernel self-tests, UEFI (OVMF) | 91 of 91 |
| In-kernel self-tests, 4 CPUs with SMEP and SMAP | 91 of 91, 8 consecutive runs |
| In-kernel self-tests, VirtIO disk | 91 of 91 |
| A ring-3 program's own checks | 15 of 15: system calls, rejected pointers, heap, `mmap`, files, registers across a system call and across preemption |
| Two programs at the same virtual addresses | each keeps its own memory |
| Reading kernel memory, writing code, executing the stack | each kills the program, not the kernel |
| Memory after three processes exit | not one page leaked |
| `make release` | passes, including building from the extracted archive |

**Not verified:** real hardware, VMware, and AArch64. Everything above is
QEMU. Three concurrency hazards were found by reading the source and have
not been reproduced or fixed; they are listed in
[docs/AUDIT.md](docs/AUDIT.md#4-broken-or-incomplete).

### Getting it

The ISO is attached to the
[GitHub release](https://github.com/MGKFN/HavenOS/releases/tag/v0.1.2), or
build it:

```bash
git clone https://github.com/MGKFN/HavenOS.git
cd HavenOS
make iso          # build/havenos-0.1.2.iso
make run          # boot it under QEMU with the log on your terminal
```

---

# The 0.1.0 notes

What follows is the text of the 0.1.0 release notes, kept because the
description of the kernel still stands. Read its claims about user mode
and the system-call interface in the light of the correction above.

## What this release is

HavenDOS 0.8.1 is a working 32-bit system with a desktop, a text editor, a
network stack and an installer — all of it in ring 0, in one address
space, with no scheduler. HavenOS 0.1.0 has none of those applications.
What it has instead is the foundation they were missing: long mode, a real
virtual address space, preemptive multitasking across multiple
processors, and an enforced user/kernel boundary.

That is a deliberate trade, and it is the whole point of starting again
rather than extending. A 32-bit kernel cannot address more than 4 GiB,
cannot run a 64-bit program, and cannot be given a scheduler without
rewriting everything that assumed it did not have one.

This release completes Phases 1 and 2 of the
[roadmap](README.md#development-roadmap): UEFI boot, memory management,
interrupt handling, a scheduler, process management, system calls, an ELF
loader and a virtual filesystem.

## Highlights

**64-bit throughout.** A higher-half kernel at `0xFFFFFFFF80200000`,
entered through Multiboot2, with a four-level page table and a 256 TiB
address-space layout. One ISO boots under both legacy BIOS and 64-bit
UEFI firmware.

**Memory management that uses the machine it is on.** A buddy allocator
over every page of RAM, zoned so a 32-bit DMA device gets memory it can
address, and a 16 TiB kernel arena backed on demand. HavenDOS had a
bitmap with a 2 GiB ceiling and a 256 KiB static heap that every
`kmalloc()` in the system competed for.

**Preemptive multitasking, on every core.** Fixed-priority round-robin
with per-CPU run queues and preemption from the timer interrupt.
Measured: four spinning threads that never yield share one CPU through 13
preemptions in 200 ms, and a 100 ms sleep takes 103 ms. HavenDOS's
documented behaviour was a delay loop that overshoots without bound,
because nothing could run while it waited.

**A real user/kernel boundary.** Processes get their own page tables and
run in ring 3, with SMEP, SMAP and NX enabled where the CPU has them.
W^X is applied to the kernel's own image. Every pointer crossing the
boundary is validated and copied through an exception-table fixup, so a
bad user pointer returns `-EFAULT` instead of panicking the kernel.

**Filesystems behind a switch.** FAT12, FAT16 and FAT32 in one driver,
plus `ramfs` and `devfs`, behind a VFS with a mount table. Every block
device is probed at boot and every FAT volume found is mounted
automatically under `/mnt/`.

**Drivers that register themselves.** A link-time registry replaces
HavenDOS's named calls from `kernel_main()` — the arrangement that made
its boot screen report "NIC .... Not detected" on machines with a working
NIC, because the report ran before the probe.

**Portability that is checked, not claimed.** All hardware access goes
through 62 functions in `include/haven/arch.h`, and
`scripts/check-portability.sh` fails the build if generic code reaches
past them. An AArch64 skeleton implements 30 of the 62 and panics on the
rest, naming what is missing — it exists to keep the abstraction honest.

## Verified in this release

Everything in this list was observed, not inferred:

| | Result |
|---|---|
| Long mode, BIOS boot | boots to a shell |
| Long mode, UEFI boot (OVMF) | boots to a shell |
| RAM reported from the firmware map | 511 MiB of 512 (BIOS), 505 MiB (UEFI) |
| Smallest configuration booted | 8 MiB |
| W^X on the kernel image | applied; identity map dropped |
| Timer preemption | 4 spinning threads, 13 preemptions in 200 ms |
| Sleep accuracy | 100 ms requested, 103 ms measured |
| Priority ordering | honoured against creation order |
| SMP | 4 of 4 CPUs online under `-smp 4` |
| PCI enumeration | 6 devices identified and named |
| ATA | IDENTIFY reports model and LBA48; MBR parsed, partition registered |
| VirtIO block | 256-entry queue, reads through the bounce buffer, zero errors |
| FAT | volume written by `mtools` enumerated; 24 KiB multi-cluster file written and verified byte-for-byte |
| Kernel shell | 25 commands driven over the serial console |
| Host tests | 842 checks across 7 suites |
| In-kernel self-tests | 82 checks across 13 tests |
| Portability check | 62 HAL entry points × 2 architectures |

`make check` runs the host suite, the portability check, a full build and
the in-kernel self-tests under QEMU, and is green.

## Getting it

There is no prebuilt image for 0.1.0. Build the ISO:

```bash
git clone https://github.com/MGKFN/HavenOS.git
cd HavenOS
make iso          # build/havenos-0.1.0.iso, 8.0 MiB
make run          # boot it under QEMU with the log on your terminal
```

A stock x86_64 Linux toolchain is enough — no cross-compiler, because the
kernel is compiled freestanding for the host's own architecture. GCC 12+
or Clang 15+, GNU binutils, GNU Make, Python 3.8+. For images and
emulation: `grub-pc-bin`, `grub-efi-amd64-bin`, `xorriso`, `mtools`,
`qemu-system-x86`.

Start in a virtual machine, and give it an empty virtual disk. See the
[user guide](docs/USER.md).

## What is not here

- **No networking.** No stack, no NIC driver. The e1000 that `lsdev`
  reports under QEMU is identified, not driven. HavenDOS has ARP, ICMP,
  UDP, TCP, DHCP, DNS and TLS; none of it was ported, because a 32-bit
  single-tasking stack written against a flat address space is not a
  starting point for a 64-bit preemptive one.
- **No USB**, so no USB keyboards and no USB storage.
- **No NVMe and no AHCI.** Storage is ATA PIO and VirtIO. A modern SATA
  or NVMe disk will appear in `lsdev` and be unusable.
- **No graphical desktop and no window manager.** The framebuffer is a
  text console.
- **No installer.** HavenOS boots from its ISO.
- **No user accounts and no permissions.** The user/kernel boundary is
  real and enforced; a *user* model is not there yet.
- **No `fork()`.** It returns `-ENOSYS` on purpose — copy-on-write is not
  implemented, and an eager-copy `fork()` would be a trap for anything
  that used it.
- **Long file names on FAT are read-only.** New files get 8.3 names.
- **The first byte typed into the serial port can be lost.** Enabling the
  16550 FIFO resets the receiver. Three hypotheses were tested and none
  fixed it; press Enter once before your first command.

Full detail, including every bug found during development and what found
it, is in [CHANGELOG.md](CHANGELOG.md).

## Documentation

| Document | For |
|---|---|
| [docs/USER.md](docs/USER.md) | booting it and using it |
| [docs/DEVELOPER.md](docs/DEVELOPER.md) | building, testing, debugging, contributing |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | how the subsystems fit together |
| [docs/DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md) | why each choice was made, including the wrong ones |
| [docs/API.md](docs/API.md) | the kernel API, generated from the headers |
| [docs/PORTING_ARM64.md](docs/PORTING_ARM64.md) | the AArch64 path |
| [docs/HAVENDOS_HERITAGE.md](docs/HAVENDOS_HERITAGE.md) | what came from HavenDOS and what changed |

`docs/DESIGN_DECISIONS.md` is the one worth reading if you only read one.
It includes the decisions that turned out to be wrong — the scheduler that
could not preempt a spinning thread, and the VirtIO queue size that was
clamped rather than refused — because a design document that records only
the successes is not much use to the next person.

## Upgrading from HavenDOS

You cannot, and that is not a packaging problem. HavenOS does not install
itself to a disk, does not read HavenDOS's configuration, and requires a
64-bit processor that HavenDOS does not. HavenDOS 0.8.1 remains the
system to use on 32-bit hardware and the system to use if you need a
desktop or networking today.

The two can share a disk: HavenOS mounts FAT volumes read-write, and
HavenDOS's persistent filesystem is FAT16.

## Thanks

HavenOS is derived from HavenDOS by **Tech Haven Studios**, and the
derivation is in the boot banner, not a footnote. HavenDOS's own
documentation — candid about which laptop booted, which disk could not be
installed to, and which interface labels still said "Beta" — set the
standard this project's documentation tries to meet.

The 8x8 console font is `font8x8` by Daniel Hepper, public domain.

## Reporting problems

[GitHub Issues](https://github.com/MGKFN/HavenOS/issues). Include the
output of `uname`, the firmware mode (BIOS or UEFI), the full serial log,
and the QEMU command line or machine description. For storage problems,
include `lsdev` and `lsblk`.

The serial console carries the entire kernel log at 115200 8N1, and that
log is what makes a report actionable.

---

**Next:** Phase 3 — storage drivers (AHCI, NVMe), USB, and a networking
stack written for a preemptive 64-bit kernel.
