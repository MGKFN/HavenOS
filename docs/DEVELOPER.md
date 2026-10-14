# HavenOS developer guide

## Requirements

HavenOS builds with a **stock x86_64 Linux toolchain**. No cross-compiler
is needed, because the kernel is compiled freestanding for the host's own
architecture.

| Required | Used for |
|---|---|
| GCC 12+ or Clang 15+ | the kernel and the host tests |
| GNU binutils | `ld`, `objcopy`, `nm`, `size` |
| GNU Make 4+ | the build |
| Python 3.8+ | image verification, version generation, test disks |

| Optional | Used for |
|---|---|
| `grub-pc-bin`, `grub-efi-amd64-bin`, `xorriso` | `make iso` |
| `mtools` | the self-test disk image |
| `qemu-system-x86` | `make run`, `make selftest` |
| OVMF (`/usr/share/ovmf/OVMF.fd`) | UEFI boot testing |

On Debian or Ubuntu:

```bash
sudo apt install build-essential python3 \
                 grub-pc-bin grub-efi-amd64-bin xorriso mtools \
                 qemu-system-x86 ovmf
```

## Building

```bash
make                  # build/havenos.elf
make iso              # a BIOS+UEFI bootable ISO
make test             # host unit tests
make selftest         # boot under QEMU and run the in-kernel tests
make check            # everything: host tests, portability, build, selftest
make release          # the full release package and ZIP archive
make clean
make info             # what the build is configured to do
```

Useful variables:

```bash
make V=1              # show every command
make ARCH=arm64       # build the AArch64 skeleton (does not boot yet)
make CPUS=4 run       # QEMU with four processors
make MEM=2048 run     # QEMU with 2 GiB
```

The build discovers sources by directory. **Adding a `.c` file to
`kernel/`, `fs/`, `drivers/` or `arch/$(ARCH)/` is all that is required**
— there is no file list to edit. That is deliberate: HavenDOS listed
every source by hand in one `SRCS_C` variable.

### Why the compiler flags are what they are

| Flag | Reason |
|---|---|
| `-ffreestanding -nostdinc` | no hosted libc; `lib/` provides what the kernel needs |
| `-mno-red-zone` | **mandatory.** An interrupt landing in the ABI's 128-byte red zone would corrupt the frame. |
| `-mcmodel=kernel` | code for the top 2 GiB, matching the higher-half link address |
| `-mno-sse -mgeneral-regs-only` | the kernel uses no vector registers, so no FPU state is saved on entry |
| `-fno-omit-frame-pointer` | makes `panic()`'s backtrace work without DWARF |
| `-fno-stack-protector` | the guard needs TLS, which does not exist this early |
| `-Werror` | see below |
| `-Wframe-larger-than=2048` | a 16 KiB kernel stack has no room for large frames |

`-Werror` is not aspirational. Four real bugs in this kernel were caught
by warnings rather than by testing:

- `-Wframe-larger-than` found a **64 KiB stack buffer on a 64 KiB boot
  stack** in the console's log-replay path;
- `-Waddress-of-packed-member` found FAT long-name fields being
  dereferenced through unaligned `uint16_t *`, which works on x86 and
  faults on AArch64;
- `-Wmissing-prototypes` found two syscall handlers whose real
  implementation would have silently overridden the weak stub through a
  mismatched ABI;
- `-Wmaybe-uninitialized` found a directory-slot allocation whose failure
  path read uninitialised output parameters.

## Testing

### Host tests — `make test`

842 checks. Each suite links the **real** kernel source it exercises,
never a copy:

| Suite | Links | Checks |
|---|---|---|
| `test_string` | `lib/string.c` | 82 |
| `test_printf` | `lib/printf.c` | 59 |
| `test_list` | `include/haven/list.h` | 30 |
| `test_cmdline` | `kernel/cmdline.c` | 33 |
| `test_pmm` | `kernel/mm/pmm.c` | 478 |
| `test_vfs` | `fs/vfs.c`, `fs/ramfs.c` | 116 |
| `test_elf` | `kernel/proc/elf.c` | 44 |

`tests/host/stubs.c` simulates physical memory as one host allocation,
which works only because the kernel reaches physical memory exclusively
through `arch_phys_to_virt()`. `tests/host/stubs_mm.c` provides host
`malloc` and a flat simulated page table for tests of layers *above*
memory management, so a VFS test fails because of the VFS.

Two flags are load-bearing and must not be removed:

- **`-fno-builtin`** — without it GCC replaces `memcpy`/`strncpy` with
  its own builtins, so the test passes while `lib/string.c` never runs.
- **`-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0`** — without it glibc's
  headers redirect `snprintf()` to `__builtin___snprintf_chk()`, so the
  test exercises glibc.

Both were actually happening. `test_printf` now checks `%b`, a HavenOS
extension glibc does not implement, so a wrong link fails loudly.

### In-kernel tests — `make selftest`

82 checks that need a real CPU: whether a context switch returns to the
right stack, whether the timer actually preempts a spinning thread,
whether interrupt-state save/restore nests, whether the FAT driver agrees
with mtools.

`scripts/run-selftest.sh` builds an ISO whose GRUB default is the
self-test entry, builds a partitioned FAT disk with
`tools/make_test_disk.py`, boots under QEMU, and exits non-zero unless
the verdict line appears. The disk is formatted by **mtools, not by
HavenOS**, because a FAT driver tested only against volumes it wrote
itself will agree with its own mistakes.

```bash
make selftest                      # BIOS
./scripts/run-selftest.sh uefi     # UEFI, via OVMF
CPUS=4 ./scripts/run-selftest.sh   # four processors
```

