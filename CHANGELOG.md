# Changelog

All notable changes to HavenOS are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and HavenOS
uses [semantic versioning](https://semver.org/).

Versions before 1.0.0 make no interface-stability promise: the kernel API
is internal and changes without notice.

## [0.1.0] — 2026-10-14 — "Foundation"

The first release. HavenOS is a 64-bit operating system derived from
[HavenDOS](https://github.com/MGKFN/HavenDOS) 0.7.18, which is 32-bit and
single-tasking. Nothing here is an incremental change to HavenDOS; the
whole kernel is new, and
[docs/HAVENDOS_HERITAGE.md](docs/HAVENDOS_HERITAGE.md) records what was
inherited, what was replaced, and why.

This release completes Phases 1 and 2 of the
[roadmap](README.md#development-roadmap).

### Added — boot

- Multiboot2 boot path with a 64-bit higher-half kernel at
  `0xFFFFFFFF80200000`, entered through the Multiboot2 entry-address tag.
- Long-mode transition in `arch/x86_64/boot/boot.S`: PAE, `EFER.LME`,
  `CR0.PG`, a 64-bit GDT, and an absolute jump to the higher half.
- One hybrid ISO that boots under legacy BIOS (El Torito) and 64-bit UEFI
  (EFI System Partition), built by `grub-mkrescue`.
- A diagnostic path for 32-bit CPUs: long-mode absence is detected and
  explained on both the screen and COM1, instead of faulting.
- `tools/verify_multiboot2.py` and `tools/verify_image.py` validate the
  header and the segment layout on every build.
- Firmware-neutral handover: `struct boot_info` carries the memory map,
  framebuffer, modules, ACPI RSDP and kernel image bounds, so no
  subsystem sees a Multiboot2 structure.
- A GRUB menu generated from `include/haven/version.h`, with entries for
  verbose logging, single-CPU operation, the self-test suite and a
  serial-only console.

### Added — memory management

- Buddy physical allocator over a `struct page` array covering all RAM,
  with DMA (below 16 MiB), DMA32 (below 4 GiB) and NORMAL zones, so a
  32-bit device is given memory it can address.
- Reserved regions derived from the firmware memory map and from the
  linker's own image bounds — nothing about the image extent is written
  by hand.
- Four-level paging with mixed granularity: 1 GiB and 2 MiB mappings for
  the direct map, 4 KiB for the kernel image so per-section permissions
  are possible.
- A 256 TiB address space split: direct map at `0xFFFF800000000000`,
  16 TiB kernel arena at `0xFFFFC00000000000`, MMIO window at
  `0xFFFFD00000000000`, kernel stacks at `0xFFFFE00000000000`.
- Demand-backed kernel arena, so a large allocation costs address space
  rather than contiguous physical memory.
- Size-class slab allocator with header magics (`0xA110CA7E` live,
  `0xFEEBDAED` free — inherited from HavenDOS), a redzone, and `0xDD`
  poison on free.
- `pmm_check()` and `kmalloc_check()`: full consistency walks over the
  buddy free lists and live allocation headers, reachable from the shell
  as `meminfo`.
- W^X on the kernel image: `.text` read-execute, `.rodata` read-only,
  `.data`/`.bss` never executable. The identity map used during boot is
  dropped once real tables exist.
- Guard pages on kernel stacks.
- DMA support: coherent allocation plus per-device bounce buffers, so a
  buffer from the kernel arena can be given to a device that needs a
  physical address.

### Added — execution

- Preemptive, fixed-priority round-robin scheduler: 8 priority bands,
  per-CPU run queues, 4-tick slices at 100 Hz.
- Preemption from the timer interrupt, performed at the end of
  `irq_dispatch()` when interrupt nesting reaches zero.
- Ticket spinlocks that panic on deadlock, plus sleeping mutexes,
  semaphores and condition variables.
- Timekeeping: TSC calibrated against PIT channel 2 (median of three
  runs), with 128-bit multiply-shift nanosecond conversion, and a warning
  when the CPU has no invariant TSC.
- Local APIC timer in virtual-wire mode, calibrated against the PIT.
- SMP: ACPI MADT parsing, an AP trampoline that climbs real → protected →
  long mode, and per-CPU state through `GS` base and `SWAPGS`. Tested at
  four processors.
- GDT and TSS with IST stacks for `#DF`, NMI and `#DB`; an IDT with all
  256 vectors installed.

### Added — processes and system calls

- Per-process page tables, file-descriptor tables, `brk` and `mmap`.
- Ring 3 execution with SMEP, SMAP and NX enabled where the CPU provides
  them.
- `SYSCALL`/`SYSRET` entry with 30 system calls.
- `copy_from_user()` and `copy_to_user()` with exception-table fixups, so
  a bad user pointer returns `-EFAULT` rather than panicking.
- ELF64 loader with exhaustive validation of untrusted input.

### Added — filesystems

- Virtual filesystem switch with a mount table and path walking done in
  the VFS, one component per `->lookup()`.
- `ramfs` with page-block files and real directories.
- `devfs`, exposing `console`, `null`, `zero`, `random`, `keyboard`, `rtc`
  and every block device.
- FAT12, FAT16 and FAT32 in one driver, with the variant chosen from the
  cluster count. Reading and writing, including multi-cluster files;
  long file names are read-only.
- Automatic mounting: every block device is probed at boot and every FAT
  volume found is mounted under `/mnt/<device>`.
- A root namespace created at boot with `/dev`, `/tmp`, `/etc`, `/bin`,
  `/home`, `/var`, `/proc` and `/mnt`, and `/etc/os-release` written from
  the kernel's own version data.

### Added — devices and drivers

- Link-time driver registry: `HAVEN_DRIVER()` places a descriptor in
  `.haven.drivers`, and drivers are probed against the buses actually
  found rather than called by name in a fixed order.
- PCI enumeration with recursive bridge traversal.
- Block layer with MBR partition parsing, each partition registered as
  its own device.
- ATA/IDE in PIO mode with LBA48.
- VirtIO block over the legacy virtqueue interface.
- PS/2 keyboard, 16550 serial, CMOS real-time clock, framebuffer and EGA
  text consoles.

### Added — diagnostics

- A kernel log with levels, a ring buffer, and replay into any console
  registered later, so a display that appears mid-boot still shows the
  whole log.
- A 28-command kernel shell: `meminfo`, `cpuinfo`, `ps`, `lsblk`,
  `lsdev`, `lsirq`, `mount`, `dmesg`, `selftest`, `run`, and the usual
  file commands.
- In-kernel self-test suite: 13 tests, 82 checks, run from the GRUB menu
  or with `selftest` on the command line. These are the checks that need
  real hardware — whether a context switch returns to the right stack,
  whether the timer actually preempts a spinning thread.
- Host test suite: 842 checks across 7 suites, each linking the real
  kernel source it exercises rather than a copy.
- `scripts/check-portability.sh`: fails the build if generic code
  contains inline assembly, architecture-conditional code or an
  architecture-private include, and verifies that both architectures
  define all 62 HAL entry points.
- Panics print the reason, the faulting address and a backtrace to every
  console.

### Added — portability

- A 62-function hardware abstraction layer in `include/haven/arch.h`.
- An AArch64 skeleton (`arch/arm64/`) implementing 30 of those 62 —
  barriers, interrupt masking, CPU identity, the generic timer, address
  translation and TLB maintenance — with the remaining 32 panicking and
  naming what is missing. Its purpose is to keep the HAL honest, not to
  claim a port. See [docs/PORTING_ARM64.md](docs/PORTING_ARM64.md).

### Added — build and documentation

- A directory-discovery build: adding a file to `kernel/` or `drivers/`
  needs no Makefile edit, and `make ARCH=arm64` is a real target rather
  than a fork of the build.
- `make check` runs the host suite, the portability check, a full build
  and the in-kernel self-tests under QEMU.
- `make release` produces a verified release package and ZIP archive.
- Documentation: architecture, design decisions, API reference, developer
  guide, user guide, ARM64 porting guide and HavenDOS heritage.
- `docs/API.md` is generated from the headers by `tools/gen_api_docs.py`,
  so a declaration whose comment was not updated shows up in a diff.

### Fixed during development

Recorded because each one was found by something in the tree, and the
thing that found it is part of the deliverable.

- **Preemption never happened.** The first scheduler asserted that a
  context switch may never occur in interrupt context, so the timer only
  set a flag acted on at syscall return — which a spinning thread never
  reaches. The in-kernel self-test caught it immediately: of four
  spinning threads, one ran forever and three never started. Switching
  from the timer handler is in fact safe, because the handler runs on the
  interrupted thread's own kernel stack; what is unsafe is doing it from
  a nested handler or an IST stack.
- **Three firmware memory regions were dropped from the direct map**, one
  of them the kernel image, because overlapping regions in the firmware
  map made `arch_pt_map()` return `-EEXIST`. Mapping the same VA to the
  same PA with the same permissions is now idempotent.
- **The physical allocator could return pages above the boot direct map.**
  `pmm_init()` runs before `vmm_init()` extends the direct map, so a page
  above the boot window was unreachable through `arch_phys_to_virt()`.
  Bounded until `vmm_init()` lifts the limit.
- **A DMA buffer from the kernel arena was passed to a device.** The
  partition parser's `kmalloc()`'d sector buffer reached
  `arch_virt_to_phys()`, which asserted rather than returning a wrong
  address. That assertion is why this was a five-minute diagnosis, and it
  is what produced `kernel/mm/dma.c`.
- **VirtIO timed out for five seconds per request** because the driver
  clamped the device's reported 256-entry queue to its own 64. In legacy
  VirtIO the queue-size register is read-only, so the ring layout
  disagreed with the device. An oversized or non-power-of-two queue is
  now refused, not clamped.
- **Bringing up a second CPU reset the machine.** An AP executes the
  trampoline at its physical address, so enabling paging with the kernel
  `CR3` left its instruction pointer unmapped: triple fault, and the log
  simply stopped. The trampoline page is now identity-mapped for the
  duration of bring-up.
- **One kernel image reported two different git revisions.** The build
  stamp was a `-D` on the command line, and a macro's value is invisible
  to dependency tracking, so objects compiled at different commits kept
  different revisions — `uname` and `/etc/os-release` disagreed. The
  stamp is now a generated header that participates in dependency
  tracking.
- **The portability checker exempted four HAL functions from its own
  check**, because its pattern did not match a `const char *` or
  `struct arch_pagetable *` return type. Found while documenting it.
- **The GRUB menu offered options the kernel ignored.** Two entries
  passed `loglevel=debug` and `console=serial` to a kernel that parsed
  neither. Both now work, with a 33-check host suite for the parser.
- **`-Werror` caught a 64 KiB stack frame** in `console_register()` — a
  `char tail[RING_SIZE]` buffer on a 64 KiB boot stack. It is also why
  kernel stacks have guard pages.
- **Host tests were testing glibc, not HavenOS.** GCC substituted
  `__builtin_strncpy` and `__builtin___snprintf_chk` for the calls under
  test, so the suite passed while never executing `lib/string.c`.
  `-fno-builtin` and `_FORTIFY_SOURCE=0` fixed it, and `test_printf` now
  checks `%b` — a HavenOS extension glibc does not implement — as a
  permanent canary that the right code is linked.
- **Taking a `uint16_t *` to a packed FAT long-name field** is undefined
  behaviour that works on x86 and faults on AArch64.
  `-Waddress-of-packed-member` caught it before the ARM64 skeleton
  existed.

### Known issues

- **The first byte typed into the serial port can be lost.** Enabling the
  16550 FIFO resets the receiver, discarding a byte that arrives during
  initialisation. Three hypotheses were tested and none fixed it; the
  comment in `drivers/char/serial.c` says what is actually happening
  rather than claiming a fix. Press Enter once before the first command.
- **`fork()` returns `-ENOSYS` deliberately.** Copy-on-write is not
  implemented, and a `fork()` that eagerly copies every page would be a
  trap for anything that used it. See
  [docs/DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md).
- **No networking, no USB, no NVMe or AHCI, no graphical desktop, no
  installer, no user accounts.** These are Phases 3 to 5. HavenDOS has a
  network stack and a desktop; HavenOS does not, and nothing was ported,
  because a 32-bit single-tasking stack is not a starting point for a
  64-bit preemptive one.
- **Long file names on FAT are read-only.** New files get 8.3 names.
- **Unprivileged ATA PIO only.** Transfers are synchronous and
  single-sector, which is correct and slow.

[0.1.0]: https://github.com/MGKFN/HavenOS/releases/tag/v0.1.0
