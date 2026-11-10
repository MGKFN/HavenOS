# HavenOS 0.1 technical audit

**Date:** 2026-11-10 · **Audited commit:** `1835fc3` (tag `v0.1.0`, "Foundation")
**Branch with fixes:** `dev/0.1-audit` · **Scope:** the whole repository

This is the audit called for by the HavenOS master development brief. Every
statement about behaviour below was observed by building the tree and booting
it under QEMU 8.2 (BIOS and OVMF/UEFI, `pc` and `q35` machines, 1–8 CPUs,
8 MiB–5 GiB RAM, IDE and VirtIO disks, default and `-cpu max` CPU models).
Where something was only read in the source and not exercised, it says so.

## 1. Summary

HavenOS 0.1.0 is a real 64-bit kernel, not a demo: about 27,800 lines of C
and assembly, cleanly layered, heavily commented, with a host test suite
(842 checks), a portability checker and an in-kernel self-test suite. It
boots reliably on every configuration tried, in both firmware modes.

**Ring 0 is solid. Ring 3 did not work at all.** The release notes list
"a real user/kernel boundary" and 30 system calls as verified, but no
program had ever been run in user mode: `/bin` is empty, the self-tests
never leave ring 0, and the first attempt to run one exposed a chain of
defects, each hidden behind the one before it:

| # | Defect | Effect as shipped |
|---|---|---|
| 1 | `syscall_entry` loaded the per-CPU block into `RAX` before saving it | Every system call was dispatched as call number `0xffffffff8044e1a0` and returned `-ENOSYS` |
| 2 | `copy_bytes()` fixup pad assembled on top of the loop's exit label | `copy_from_user`/`copy_to_user` never returned; first pointer-taking syscall hung the machine |
| 3 | Saved thread context and TSS `rsp0` were the same 8 bytes | Preempting any user thread overwrote its `SS`; `iretq` raised `#GP`, kernel panic |
| 4 | `isr_common` tested trap-frame slot 17 (RIP) instead of 18 (CS) for `SWAPGS` | Latent; masked because both GS bases are equal |
| 5 | `CR3` loaded once per process, never on context switch | Two live processes ran on each other's address space |
| 6 | No FPU/SSE state in the context switch | Two processes resumed with each other's XMM registers and corrupted their own memory |
| 7 | One list node used for both `all_threads` and a process's thread list | `ps` looped forever after any program had run |
| 8 | `sys_exit` called `thread_exit()`, not `proc_exit()` | Exited programs stayed "running" with files open |
| 9 | Kernel-spawned processes got no fds 0–2 | All program output silently discarded (`-EBADF`) |
| 10 | Reaping freed page tables but not pages, threads or kernel stacks | Every program leaked all of its memory |
| 11 | Boot threads idled at normal priority | Compute-bound work got ~50% of a CPU |
| 12 | VirtIO flush sent a zero-length descriptor | `sync` on any VirtIO disk timed out (5 s), device left broken |

All twelve are fixed on `dev/0.1-audit`, and the suite now runs a real
ring-3 program on every boot test (section 6). Defect 6 was found *by* that
new test, which is the argument for it.

The lesson for the project is not that the kernel is poor — it is unusually
careful — but that **"verified" was claimed for a path with no test**. Rule
11 of the brief exists for exactly this.

## 2. Current architecture

| Area | What exists |
|---|---|
| Target | x86-64 only. `arch/arm64` is a deliberate skeleton (30 of 62 HAL functions) that keeps the abstraction honest |
| Boot | GRUB 2 + Multiboot2, one hybrid ISO for BIOS and UEFI. Long-mode entry in `boot.S`; higher-half kernel at `0xFFFFFFFF80200000` |
| Kernel type | Monolithic, C11 + GNU assembly, freestanding, `-Werror`, no C++ |
| HAL | 62 functions in `include/haven/arch.h`; generic code is machine-checked to contain no arch-specific code |
| Physical memory | Buddy allocator, `struct page` array, DMA/DMA32/NORMAL zones |
| Virtual memory | 4-level paging; direct map, 16 TiB kernel arena, MMIO window, guarded kernel stacks; W^X on the kernel image |
| Heap | Size-class slab allocator with redzones and poisoning |
| Interrupts | GDT/TSS with IST stacks, 256-vector IDT, 8259 PIC for device IRQs, Local APIC for the timer and IPIs |
| Time | TSC calibrated against the PIT; LAPIC timer tick at 100 Hz |
| Scheduling | Preemptive fixed-priority round-robin, 8 bands, per-CPU run queues |
| SMP | ACPI MADT, AP trampoline, per-CPU data via `GS` |
| Processes | Per-process page tables, fd tables, `brk`, `mmap`; ELF64 static loader |
| Syscalls | `SYSCALL`/`SYSRET`; 30 numbers assigned, 26 implemented |
| Filesystems | VFS with mount table; ramfs (root), devfs, FAT12/16/32 |
| Drivers | 16550 serial, PS/2 keyboard, CMOS RTC, PCI enumeration, ATA PIO, VirtIO-blk (legacy), framebuffer/EGA text console |
| Shell | 28-command shell running **inside the kernel** as a kernel thread |
| Build | GNU Make with directory discovery, host toolchain, reproducible stamp header |
| Tests | 842 host checks, portability check, in-kernel suite booted under QEMU |

