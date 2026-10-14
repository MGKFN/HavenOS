# HavenOS 0.1.0 "Foundation"

**Released 14 October 2026 · MIT licensed · x86_64**

The first release of HavenOS: a 64-bit, preemptive, multi-processor
operating system derived from
[HavenDOS](https://github.com/MGKFN/HavenDOS), which is 32-bit and
single-tasking.

This is a hobby operating system under active development. It boots, it
runs programs in user mode, it reads and writes FAT filesystems, and it
has no networking. Please read [What is not here](#what-is-not-here)
before planning anything around it.

---

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
