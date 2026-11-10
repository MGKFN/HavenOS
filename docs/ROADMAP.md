# HavenOS 0.1 development roadmap

Companion to [AUDIT.md](AUDIT.md). The audit says where HavenOS is; this
says what order to build things in and why.

## Where 0.1 stands against the brief's phases

| Phase | Status after the audit branch |
|---|---|
| 1 Kernel foundation | **Done.** Boot, GDT/IDT, exceptions, PMM, VMM, heap, serial logging all verified |
| 2 Multitasking | **Mostly done.** Scheduler, context switch, syscalls, isolation and ring 3 now verified. Missing: process creation from user space, pipes, signals, race-free blocking |
| 3 Storage | **Partly done.** Block layer, ATA, VirtIO, VFS, FAT read/write. Missing: AHCI, buffer cache, long-name writes, persistent configuration |
| 4 Hardware | **Started.** PCI, MADT, LAPIC, keyboard. Missing: I/O APIC, ACPI proper, mouse, USB, real storage controllers |
| 5 Command line | **In the wrong ring.** A capable shell exists, but inside the kernel |
| 6 Graphics | Framebuffer text console only |
| 7 Applications | ELF loader only |
| 8 Networking and security | Not started |

## Principles

1. **Nothing is "done" without a test that boots.** Every milestone below ends with a check in the in-kernel suite or the user suite, run by CI.
2. **User space first.** The next most valuable thing is always whatever lets more code leave ring 0.
3. **QEMU before hardware, then one real machine early.** Real-hardware bring-up finds a different class of bug; do it at 0.1.3, not at 1.0.
4. **Small releases.** Each one below is a few weeks at most and leaves `main` bootable.

## 0.1.1 — "It runs programs" (this branch)

Merge `dev/0.1-audit`. Contents are in AUDIT.md section 6: ring 3 works,
is tested on every push, and nothing leaks.

Before tagging:
- [ ] Review and merge the branch.
- [ ] Correct `RELEASE_NOTES.md`: the 0.1.0 "verified" table claimed a user/kernel boundary that had not been exercised.
- [ ] Bump `include/haven/version.h` to 0.1.1.

## 0.1.2 — A process model you can build a shell on

Goal: a user program can start another program, pass it arguments, wait
for it, and talk to it.

1. **Race-free blocking.** Replace check–unlock–block with a prepare-to-wait primitive (condition re-checked under the scheduler lock). Convert `proc_wait`, `thread_join`, mutexes' slow path. Re-enable interrupts inside system calls. *Test: a stress test of spawn/exit/wait in a loop on 4 CPUs.*
2. **`SYS_spawn(path, argv, envp)`** exposing the existing `proc_spawn()`, with `argv`/`envp` copied in from user space; fix `execve` the same way. `fork` stays `-ENOSYS` until copy-on-write exists — that decision in `DESIGN_DECISIONS.md` is sound.
3. **An init process.** `/bin/init` is pid 1, started by the kernel, and reaps orphans. Remove `proc_reap()`'s polling once init exists.
4. **Pipes and `dup2`.**
5. **`kill` that kills**, with a minimal signal set (TERM, KILL, INT, CHLD) delivered at return to user mode.
6. **Safe thread reaping across CPUs** and a simple load balancer (idle CPUs steal), so SMP does work instead of only coming online.
7. **Move the shared ABI** — errno values, open flags, `struct stat` — into `include/haven/abi/` beside `syscall_nr.h`.

## 0.1.3 — User space exists

Goal: the kernel shell becomes a debugging aid, not the interface.

1. **`user/libc`**: string, stdio (`printf`, buffered files), `malloc` on `brk`/`mmap`, `errno`, `exit`/`atexit`. Write it; do not port one yet — the syscall surface is too small for a port to pay off.
2. **`/bin/sh`** in user space: command lines, `argv`, pipelines, redirection, `cd`, exit status.
3. **Core utilities** as separate programs: `ls cat echo mkdir rm cp mv ps kill uname date hexdump`.
4. **An initial RAM disk** loaded as a Multiboot2 module (`boot_info` already carries modules) so `/bin` is populated at boot without a disk.
5. **A TTY layer**: line discipline, echo, Ctrl-C → SIGINT, so keyboard and serial behave the same for every program.
6. **First real-hardware boot** from USB stick on one known machine, serial log captured. Expect to fix things.

## 0.1.4 — Real machines

1. **I/O APIC** with MADT interrupt source overrides; retire the PIC except as a fallback.
2. **Framebuffer request** in the Multiboot2 header so BIOS boots get graphics too.
3. **AHCI** (the disk controller on essentially every PC since 2008), interrupt-driven.
4. **Buffer cache** in the block layer; **FAT long-name writes**.
5. **Persistent configuration**: `/etc` on disk, mounted at boot from a command-line `root=`.
6. **ACPI power-off and reboot.**
7. **PS/2 mouse.**

## 0.1.5 — Users and permissions

1. Enforce the owner/mode bits the VFS already stores.
2. `setuid`/`getuid`, a passwd file, `login`.
3. User-pointer hardening review now that there is a real attack surface; ASLR for the user stack and `mmap`.

At this point HavenOS 0.1 meets the brief's definition for Phases 1–5:
it boots on real hardware, runs multiple isolated programs, stores files
persistently, and is used through a command line that is itself a program.

## 0.2 — Graphics (Phase 6)

Graphics primitives and font rendering on the framebuffer → shared memory
and a message-passing IPC → a compositor process that owns the framebuffer
→ a window manager → a terminal emulator as the first graphical program.
The compositor belongs in user space; the kernel's part is the framebuffer
device, shared memory, input events and IPC.

## 0.3 — Applications (Phase 7)

Dynamic linking (or stay static and keep it simple), a native executable
and package format built on ELF, per-application permissions, an installer.
Evaluate porting a C library (mlibc or newlib) here, when the syscall
surface can support one.

## 0.4 — Networking (Phase 8)

virtio-net and e1000 → Ethernet/ARP → IPv4/ICMP → UDP → DHCP → TCP → DNS →
a sockets API. An independent stack is a large project; plan for it to be
the longest phase. Security auditing runs alongside from 0.1.5 onward, not
at the end.

## Deferred, deliberately

- **C++ in the kernel.** The brief lists C++ as a language. The kernel is C and has no C++ runtime; adding one buys nothing yet. Use it in user space once libc exists.
- **BIOS-specific work.** BIOS boot already works through GRUB.
- **A native filesystem.** FAT is enough until there are users and permissions to store.
- **USB.** xHCI is months of work; PS/2 and AHCI cover QEMU and many real machines.
- **AArch64.** Keep the skeleton compiling; do not port until x86-64 has user space.
- **Replacing GRUB with Limine.** See AUDIT.md section 7.