## 3. What works (observed)

- Boots to a shell under BIOS and UEFI; `pc` and `q35`; 1, 2, 4 and 8 CPUs; 8 MiB to 5 GiB.
- Memory management: all allocator self-checks pass; W^X applied; identity map dropped.
- Preemption, sleeping, priorities and timers in ring 0.
- SMP bring-up: 8 of 8 processors online.
- ATA and VirtIO disks detected; MBR parsed; FAT volume written by `mtools` mounted, read and written.
- ramfs/devfs/VFS operations from the kernel shell.
- Host suite 842/842; portability check passes.
- **After the fixes:** ELF programs run in ring 3, make system calls, are preempted, are isolated from the kernel and from each other, and are fully reclaimed on exit. Verified with SMEP/SMAP on (`-cpu max`).

## 4. Broken or incomplete

Fixed on this branch: the twelve defects in section 1.

Still open (none of these block booting; all are on the roadmap):

**Process model**
- No way for a user program to create a process: `fork` is `-ENOSYS` by design and there is no `spawn` system call. `proc_spawn()` is kernel-only.
- `execve` ignores `argv`/`envp` from user space.
- `pipe`, `thread_create`, `thread_join` are `-ENOSYS`.
- `kill` logs that it terminates the target but only wakes its threads. There are no signals.
- Processes whose parent exits are re-parented to "pid 1", which does not exist; they are never reaped. Kernel-started processes are reaped only because `run` now waits.
- No user-space runtime beyond the new `user/` crt0: no libc, no dynamic linking, no init, no programs.

**Concurrency (read in the source, not yet reproduced)**
- Wait queues can lose a wake-up: `proc_wait()` and `thread_join()` test a condition, drop the lock, then call `sched_block_on()`; `sched_wake_all()` on a still-empty queue leaves no trace. A blocked `waitpid` can sleep forever.
- `thread_join()` may free a kernel stack that the exiting thread is still running on, if joiner and exiter are on different CPUs. Not reachable today only because threads never migrate.
- System calls run with interrupts disabled for their entire duration (`SFMASK` clears `IF` and nothing sets it again). Long calls stall the tick.
- No TLB shootdown. Safe today only because every process has one thread.
- No load balancing: a thread runs on the CPU that created it, so secondary CPUs come online and stay idle.

**Memory**
- A process's page table copies the kernel half of the PML4 at creation. A top-level kernel entry created *later* would be missing from existing processes. Not reachable until a kernel region grows past its first 512 GiB slot, but it is a trap.
- No copy-on-write, no demand paging, no shared or file-backed mappings, no swap.
- The 2 MiB contiguous-DMA self-test fails on an 8 MiB machine (a test assumption, not a kernel fault).

**Devices**
- I/O APIC is detected and unused; device interrupts go through the legacy PIC, which limits real-hardware support.
- ACPI: MADT only. No power management, no `_PRT` interrupt routing, no clean shutdown.
- No AHCI, NVMe, USB, mouse, network card or audio driver. On `q35` (and most real machines) **no disk is found at all**.
- Block I/O is polled and synchronous; ATA is single-sector PIO.
- The link-time driver registry holds one driver (VirtIO); everything else is still called by name from `kernel_main()`.
- No framebuffer is requested from GRUB: BIOS boots get 80×25 EGA text, UEFI boots get whatever GOP mode the firmware left (800×600 under OVMF).

**Filesystem and security**
- FAT long file names are read-only; new files get 8.3 names.
- No buffer cache.
- Ownership and mode bits are stored but never checked. Everything runs as uid 0. There are no user accounts.
- `/dev/random` is a clock-seeded PRNG (the kernel log says so).
- No ASLR, no stack protector, no KASLR.

**Known and documented upstream:** the first byte typed on the serial port can be lost.

## 5. Critical architectural weaknesses

In priority order:

1. **There is no user space.** The shell, the file commands and every diagnostic live in ring 0. Phases 5–8 of the brief (commands, graphics, applications, networking) all assume programs, and there is currently nothing between "kernel" and "ELF file on a disk". This is the single largest gap.
2. **The process model cannot grow programs.** Without `spawn` (or CoW `fork`), `argv`, `pipe`, `wait` that cannot hang, and signals, a user-space shell cannot be written.
3. **Blocking primitives are racy by construction** (lost wake-ups). Everything that sleeps is built on them, so this has to be fixed before more code depends on it.
4. **Interrupt delivery is PC-AT legacy.** PIC-only routing and no ACPI interrupt model will not survive contact with real post-2010 hardware, and the brief's final objective is real hardware.
5. **Storage is limited to what QEMU's oldest machine type emulates.** AHCI is the minimum for real disks.
6. **The syscall ABI is unversioned and untested from outside.** Now that there is a user tree, the numbers are shared from one header; the calling convention needs the same treatment (errno values and open flags are still duplicated).

