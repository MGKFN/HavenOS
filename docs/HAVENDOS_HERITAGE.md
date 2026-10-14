# HavenDOS heritage

HavenOS is derived from [HavenDOS](https://github.com/MGKFN/HavenDOS) by
Tech Haven Studios. This document records what was inherited, what was
replaced, and why — in enough detail that someone who knows HavenDOS can
find their way around, and someone who knows neither can see what the
design is a reaction to.

The reference is the only published source tree, **HavenDOS 0.7.18**
(`havendos-v0_7_18-source.zip`): 39,428 lines of C and x86 assembly across
64 `.c` files and 29 headers. HavenOS 0.1.0 is 30,254 lines of C, headers
and assembly, plus 1,327 lines of build and release tooling.

The two sizes are not comparable in the way they look. HavenDOS puts
15,948 lines in `shell/` — the desktop, the text editor, the calculator,
the calendar, the package manager, the chat client, the login screen — all
of it inside the kernel, because it has no user space to put it in.
HavenOS has a 922-line kernel shell for inspecting the system and a
process model for everything else.

## The short version

| | HavenDOS 0.7.18 | HavenOS 0.1.0 |
|---|---|---|
| Word size | 32-bit protected mode | 64-bit long mode |
| Boot protocol | Multiboot 1 | Multiboot2 |
| Firmware | BIOS and UEFI (32-bit kernel loaded by a 64-bit loader) | BIOS and UEFI, 64-bit throughout |
| Address space | identity-mapped, one space | higher-half kernel, per-process spaces |
| Physical memory | bitmap, 2 GiB ceiling, first-fit scan | buddy allocator over all RAM, with DMA zones |
| Kernel heap | one 256 KiB static array | 16 TiB demand-backed arena with size-class caches |
| Multitasking | none; a cooperative main loop | preemptive, priority round-robin, SMP |
| Processors | one | up to 64, brought up through ACPI |
| User/kernel split | none; everything is ring 0 | ring 3 processes, SMEP/SMAP/NX, validated syscall arguments |
| Filesystems | FAT12, FAT16, ramfs, ISO9660 | FAT12/16/32, ramfs, devfs, behind a VFS switch |
| Drivers | called directly by name | link-time registry, probed against a bus |
| Portability | x86-only by construction | 62-function HAL, checked mechanically |
| Tests | 446 lines, 3 files | 842 host checks and 82 in-kernel checks |
| Networking | ARP/ICMP/UDP/TCP/DHCP/DNS/TLS, 4,149 lines | **none** |

The last row is the honest one. HavenDOS has a working network stack and
HavenOS does not. Nothing was ported because a 32-bit single-tasking
stack written against a flat address space is not a starting point for a
64-bit preemptive one; it is a specification for what to write next.

## What was inherited

### Ideas that were right and were kept

**The memory-corruption magics.** HavenDOS added magic values to its heap
blocks in v0.6.8 to detect corruption. HavenOS keeps the idea *and the
constants* — `0xA110CA7E` for a live allocation and `0xFEEBDAED` for a
free one still appear in `kernel/mm/kmalloc.c`, next to a redzone and a
`0xDD` poison fill. They are a small, deliberate continuity: a kernel
developer who has debugged HavenDOS will recognise them in a HavenOS
memory dump.

**Deriving the reserve from the linker.** HavenDOS's physical allocator
protected the kernel image with a hard-coded boundary, got it wrong twice
(1 MiB, then 4 MiB against a 6.31 MiB image), and fixed it in v0.7.18 by
taking `_kernel_end` from the linker. HavenOS starts from that
conclusion: nothing about the image's extent is written by hand anywhere,
and `tools/verify_image.py` checks the result on every build.

**Generating the version.** HavenDOS shipped an image whose
`ETC/SYSCONF.TXT` said `VERSION=0.6.1` while the kernel was 0.7.18, and
added `tools/make_version.py` in v0.7.18 so the GRUB titles and version
files could not drift again. HavenOS has the same tool, from the first
commit, and `include/haven/version.h` is the only place a version number
is written.

**An explicit, numbered boot sequence.** HavenDOS's `kernel_main()`
accumulated ordering bugs its own comments catalogue: `net_init()` ran
*after* the screen that reported whether a NIC was found, so every boot
said "NIC .... Not detected"; `virtio_scan_drives()` ran lazily, so the
same screen said "VirtIO BLK .... Not detected" on machines whose disk
worked two lines later. Neither was a driver bug. HavenOS numbers its
stages, and each one states what it provides and what it requires —
`kernel/main.c` and `arch/x86_64/boot/main.c` are both written that way.

**Serial first.** HavenDOS could not debug a machine with no working
display. In HavenOS the serial port is initialised before anything that
can fail, and the console ring buffer is replayed into every console
registered later, so a display that appears halfway through the boot still
shows the whole log.

**Testing the allocator directly.** HavenDOS's `tests/test_pmm.c` argued
that a bug in the physical allocator hands out memory belonging to the
kernel, which is worth testing rather than inferring from a successful
boot. HavenOS agrees and takes it further: the host suite links the real
kernel sources, and `test_pmm` alone is 478 checks.

### Code lineage

One file is inherited verbatim: `include/haven/font8x8.h`, the 8x8
console font. It is Daniel Hepper's public-domain `font8x8`, which
HavenDOS had already bit-reversed from the upstream LSB-first layout into
MSB-first order for direct upload to a VGA font plane. HavenOS keeps that
orientation because it is also the order a linear framebuffer wants, so
re-deriving it would have meant redoing work that was already correct.

Nothing else survives unchanged — the word size alone guarantees that.
What survives is structural: the FAT directory-entry handling, the ATA PIO
command sequence, the PCI configuration mechanism, and the PIT and CMOS
programming are all recognisably descended from their HavenDOS
counterparts, re-expressed for 64-bit and re-checked against the
specifications.

87 source files in HavenOS carry a comment naming HavenDOS — 253
references in all. Where a design decision was made *because* of
something in HavenDOS, the comment says which file and which version.
That is the actual mechanism of inheritance here: not copied code, but a
recorded reason.

## What was replaced, and why

### 32-bit to 64-bit

This is the change everything else follows from. HavenDOS loads a 32-bit
kernel even under UEFI, which its own README states plainly: "UEFI boot
support does not make the kernel 64-bit". The consequences are
structural, not cosmetic — a 32-bit kernel cannot address more than 4 GiB
of physical memory, cannot use NX per page without PAE gymnastics, and
cannot run a 64-bit program.

HavenOS enters long mode in `arch/x86_64/boot/boot.S` before any C code
runs, and lives at `0xFFFFFFFF80200000` with the kernel in the upper half
of a 48-bit address space. The identity map used to get there is dropped
once real page tables exist, which is how a stray physical-address
dereference becomes a fault instead of a silent success.

### The allocators

HavenDOS had two, and both were limited by construction:

- `pmm.c` — a bitmap over a fixed 2 GiB ceiling with a linear first-fit
  scan.
- `heap.c` — a **256 KiB static array** with a first-fit free list. Every
  `kmalloc()` in the system competed for that quarter-megabyte, and it
  could never grow, so in v0.7.18 the large buffers had to be moved onto
  the physical allocator with `pmm_alloc_contig()` — which is what made
  the boundary bug live for the first time. One limitation created the
  conditions for the other.

HavenOS replaces both with a buddy allocator over a `struct page` array
covering all of RAM, zoned so a 32-bit DMA device is given memory it can
actually address; a 16 TiB kernel arena backed on demand, so a large
allocation costs address space rather than competing for a fixed pool; and
size-class caches over that arena. A 4 MiB allocation no longer needs 4
MiB of adjacent physical memory.

### No multitasking to preemptive multitasking

HavenDOS runs one thread of control. Its documented consequence is a
delay loop that overshoots without bound, because nothing can run while
it waits.

HavenOS has threads, a fixed-priority round-robin scheduler with per-CPU
run queues, and preemption from the timer interrupt. The measured
difference: four spinning threads share one CPU through nine preemptions
in 200 ms, and a 100 ms sleep takes 103 ms.

That last part took two attempts, and the first one was wrong in an
instructive way — the original scheduler asserted that a context switch
may never happen from interrupt context, so the timer only set a flag that
was acted on at syscall return. A thread that spins reaches neither. The
in-kernel self-test caught it immediately: of four spinning threads, one
ran forever and three never started. The reasoning and the fix are in
[DESIGN_DECISIONS.md](DESIGN_DECISIONS.md).

### Everything in ring 0 to a real boundary

HavenDOS's desktop, editor, shell and network clients are kernel code.
There is no user mode, so there is nothing to enforce.

HavenOS gives each process its own page tables, runs it in ring 3, and
enables SMEP, SMAP and NX where the CPU has them. Every pointer crossing
the boundary is validated and copied through `copy_from_user`, which uses
an exception-table fixup so a bad user pointer returns `-EFAULT` instead
of panicking the kernel. W^X is applied to the kernel's own image at
boot: `.text` is read-execute, `.rodata` is read-only, and `.data`/`.bss`
are never executable.

### Direct driver calls to a driver framework

HavenDOS calls drivers by name from `kernel_main()`, in an order that was
the source of the reporting bugs above. HavenOS drivers register
themselves into a link-time table with `HAVEN_DRIVER()` and are probed
against the buses actually found, so `lsdev` can distinguish a device that
was *seen* from one that was *claimed* — the distinction HavenDOS's "Not
detected" screens could not make.

### Portability

HavenDOS is x86-only by construction: `io.h` is inline x86 assembly and
every subsystem includes it. HavenOS routes all hardware access through
62 functions declared in `include/haven/arch.h`, and
`scripts/check-portability.sh` fails the build if generic code reaches
past them. The check has already caught a real violation —
`kernel/main.c` had grown an `#include <haven/arch/x86_64/acpi.h>` and a
direct `acpi_init()` call, which moved into `smp_start_secondary()` where
finding the CPU list is the architecture's business.

`arch/arm64/` exists as a skeleton: the barriers, interrupt masking,
timer and direct map are implemented; everything else panics with a
pointer to [PORTING_ARM64.md](PORTING_ARM64.md). It is there to keep the
HAL honest, not to claim a port.

## What HavenDOS has that HavenOS does not

Said plainly, because the comparison table above is not an argument that
HavenOS is finished:

- **A network stack.** ARP, ICMP, UDP, TCP, DHCP, DNS and TLS, with
  RTL8139 and e1000 drivers. 4,149 lines of it. HavenOS has none.
- **A desktop.** A character-based desktop with a text editor,
  calculator, calendar, to-do list and settings.
- **An installer and an update mechanism.** HavenDOS installs to a disk
  and can roll back an update. HavenOS boots from its ISO only.
- **User management.** A login screen and user accounts.
- **A package manager.**
- **Demonstrated UEFI boot on real hardware** — an HP laptop, 9 October
  2026. HavenOS's UEFI path is tested under OVMF.
- **ISO9660 and ATAPI drivers**, for reading its own install media.

HavenOS 0.1.0 delivers Phases 1 and 2 of the
[roadmap](../README.md#development-roadmap): the boot path, memory
management, interrupts, the scheduler, processes, system calls, the ELF
loader and the virtual filesystem. The desktop and the network stack are
Phases 3 and 4, and will be written for a 64-bit preemptive kernel rather
than ported to one.

## On reading HavenDOS's own documentation

HavenDOS's README is unusually candid about its limitations — it says
which laptop booted, that installing to that laptop's NVMe disk does not
work, that recognising a controller in a hardware list does not mean there
is a driver for it, and that some 0.8.1 interface labels still say "Beta".

That tone was inherited too, and it is the most useful thing in this
document. Every HavenOS claim in `docs/` is either something that was
measured or something stated as not done. Where a fix did not work —
the serial port's loss of a byte during FIFO initialisation, which three
hypotheses failed to cure — the comment in the source says what is
actually happening instead of claiming a fix.

## Licensing and attribution

HavenDOS is MIT licensed, and HavenOS is MIT licensed. The copyright
notice in `LICENSE` and in every source header credits KUNIX / Tech Haven
Studios, and the kernel says so at boot and in `uname`:

```
HavenOS 0.1.0 "Foundation" x86_64 2026-10-14 (rev 939b885)
Copyright (c) 2026 KUNIX / Tech Haven Studios - MIT licensed
derived from HavenDOS by Tech Haven Studios
```

That third line is in the banner on purpose. The derivation is not a
footnote.