### Portability — `make portability`

Enforces that nothing in `kernel/`, `fs/` or `lib/` contains inline
assembly, an architecture conditional, an architecture-private header or a
port-I/O call, and that both architectures define all 62 HAL entry points
and all eight memory-layout constants.

It has already caught a violation committed by mistake. Run it before
sending a patch.

## Running

```bash
make run              # BIOS, serial on stdio, no display
make run-graphic      # with a QEMU window
make run-uefi         # OVMF
make run-smp CPUS=4
```

The serial console carries the full kernel log. On real hardware that is
the recommended way to capture a boot: COM1, 115200 8N1.

To drive the shell from a script:

```bash
printf 'uname\nmeminfo\nls /dev\nexit\n' | \
  qemu-system-x86_64 -m 512 -cdrom build/havenos-*.iso -boot d \
    -serial stdio -display none -no-reboot
```

One byte of the first command is lost — the UART receiver is reset when
the FIFO is enabled, on real 16550 hardware as well as under QEMU.
Interactive use never sees it. Prefix a newline if it matters.

## Debugging

### GDB

```bash
qemu-system-x86_64 -m 512 -cdrom build/havenos-*.iso -boot d \
  -serial stdio -display none -s -S &
gdb build/havenos.elf -ex 'target remote :1234'
```

The ELF keeps full debug information; only the loadable segments go into
the image.

### Reading a panic

```
*** HAVENOS KERNEL PANIC ***
  virt_to_phys: 0xffffc000000167c0 is not in a fixed-offset kernel window;
                use arch_pt_translate() for mapped memory
  at arch/x86_64/mm/phys.c:42 in arch_virt_to_phys()
  cpu 0
  backtrace:
    [0] 0xffffffff80201f6a
    ...
```

Resolve a backtrace address with:

```bash
addr2line -e build/havenos.elf -f -p 0xffffffff80201f6a
```

Every panic goes to **every** registered console sink, so a serial
capture survives a dead display.

### When a boot produces no output at all

That means the failure is before `serial_early_init()`, which is a very
short list: a malformed Multiboot2 header (run
`tools/verify_multiboot2.py build/havenos.elf`), a CPU without long mode
(the trampoline prints to EGA memory and COM1 for exactly this case), or
a triple fault in the trampoline. QEMU's `-d int,cpu_reset` shows the
last.

A boot that stops *mid-log* after the SMP trampoline message is an
application processor triple-faulting — which resets the whole machine.
That exact symptom happened during development; see the identity-map
comment in `arch/x86_64/smp/smp.c`.

## Conventions

- **Tabs** for indentation, 8 columns, 80-column lines.
- `snake_case` for functions and variables, `SCREAMING_CASE` for macros,
  `subsystem_verb()` for names (`pmm_alloc_page`, `vfs_resolve`).
- Return 0 or a positive count on success, `-HAVEN_E*` on failure.
  Never a bare `-1`.
- Use the bounded string routines. `strcpy` and `strcat` are not
  declared.
- A function that can fail returns a value the caller can distinguish.
  "Not implemented" is `-ENOSYS`, not 0.
- Comments explain **why**, not what. A comment that says what the code
  does is noise; one that says why a bound is 2048 or why the order of
  two writes matters is the most valuable thing in the file.

### Where to put a comment

The convention in this tree is that non-obvious decisions are commented
*at the point of the decision*, and the comment says what would go wrong
otherwise. For example:

```c
/* Acknowledge after the handlers have run.  Acknowledging first would
   let a level-triggered device re-assert before its handler has cleared
   the condition, producing an interrupt storm.  It must also happen
   BEFORE any context switch below, or the controller stays blocked for
   as long as the next thread runs. */
arch_irq_eoi(irq);
```

## Adding things

### A driver

Create a file under `drivers/`, fill in a `struct driver`, and register
it:

```c
static bool my_match(const struct device *dev)
{
	return dev->id.pci.vendor == 0x1234 &&
	       dev->id.pci.device == 0x5678;
}

static int my_probe(struct device *dev)
{
	pci_enable_device(dev, true);
	/* ... */
	return 0;
}

static const struct driver my_driver = {
	.name = "my-device", .bus = BUS_PCI,
	.match = my_match, .probe = my_probe,
};

HAVEN_DRIVER(my_driver);
```

That is all. The bus probes it; nothing else needs editing.

A `probe` that fails must leave the device exactly as it found it,
because another driver may be offered the same device next.

### A filesystem

Fill in a `struct vfs_ops` and call `vfs_mount()`. Your `->lookup()`
receives a **single name component** with no separator in it — the VFS
walks paths. Leave an operation NULL if you do not implement it; the VFS
returns `-ENOSYS`.

### A system call

Append a number in `include/haven/syscall.h` (never renumber), add a
`SYSCALL_DECLARE`, add a table entry, and implement it. Use
`copy_from_user()` and `strncpy_from_user()` — never dereference a user
pointer.

### A test

Host test: add to `tests/host/`, link the real source, add it to `TESTS`
in the Makefile. In-kernel test: add a function to
`kernel/test/selftest.c` and call it from `selftest_run_all()`. A test
whose prerequisites are absent should report **skipped**, not fail.

## Releases

```bash
make release
```

Runs the full check, builds the ISO, generates documentation, assembles
`dist/havenos-<version>/`, produces `dist/havenos-<version>.zip`, and
validates the archive's structure and contents. See
[RELEASE_NOTES.md](../RELEASE_NOTES.md).