What is *not* a weakness: the layering, the HAL, the memory managers, the VFS shape, the build system and the test infrastructure are good foundations and should be kept.

## 6. What was changed in this audit

Seven commits on `dev/0.1-audit`, each building and passing `make check`:

1. `x86_64: stop SYSCALL entry destroying the call number; test CS, not RIP, for SWAPGS`
2. `x86_64: fix copy_from_user/copy_to_user spinning forever`
3. `sched: make user threads preemptible and switch address spaces`
4. `proc: make exit, reaping and standard I/O actually work`
5. `Run a real ring-3 program in the self-test suite; save FPU/SSE state per thread`
6. `virtio-blk: send a flush as two descriptors, and only when the device has a cache`
7. `ci: boot-test every push; audit report and roadmap`

New in the tree:

- `user/` — crt0, linker script, system-call wrappers, and `user/selftest`, the user half of the self-test suite.
- `include/haven/abi/syscall_nr.h` — call numbers shared by kernel and user code.
- `test_user_mode()` in `kernel/test/selftest.c` — 9 checks that run a real program: system calls, rejected pointers, heap, `mmap`, files, register preservation across a syscall and across preemption, two address spaces side by side, three fatal protection violations, and a leak check.
- `.github/workflows/ci.yml` — `make check` plus UEFI, 4-CPU, VirtIO and SMEP/SMAP boot tests on every push.
- `scripts/run-selftest.sh` stops QEMU when the verdict appears (a few seconds per run instead of two minutes) and treats a panic as a failure.

No existing functionality was removed. One behaviour changed: the shell's `run` now waits for the program and reports its exit status instead of returning immediately.

### Evidence

| Check | Before | After |
|---|---|---|
| Host tests | 842/842 | 842/842 |
| In-kernel suite, BIOS + IDE | 82/82 | 91/91 |
| In-kernel suite, UEFI + IDE | passed | 91/91 |
| In-kernel suite, 4 CPUs, `-cpu max` | 82/82 | 91/91, 8 consecutive runs |
| In-kernel suite, VirtIO disk | **81/82** (sync timeout) | 91/91 |
| Ring-3 program makes a system call | **no** (`-ENOSYS`, then hang, then panic) | yes |
| Two ring-3 programs at once | **no** | yes |
| Pages leaked per process | all of them | 0 |

Not tested: real hardware (rule 10: QEMU first), VMware, and AArch64.

## 7. Bootloader evaluation (Limine vs. the current GRUB/Multiboot2)

The brief asks for Limine to be evaluated. Recommendation: **keep GRUB + Multiboot2 for 0.1.**

- The existing path works under BIOS and UEFI today and is covered by tests; replacing it is all risk and no new capability for 0.1.
- The firmware handover is already isolated behind `struct boot_info`, so the kernel does not depend on Multiboot2 outside `arch/x86_64/boot/`. A second boot protocol can be added later without touching anything else.
- Limine would give a higher-half entry in long mode, a framebuffer and an SMP start for free — all things HavenOS has already written and tested itself.
- The one real gap, a guaranteed linear framebuffer under BIOS, is a Multiboot2 header tag, not a new bootloader.

Revisit when Secure Boot or a HavenOS-branded boot experience matters. This is a decision for the project owner, so nothing was changed.

## 8. Missing subsystems and estimated complexity

Sizes are relative effort for this codebase: **S** days, **M** one to two weeks, **L** several weeks, **XL** months.

| Subsystem | Size | Depends on |
|---|---|---|
| Race-free wait queues, IRQs on during syscalls | S | — |
| `spawn`, `argv`, `waitpid` for orphans, init process | M | wait queues |
| Pipes, `dup2`, basic signals (`kill` that kills) | M | process model |
| Minimal libc (`user/libc`): stdio, string, malloc, errno | M | syscall ABI |
| User-space shell and core utilities | M | libc, spawn, pipes |
| Scheduler load balancing, safe cross-CPU thread reaping | M | — |
| I/O APIC + ACPI interrupt routing | M | — |
| ACPI shutdown/reboot (AML subset or ACPICA port) | L | — |
| AHCI driver, interrupt-driven block layer, buffer cache | L | I/O APIC |
| FAT long-name writes | S | — |
| Native filesystem (after FAT is solid) | XL | buffer cache |
| PS/2 mouse | S | — |
| Framebuffer request, graphics primitives, font rendering | M | — |
| Window manager / compositor, desktop | XL | user space, mouse, IPC, shared memory |
| USB (xHCI, HID, mass storage) | XL | I/O APIC, MSI |
| Network: NIC driver (e1000/virtio-net), Ethernet, ARP, IPv4, UDP, TCP, DNS, sockets | XL | interrupt-driven I/O |
| Users, permission checks, login | M | user space |
| CoW `fork`, demand paging, `mmap` of files | L | — |
| Application packaging and installation | L | native FS, users |

The recommended order is in [ROADMAP.md](ROADMAP.md).
