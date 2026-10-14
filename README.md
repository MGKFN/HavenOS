# HavenOS

HavenOS is a modern bare-metal operating system based on and inspired by HavenDOS. Built from the ground up for performance, security, scalability, and direct hardware control, HavenOS aims to provide a clean, modern computing platform free from unnecessary legacy limitations.

## Vision

HavenOS combines the proven concepts of HavenDOS with modern operating system architecture, including:

- 64-bit native kernel
- UEFI boot support
- Virtual memory management
- Preemptive multitasking
- Multi-core processor support
- Modular driver framework
- Modern file systems
- Native networking stack
- Desktop and server capabilities
- Secure user and kernel separation

## Goals

- Fast boot times
- High reliability
- Low resource usage
- Modern development environment
- Open-source community driven development
- Long-term maintainability

## Project Status

HavenOS is currently under active development.

## Inspiration

This project uses the HavenDOS project as a primary architectural and design reference. Existing HavenDOS source code, concepts, and implementation details may be used where appropriate while modernizing the platform for future hardware and software requirements.

## Development Roadmap

### Phase 1
- UEFI Bootloader
- Memory Management
- Interrupt Handling
- Basic Scheduler

### Phase 2
- Process Management
- System Calls
- ELF Loader
- Virtual File System

### Phase 3
- Storage Drivers
- USB Support
- Networking Stack

### Phase 4
- Graphics System
- Window Manager
- Desktop Environment

### Phase 5
- Security Hardening
- Package Management
- SDK and Developer Tools

## Building

HavenOS builds with a stock x86_64 Linux toolchain — no cross-compiler is
required, because the kernel is compiled freestanding for the host's own
architecture.

```bash
make            # build build/havenos.elf
make test       # build and run the host unit-test suite
make iso        # build a BIOS+UEFI bootable ISO (needs grub-mkrescue + xorriso)
make run        # boot the ISO under QEMU (needs qemu-system-x86_64)
```

Requirements: GCC 12+ or Clang 15+, GNU binutils, GNU Make, Python 3.8+.
Optional, for images and emulation: `grub-pc-bin`, `grub-efi-amd64-bin`,
`xorriso`, `mtools`, `qemu-system-x86`.

See [docs/DEVELOPER.md](docs/DEVELOPER.md) for the full build, test and
debugging workflow.

## Running

```bash
make run              # BIOS boot under QEMU
make run-uefi         # UEFI boot under QEMU (needs OVMF firmware)
make run-smp CPUS=4   # multi-core boot
```

The kernel also boots on real hardware from the generated ISO. A serial
console on COM1 (115200 8N1) carries the full kernel log, which is the
recommended way to capture a boot on physical machines.

## Repository layout

| Path | Contents |
|---|---|
| `arch/x86_64/` | x86_64 boot path, CPU, paging, APIC, SMP, syscall entry |
| `arch/arm64/` | AArch64 port skeleton (see `docs/PORTING_ARM64.md`) |
| `kernel/` | Architecture-neutral kernel: MM, scheduler, VFS glue, syscalls |
| `fs/` | Virtual filesystem switch, ramfs, devfs, FAT |
| `drivers/` | Modular drivers: console, PCI, block, input |
| `lib/` | Freestanding C library used by the kernel |
| `include/haven/` | Public kernel API headers |
| `tests/host/` | Host-side unit tests that compile the real kernel sources |
| `docs/` | Architecture, API, developer and user documentation |

## Documentation

- [Architecture](docs/ARCHITECTURE.md) — subsystem design and boot flow
- [Design decisions](docs/DESIGN_DECISIONS.md) — what was chosen and why
- [API reference](docs/API.md) — kernel-internal interfaces
- [Developer guide](docs/DEVELOPER.md) — build, test, debug, contribute
- [User guide](docs/USER.md) — booting and using the system
- [ARM64 porting guide](docs/PORTING_ARM64.md) — the AArch64 path
- [HavenDOS heritage](docs/HAVENDOS_HERITAGE.md) — what was inherited and what changed
- [Release notes](RELEASE_NOTES.md) — this release in detail
- [Changelog](CHANGELOG.md) — what changed, and what broke on the way

## Releases

```bash
make release
```

Builds the kernel, runs every test, assembles `dist/havenos-<version>/`
with the source, build system, documentation, licence and bootable ISO,
produces `dist/havenos-<version>.zip` with a SHA-256 sum, and then
verifies the archive: that every source file the build compiles is inside
it, that no build output leaked in, that nothing escapes the top-level
directory, and that the extracted archive actually builds.

## License

See LICENSE file for details.
