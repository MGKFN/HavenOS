# HavenOS handoff

The state of the tree, for whoever picks it up next — including a fresh
chat. Read this first, then [docs/AUDIT.md](docs/AUDIT.md) and
[docs/ROADMAP.md](docs/ROADMAP.md).

## Current release

**HavenOS 0.1.1 "Foundation"**, 2026-11-10. Tag `v0.1.1` on `main`.

| Release | Date | What it was |
|---|---|---|
| 0.1.1 | 2026-11-10 | User mode actually works; 12 defects fixed; ring-3 test in the suite; `/bin/hello`; CI on every push |
| 0.1.0 | 2026-10-14 | First release. Kernel foundation. Claimed user mode; it did not work |

## What it is

A 64-bit monolithic kernel for x86-64 in C and assembly, booted by GRUB
(Multiboot2) from one ISO under BIOS and UEFI. Preemptive SMP scheduler,
per-process address spaces, `SYSCALL` interface, VFS with ramfs, devfs
and FAT, ATA and VirtIO disks, PS/2 keyboard, serial and framebuffer
consoles. The shell runs inside the kernel. One user program ships:
`/bin/hello`.

It is **not** a Linux distribution and shares no code with one.

## Building and testing

```sh
make                 # build/havenos.elf
make test            # host tests
make check           # host tests + portability + build + BIOS boot test
make iso             # build/havenos-<ver>.iso
make release         # everything, verified, into dist/
python3 tools/make_release_bundle.py    # dist/HavenOS-<ver>-bundle.zip
```

Needs: `gcc binutils make python3 zip grub-pc-bin grub-efi-amd64-bin
xorriso mtools qemu-system-x86 ovmf`. Without `grub-pc-bin` the ISO has no
BIOS boot image.

More boot tests, each of which has caught a real bug:

```sh
./scripts/run-selftest.sh uefi
CPUS=4 ./scripts/run-selftest.sh
DISK_IF=virtio CPUS=2 ./scripts/run-selftest.sh
CPU_MODEL=max CPUS=2 ./scripts/run-selftest.sh     # SMEP + SMAP
```

## Testing totals at 0.1.1

| Suite | Result |
|---|---|
| Host tests | 842 checks, 7 suites |
| In-kernel self-tests | 91 checks, 14 tests (72 with no disk attached) |
| User-mode program's own checks | 15 |
| Portability | 62 HAL entry points × 2 architectures |

Everything is verified under QEMU 8.2 only. **Never booted on real
hardware or VMware.**

## Rules this project works by

From the master development brief:

- Inspect before changing; preserve what works; no destructive rewrites without approval.
- Work on a branch, commit milestones separately, compile after every significant change.
- **Never claim a feature works without evidence.** 0.1.0 broke this one and it cost a release.
- QEMU before real hardware.
- No placeholder implementations presented as finished.

## Known open problems

The full list is AUDIT.md section 4. The ones most likely to bite:

- A user program cannot start another program (no `spawn`; `fork` is `-ENOSYS` on purpose).
- No pipes, no signals; `kill` does not kill.
- `waitpid` and `thread_join` can miss a wake-up and sleep forever (read in the source, not reproduced).
- System calls run with interrupts off.
- Secondary CPUs come online but get no work (no load balancing).
- No AHCI, NVMe, USB, mouse or network driver: most real PCs will show no disk.
- No I/O APIC use; ACPI is MADT only; no power-off.
- Permissions are stored and never checked. No user accounts.
- FAT long names are read-only.
- The first byte typed on the serial console can be lost: press Enter once.

## What to do next

Roadmap milestone **0.1.2 — a process model you can build a shell on**:

1. Race-free blocking (prepare-to-wait), interrupts on during system calls.
2. `SYS_spawn(path, argv, envp)`; `execve` with real `argv`.
3. `/bin/init` as pid 1, reaping orphans.
4. Pipes and `dup2`.
5. `kill` that kills; minimal signals.
6. Load balancing and safe cross-CPU thread reaping.
7. Shared ABI headers in `include/haven/abi/`.

Then 0.1.3: a libc, a user-space shell and core utilities.

## Where things are

| Path | Contents |
|---|---|
| `arch/x86_64/` | boot, CPU, paging, APIC, SMP, syscall entry |
| `kernel/` | memory, scheduler, processes, syscalls, shell, self-tests |
| `fs/`, `drivers/`, `lib/` | filesystems, drivers, freestanding C library |
| `user/` | user runtime (`lib/`), `hello`, and the ring-3 self-test |
| `include/haven/abi/` | what kernel and user code share |
| `tests/host/` | host-side unit tests |
| `docs/` | audit, roadmap, architecture, design decisions, guides |
