# HavenOS user guide

HavenOS 0.1.0 "Foundation" — a 64-bit operating system derived from
HavenDOS.

This guide covers booting the system and using what it provides. It
describes version 0.1.0 as it actually behaves, including the parts that
are not there yet; see [What is not here](#what-is-not-here) before
planning anything around it.

> HavenOS is a bare-metal operating system under active development. It
> is not a replacement for a general-purpose system, it has no user
> account model, and it writes to disks you give it. Use a virtual
> machine, and use an empty virtual disk.

## Contents

- [Getting the system](#getting-the-system)
- [Booting in a virtual machine](#booting-in-a-virtual-machine)
- [Booting on real hardware](#booting-on-real-hardware)
- [The boot menu](#the-boot-menu)
- [Kernel command-line options](#kernel-command-line-options)
- [Reading the boot log](#reading-the-boot-log)
- [The shell](#the-shell)
- [Files and filesystems](#files-and-filesystems)
- [Running a program](#running-a-program)
- [Shutting down](#shutting-down)
- [Hardware support](#hardware-support)
- [What is not here](#what-is-not-here)
- [Troubleshooting](#troubleshooting)

## Getting the system

Build the ISO from source — there is no prebuilt image for 0.1.0:

```bash
git clone https://github.com/MGKFN/HavenOS.git
cd HavenOS
make iso
```

This produces `build/havenos-0.1.0.iso`, a single hybrid image that boots
on both legacy BIOS and 64-bit UEFI firmware. The build needs a stock
Linux toolchain plus `grub-pc-bin`, `grub-efi-amd64-bin` and `xorriso`;
[DEVELOPER.md](DEVELOPER.md) lists the exact requirements.

## Booting in a virtual machine

The fastest path, with the kernel log on your terminal:

```bash
make run
```

That is equivalent to:

```bash
qemu-system-x86_64 -m 512 -cdrom build/havenos-0.1.0.iso -boot d \
    -serial stdio -display none
```

Other ready-made configurations:

| Command | What it does |
|---|---|
| `make run` | BIOS boot, serial console on your terminal |
| `make run-graphic` | BIOS boot with a QEMU display window |
| `make run-uefi` | UEFI boot through OVMF firmware |
| `make run-smp CPUS=4` | four processors |
| `make run-selftest` | boot, run the in-kernel test suite, report, exit |

To give it a disk, attach any raw image. HavenOS reads MBR partition
tables and mounts FAT12, FAT16 and FAT32 volumes:

```bash
qemu-img create -f raw disk.img 64M
qemu-system-x86_64 -m 512 -cdrom build/havenos-0.1.0.iso -boot d \
    -serial stdio -display none \
    -drive file=disk.img,format=raw,if=ide
```

A VirtIO disk works too — use `if=virtio` instead of `if=ide`.

Memory: the system boots and reaches a shell in 8 MiB, which is the
smallest configuration tested; 512 MiB is what it is normally tested
with. There is no hard-coded RAM ceiling — the physical allocator is
built from the firmware's memory map, so it uses what the machine
reports.

## Booting on real hardware

Write the ISO to a USB stick and boot it:

```bash
sudo dd if=build/havenos-0.1.0.iso of=/dev/sdX bs=4M status=progress oflag=sync
```

Replace `/dev/sdX` with the whole device, not a partition, and check twice
— `dd` will overwrite whatever you point it at.

The same image boots under legacy BIOS/CSM and under 64-bit UEFI. Secure
Boot is not supported: the image is not signed, so Secure Boot has to be
turned off in firmware settings.

**The serial console is the way to capture a boot on real hardware.**
HavenOS writes its entire log to COM1 at 115200 8N1 as well as to the
screen, and the log is what makes a report useful. On a machine with no
serial port, photograph the screen; the display console shows the same
text.

HavenOS requires a 64-bit (x86_64) processor. On a 32-bit CPU the boot
code detects the absence of long mode and prints an explanation to both
the screen and COM1 instead of faulting — HavenDOS ran on those machines
and HavenOS does not, so the message says so rather than leaving a blank
screen.

## The boot menu

GRUB offers seven entries. The titles carry the running version, because
they are generated from `include/haven/version.h` at build time rather
than written by hand.

| Entry | Effect |
|---|---|
| **HavenOS 0.1.0 (Foundation)** | normal boot |
| **verbose boot log** | `loglevel=debug` — every subsystem's detail, including the full firmware memory map |
| **single CPU (no SMP)** | `nosmp` — do not start the other processors |
| **run kernel self-tests and report** | `selftest loglevel=debug` — run the in-kernel suite and print a verdict |
| **serial console only** | `console=serial` — leave the firmware's display alone |
| **Reboot** | restart the machine |
| **Halt** | power off |

Press `e` on any entry to edit its command line before booting, which is
how to combine options that have no menu entry of their own.

## Kernel command-line options

Four options are understood. Each is matched as a whole word, so
`nosmp` is not found inside `nosmpfoo` and `selftest` is not found inside
a path such as `/boot/selftest.elf`.

| Option | Meaning |
|---|---|
| `nosmp` | Run on the boot processor only. Use this to tell an SMP problem from an everything-else problem. |
| `selftest` | Run the in-kernel test suite after boot and report a verdict. |
| `loglevel=<level>` | Log threshold: `panic`, `err`, `warn`, `info`, `debug`, or `quiet` (equals `warn`), or the digits `0`–`4`. The default is `info`. |
| `console=<where>` | `serial` leaves a firmware framebuffer untouched and logs only to COM1; `both` or `fb` (the default) uses every console found. |

An unrecognised value warns and is ignored rather than stopping the boot.
Messages below the threshold are still recorded in the kernel's ring
buffer, so `dmesg` in the shell shows them even when they never reached a
console.

## Reading the boot log

The log is the system's primary interface in 0.1.0. Every line carries a
monotonic timestamp and a level:

```
[    0.000000]  INFO  HavenOS x86_64 kernel starting
[    0.000000]  INFO  boot: long mode entered, higher half active at 0xffffffff8021c2e0
[    0.000000]  INFO  boot: loader 'GRUB 2.12-1ubuntu7.3', firmware BIOS
[    0.030327]  INFO  time: TSC 2100.106 MHz (calibrated against PIT, median of 3)
[    0.039451]  INFO  memory: firmware reported 10 regions, 511 MiB usable
[    0.055381]  INFO  pmm: 512 MiB RAM in 131072 pages, 505 MiB usable, 6 MiB reserved
[    0.076738]  INFO  vmm: identity map dropped; 24 page-table pages in use
[    0.096346]  INFO  vmm: kernel image permissions applied (W^X enforced)
[    0.140890]  INFO  kernel: interrupts enabled; the system is now preemptive
```

The timestamp is derived from the TSC where the CPU has an invariant one
and from the PIT otherwise; a warning line says which. Each subsystem
prefixes its own name, so `pmm:`, `vmm:`, `sched:` and `vfs:` identify
where a line came from.

A boot ends with a summary:

```
[    0.389127]  INFO  kernel: boot complete; entering the idle loop
[    0.390244]  INFO  kernel: 1 CPU online, 511 MiB RAM, scheduler running
```

If the log stops before that line, the last subsystem named is where to
look.

## The shell

The shell starts automatically on the last line of boot and reads from
the serial port and the PS/2 keyboard alike. It exists to inspect and
exercise the kernel.

```
haven:/$ help
```

### Looking at the system

| Command | Shows |
|---|---|
| `uname` | version, architecture, build date and git revision |
| `meminfo` | physical, virtual and object allocator use, plus an integrity check |
| `cpuinfo` | processors, clock, uptime, per-CPU switch counts |
| `ps` | threads and processes with priorities, states and CPU time |
| `lsblk` | block devices and their I/O counters |
| `lsdev` | devices found on the PCI bus and which driver claimed each |
| `lsirq` | interrupt lines, their handlers and their counts |
| `mount` | mounted filesystems and VFS statistics |
| `dmesg` | the kernel log ring buffer, including messages below the log level |
| `date` | wall-clock time from the hardware clock |

```
haven:/$ meminfo
physical memory
  total            131072 pages       512 MiB
  free             129319 pages       505 MiB
  allocated            79 pages         0 MiB
  reserved           1674 pages         6 MiB
  requests            126 alloc, 47 free, 0 failed
virtual memory
  mapped               50 pages
  arena used          264 KiB of 16 TiB
  page tables          30 pages
kernel allocator
  live                 27 objects, 18192 bytes
  slab pages           32
  requests             45 alloc, 18 free, 0 failed
integrity      physical consistent, objects consistent
```

That last line is worth knowing about. It walks every buddy free list and
every live allocation header, so it answers "is memory corrupted" with a
check rather than an opinion.

### Working with files

| Command | Does |
|---|---|
| `ls [path]` | list a directory |
| `cd <path>` / `pwd` | change / print the working directory |
| `cat <file>` | print a file |
| `hexdump <file>` | print a file as hex and ASCII |
| `write <file> <text…>` | write text to a file, creating it |
| `mkdir <path>` | create a directory |
| `rm <path>` | delete a file or an empty directory |
| `sync` | flush every filesystem to its disk |
| `echo <text…>` | print arguments |

### Other commands

| Command | Does |
|---|---|
| `selftest` | run the in-kernel test suite now |
| `run <file>` | load and run an ELF64 executable |
| `sleep <ms>` | block the shell, which is a way to watch preemption in `ps` |
| `history` | recent command lines |
| `reboot` / `halt` | flush filesystems, then restart or stop |
| `exit` | leave the shell; the kernel keeps running |

Line editing is deliberately minimal: backspace, `Ctrl-U` to erase the
line, `Ctrl-C` to abandon it, and `Ctrl-D` on an empty line to exit.
Arrow keys are not interpreted — `history` lists recent commands but
nothing recalls them for you yet.

## Files and filesystems

A fresh boot has three or more mounts:

```
haven:/$ mount
vfs: 3 mounts, 0 open files
vfs:   /                ramfs
vfs:   /dev             devfs
vfs:   /mnt/hdap1       fat
```

- **`/`** is a RAM filesystem. It is created at boot and lost at
  shutdown. The standard directories `/dev`, `/tmp`, `/etc`, `/bin`,
  `/home`, `/var`, `/proc` and `/mnt` exist, and `/etc/os-release` is
  written at boot from the kernel's own version data.
- **`/dev`** holds the devices the kernel has registered: `console`,
  `null`, `zero`, `random`, `keyboard`, `rtc`, and one entry per block
  device (`hda`, `hdap1`, …).
- **`/mnt/<device>`** is where each FAT volume found on a disk is
  mounted, named after the block device it came from — `hda` for a whole
  disk, `hdap1` for its first partition. Every block device is probed at
  boot and every volume that looks like FAT is mounted; nothing has to be
  configured, and a machine with no disk simply has no `/mnt` entries.

FAT12, FAT16 and FAT32 are all supported for reading and writing,
including files that span clusters. Long file names are readable but new
files get 8.3 short names. `/` and `/dev` are not persistent: anything
that must survive a reboot belongs on a FAT volume, and should be
followed by `sync`.

## Running a program

`run` loads a 64-bit static ELF executable into a fresh address space and
enters it in user mode:

```
haven:/$ run /mnt/hdap1/hello.elf
```

The program gets its own page tables, a user stack, and a 30-call system
interface — `write`, `read`, `open`, `close`, `exit`, `brk`, `mmap`,
`nanosleep`, `getpid` and the rest. It cannot read or write kernel memory:
the kernel runs with SMEP and SMAP enabled where the CPU has them, and
every pointer a program passes to a system call is validated and copied
rather than used in place.

There is no compiler on HavenOS and no C library for it yet, so programs
have to be built elsewhere. `fork()` is deliberately absent and returns
`-ENOSYS` — see [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) for why an
honest error is better than a half-working implementation.

## Shutting down

```
haven:/$ sync
haven:/$ halt
```

`halt` and `reboot` both flush every filesystem first, so `sync` is
belt-and-braces rather than required. Closing a virtual machine window
without either is safe only if nothing was written.

## Hardware support

| Area | 0.1.0 |
|---|---|
| Processor | x86_64 with long mode; SMP up to 64 CPUs via ACPI |
| Firmware | BIOS and 64-bit UEFI, one image |
| Memory | the firmware memory map, any size; zones for 32-bit DMA devices |
| Timers | TSC, PIT, local APIC timer |
| Display | firmware framebuffer or EGA text, as a console |
| Input | PS/2 keyboard, serial console |
| Storage | ATA/IDE in PIO mode, VirtIO block |
| Partitions | MBR |
| Filesystems | FAT12/16/32, ramfs, devfs |
| Buses | PCI with recursive bridge enumeration |
| Clock | CMOS real-time clock |

Devices are listed with `lsdev` whether or not a driver claimed them, so
"HavenOS can see it" and "HavenOS can use it" stay visibly separate —
recognising a controller in a list is not the same as having a driver for
it.

## What is not here

Stated plainly, because a roadmap entry is not a feature:

- **No networking.** There is no network stack and no NIC driver in
  0.1.0. The e1000 that `lsdev` reports under QEMU is identified, not
  driven.
- **No USB.** No host controller driver, so no USB keyboards, and no USB
  storage.
- **No NVMe, no AHCI.** Storage is ATA PIO and VirtIO. A modern SATA or
  NVMe disk will be visible in `lsdev` and unusable.
- **No graphical desktop and no window manager.** The framebuffer is used
  as a text console.
- **No installer.** HavenOS boots from the ISO; it does not install
  itself to a disk.
- **No user accounts, no permissions.** Everything runs with full
  authority. The user/kernel boundary is real and enforced; a *user*
  model is not there yet.
- **No `fork`, no shell scripting, no package management.**

These are the Phase 3–5 items in the [roadmap](../README.md#development-roadmap).
What 0.1.0 delivers is Phases 1 and 2: the boot path, memory management,
interrupts, the scheduler, processes, system calls, the ELF loader and the
virtual filesystem.

## Troubleshooting

**The screen stays blank and nothing happens.**
Capture the serial log — `make run` already shows it, and on hardware a
null-modem cable or a USB serial adapter on COM1 at 115200 8N1 will. The
last subsystem that printed is where the boot stopped.

**"This CPU does not support long mode".**
HavenOS needs a 64-bit processor. HavenDOS 0.8.1 runs on 32-bit machines.

**The boot stops right after GRUB with no HavenOS output at all.**
The kernel never ran. Usually this is a GRUB that loaded the image but
could not use the Multiboot2 handover; `make iso` validates the header on
every build, so a self-built image should not do this. Report it with the
GRUB version.

**UEFI boot shows the menu but not the system.**
Try the "serial console only" entry. If that boots, the problem is in
taking over the firmware's framebuffer, not in the kernel.

**The first character I type is swallowed.**
Known limitation. Enabling the 16550 FIFO resets the receiver, which
discards a byte that arrived during initialisation; it shows up when a
script is piped straight into the serial port. Press Enter once before
the first command. See the comment in `drivers/char/serial.c` — it says
what is actually happening rather than claiming a fix.

**A disk is listed by `lsdev` but not by `lsblk`.**
There is no driver for that controller. See
[Hardware support](#hardware-support).

**`ls /mnt` is empty but the disk has a filesystem.**
HavenOS mounts FAT only, from MBR partitions or whole disks. A GPT disk
or an ext4/NTFS volume is read as "not FAT" and left alone. The boot log
says what each block device was probed as.

**Something crashed and printed a panic.**
A panic prints the reason, the faulting address where there is one, and a
backtrace, to both the screen and the serial port. That text is the whole
bug report. Please include the full log, the firmware mode (BIOS or
UEFI), and the machine or QEMU command line.

## Reporting problems

Bug reports go to [GitHub Issues](https://github.com/MGKFN/HavenOS/issues).
Include the version from `uname`, the firmware mode, the full serial log,
and what you expected instead. For storage problems, include the `lsdev`
and `lsblk` output.
