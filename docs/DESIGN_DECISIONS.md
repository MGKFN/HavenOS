# HavenOS design decisions

Requirement 7 of this project was "document all major design decisions".
This is that document.

Each entry states the decision, the alternatives that were rejected, what
the decision costs, and — where relevant — what HavenDOS did and why
HavenOS does something else. Entries are not in order of importance; they
are in the order the decisions had to be made.

Where a decision was *changed* during development because testing proved
it wrong, that is recorded too. Those are the most useful entries.

---

## boot

### Multiboot2 and GRUB, not a bespoke UEFI loader

**Decision.** HavenOS is loaded by GRUB 2 through the Multiboot2
protocol, from a single ISO carrying both a BIOS El Torito image and an
EFI System Partition.

**Rejected: writing a UEFI loader.** A native UEFI application would let
HavenOS own the whole boot path and would make Secure Boot signing
possible. It would also require reimplementing firmware protocol
enumeration, graphics mode selection and disk access before the kernel
could print a character — and HavenDOS's own documentation records UEFI
disk installation as still unfinished after reaching the point of booting
on real UEFI hardware.

**Rejected: Limine.** Technically the nicest option: it hands over in
64-bit mode with a direct map already built, which would delete most of
`boot.S`. Rejected because it is a third-party dependency that must be
present to build, and because the 32-bit-to-long-mode climb is worth
owning: it is the part of the boot that documents what the hardware
actually requires.

**What this keeps from HavenDOS.** Its single biggest milestone was
booting on real UEFI hardware by having GRUB own both firmware paths.
That arrangement works and is kept. What changed is the half that was
holding it back: the kernel is 64-bit.

**Cost.** GRUB must be installed to build an ISO, and Secure Boot is not
addressed.

### A 32-bit trampoline rather than a 64-bit entry point

GRUB hands control over in 32-bit protected mode with paging off, so the
climb to long mode is unavoidable with this protocol. The Multiboot2
*entry address tag* is what makes a higher-half 64-bit ELF loadable at
all: without it GRUB jumps to `e_entry`, which is an address it cannot
reach.

### Verifying the image at link time

`tools/verify_multiboot2.py` and `tools/verify_image.py` run on every
link. A malformed Multiboot2 header does not fail the build — it fails
the *boot*, with GRUB printing one line and no kernel output at all. Both
scripts exist because that is an expensive way to find a checksum
mistake.

---

## memory

### A buddy allocator, not a bitmap

**Decision.** Physical memory is managed by a buddy allocator over a
`struct page` array.

**Rejected: a bitmap with first-fit, which is what HavenDOS used.** Its
own comment conceded the cost: *"A first-fit scan is plenty: these are
allocated a handful of times per boot, never in a loop."* That is true of
a system with no processes. Once page faults, thread stacks, page tables
and slab growth all allocate, a linear scan over a million bits per
allocation is the wrong shape. A buddy allocator is O(log n), coalesces
automatically, and makes contiguous multi-page allocation the same
operation as single-page allocation rather than a separate scan.

**Cost.** `struct page` is 24 bytes per 4 KiB page — about 0.6% of RAM.
On a 512 MiB machine that is 3 MiB of metadata.

### Zones, because a device cannot address everything

A 32-bit PCI device given a page above 4 GiB DMAs to a truncated address.
The failure is data corruption, not an allocation error, so the
constraint belongs in the allocator rather than in each driver. The
`DMA32` zone is what `virtio_blk.c` asks for when it allocates its
descriptor ring.

### A direct map of all physical memory

**Decision.** All of RAM is mapped at `0xFFFF800000000000`, with 1 GiB
pages where the CPU supports them.

**Rejected: temporary mappings.** Every allocator and every DMA-capable
driver needs to touch a page it knows only by physical address. Without a
direct map each such access needs a mapping, a TLB shootdown and an
unmap. With one, `phys_to_virt()` is an addition that cannot fail.

**Cost.** Page tables for the map, which large pages make negligible, and
the fact that all of physical memory is addressable from ring 0 — which
is true of any kernel with a direct map.

### The kernel image at 4 KiB granularity, deliberately

2 MiB pages would use fewer page-table pages and fewer TLB entries. They
are not used for the kernel image because per-section W^X needs the 4 KiB
boundaries the linker script produces: a 2 MiB mapping forces the whole
region to the weakest permission of its contents, which would make
`.text` writable and `.data` executable. The direct map, where no such
boundaries exist, does use large pages.

### The reserve boundary is computed, never stated

HavenDOS's PMM reserved a hard-coded number of blocks below the kernel,
and its own source records that constant being wrong twice — 1 MiB, then
4 MiB, against a 6.31 MiB image — each time putting live kernel BSS in
the free pool. The v0.7.18 fix was to derive it from the linker's
`_kernel_end`.

HavenOS states no reserve size at all. Every reserved page comes from the
firmware memory map or from `__kernel_phys_start`/`__kernel_phys_end`,
`verify_image.py` checks those symbols exist and are ordered, and
`pmm_check()` asserts no free block overlaps a reserved page.

### Size-class slabs, not first-fit

**Rejected: a first-fit free list, which is what HavenDOS's heap was**,
over a 256 KiB *static array*. Every `kmalloc()` in the system competed
for that quarter megabyte and it could never grow, which is why its large
buffers had to be moved onto contiguous PMM runs.

Size classes mean a small allocation never walks past large free blocks,
and fragmentation between classes cannot occur. The cost is up to 50%
waste in the worst case within a class.

**Kept from HavenDOS:** header magic checked before any field is trusted,
double-free detection via a distinct free magic, payload poisoned with
`0xDD` on free, and a public integrity scanner. Those were the good ideas
in its v0.6.8 heap rewrite. Its actual magic values are kept too, so a
HavenOS dump is readable by someone who has debugged HavenDOS.

### A DMA layer, because paging made this harder

This is the one place where HavenOS's virtual memory is a *disadvantage*
relative to HavenDOS. With no paging, every address was physical and
every buffer was DMA-safe by accident. With paging, a `kmalloc`'d buffer
is neither physically contiguous nor at a fixed offset from its virtual
address, so it cannot be handed to a device at all.

The VirtIO driver's first version called `arch_virt_to_phys()` on a
`kmalloc`'d sector buffer. The assertion in `arch_virt_to_phys()` caught
it and named the cause exactly:

```
virt_to_phys: 0xffffc000000167c0 is not in a fixed-offset kernel window;
use arch_pt_translate() for mapped memory
```

That assertion is the reason this is a short story rather than a long
one. `dma_alloc_coherent()` plus a per-device bounce buffer is the fix.

---

## scheduling

### Fixed-priority round-robin, not fair-share

**Decision.** Eight priority bands, round-robin within a band, 4-tick
(40 ms) slices, per-CPU run queues.

**Rejected: a CFS-style fair-share scheduler** with a virtual-time
red-black tree. It is better for a general-purpose workload and HavenOS
will want one. It was not built first because the hard part of a
scheduler is not the queue discipline — it is preemption, context
switching, blocking, wake-up and the stack discipline. A priority
round-robin is small enough to read in one sitting while debugging
something else, and the queue discipline can be replaced later without
touching any of that.

**Cost.** No fairness guarantee between threads of equal priority beyond
round-robin, and no automatic priority adjustment for interactive
threads.

### Per-CPU run queues, not one global queue

A global queue needs a lock on every scheduling decision, which is the
single hottest path in the kernel. Per-CPU queues make the common case
lock-free. The cost is that load balancing must be explicit, and HavenOS
does not yet do it: a thread runs on the CPU that created it.

### Correction: a context switch *may* happen in interrupt context

The first version of `sched.c` asserted that it may not, reasoning that
the interrupt frame would be stranded. The in-kernel self-test caught it
within one run: four spinning threads produced one that ran forever and
three that never started, because a thread that makes no system calls and
never idles reaches no other switch point.

Switching from the timer handler is in fact safe, because the handler
runs on the interrupted thread's own kernel stack — the frame simply
waits there until that thread is scheduled again, and its `iretq` pops
it. What is *not* safe is switching from a nested handler (the outer
handler's frames would be stranded mid-unwind) or from an IST stack
(which is per-CPU, not per-thread). Preemption now happens at the end of
`irq_dispatch()` once the nesting count is zero.

This entry is here because the corrected reasoning is more useful than
the conclusion.

### A ticket spinlock, not test-and-set

A test-and-set lock has no fairness guarantee: a CPU can be starved
indefinitely. A ticket lock serves waiters in arrival order, which bounds
the wait and makes contention present as uniform slowness rather than as
one thread mysteriously never progressing. It also carries a deadlock
timeout that panics with both CPU numbers and the lock's name, because a
kernel that spins forever has no diagnostic at all.

---

## the user boundary

### SYSCALL/SYSRET, not `int 0x80`

A software interrupt goes through the IDT: a descriptor lookup, a stack
switch from the TSS, and a full interrupt frame — on the order of a
hundred cycles before any kernel code runs. `SYSCALL` loads CS, SS and
RIP from MSRs with no memory access.

**Cost.** `SYSCALL` does almost nothing for you. It does not switch
stacks, does not disable interrupts, and leaves `GS` pointing at user
memory, so the entry path must do all of it by hand in the right order.
The first three instructions in `entry.S` are the most safety-critical in
the kernel and are commented as such.

### Syscall numbers are an ABI and do not move

A compiled program contains the number, not the name. Numbers are
assigned once, gaps left by removed calls are never reused, and new calls
are appended.

### `fork()` returns `-ENOSYS`

**Decision.** Not implemented, and saying so rather than faking it.

A `fork()` without copy-on-write page tables would eagerly copy the
parent's entire address space — which `fork`+`exec`, the only thing most
programs do with it, immediately discards. `proc_spawn()` covers that
case. Returning 0 or a fabricated pid would make a caller believe it had
a child.

### Weak `-ENOSYS` stubs for every syscall

Each handler has a weak default that returns `-ENOSYS`, overridden simply
by linking the real implementation. A build without the filesystem layer
still runs and reports honestly which calls are unavailable. HavenDOS
returned bare `-1` and `-8` from unimplemented paths, so a caller could
not tell "not implemented" from "failed".

---

## filesystems

### The VFS walks paths; filesystems never see a separator

This is the structural fix for the most instructive bug in the HavenDOS
sources. Its VFS passed whole path strings to each filesystem, so
`fat16_write("boot/grub.cfg", ...)` handed that string to `name_to_83()`
and produced the 8.3 name `BOOT/GRU` in the root directory. Its own
comment: *"It looked like a write that worked and a read that came back
empty."*

`vfs_resolve()` splits the path and calls `->lookup()` once per
component. A filesystem that never receives a separator cannot mishandle
one.

### One FAT implementation, not three

HavenDOS had `fat16.c` (1150 lines) and `fat12.c` (534 lines) — two
implementations of the same filesystem, which meant the path bug had to
be fixed twice. FAT12, FAT16 and FAT32 differ in exactly three things:
FAT entry width, where the root directory lives, and the end-of-chain
marker. HavenOS has one implementation with a variant field.

### The FAT variant comes from the cluster count, not the type string

The `fs_type` string in the boot parameter block is advisory and
frequently wrong — formatters write `FAT16   ` on FAT12 volumes. Trusting
it means reading the FAT with the wrong entry width and following a chain
of plausible garbage. The thresholds (4085 and 65525) are from
Microsoft's specification.

### FAT writes 8.3 names only, and *refuses* longer ones

Reading VFAT long names costs nothing and makes a disk written elsewhere
usable. Writing them means generating and maintaining long-name entries
and their checksums, and getting that subtly wrong corrupts a filesystem
other systems will read.

So the asymmetry is deliberate, and a name that does not fit is
**refused** with `-ENAMETOOLONG` rather than mangled. Mangling is what
produced `BOOT/GRU`: a file the user did not ask for and cannot find.

### `unlink` frees the directory entry before the clusters

If power fails between the two, clusters are leaked — which `chkdsk` can
reclaim. The other order would leave a directory entry pointing at freed
clusters, which `chkdsk` cannot distinguish from a valid file and may
reallocate under.

---

## storage

### ATA by programmed I/O, not DMA

PIO is slow: one 16-bit port read per two bytes. It needs no bus-master
setup, no physical region descriptor table and no coherent DMA buffer,
and it works identically on every ATA controller ever made. For a kernel
whose block layer exists so a faster driver can be added later, correct
and universal beats fast.

### Every wait in a storage driver is bounded

An unbounded `while (!(status & DRQ))` hangs the whole machine when a
drive does not respond — and a drive that does not respond is exactly
what happens on the hardware a new OS gets tested on. The floating-bus
case is checked explicitly: with no drive attached every register reads
`0xFF`, which has BSY, DRQ *and* ERR set, so naive polling sees a drive
that is simultaneously busy, ready and faulted.

### Correction: the VirtIO queue size is not negotiable

The driver's first version clamped the device's reported queue size to
64. In the legacy virtio-pci interface that register is **read only**:
the device states its size and the driver must lay out exactly that ring.
The clamped layout put the available and used rings where the device did
not look, and every request timed out after five seconds with no hint of
the cause. The constant is now named `QUEUE_SIZE_MAX` and an unsupported
size is refused rather than clamped.

### Partitions are their own block devices

A filesystem mounts `/dev/hdap1`, not `/dev/hda` with an offset. The
partition device bounds-checks against the *partition*, so a filesystem
with a corrupt superblock cannot read its neighbour's data.

---

## devices

### A link-time driver table, not a call list

HavenDOS called every driver's init by name from `kernel_main()`, and its
own comments catalogue the result: `net_init()` after the screen that
reported whether a NIC existed; `virtio_scan_drives()` lazily, so the
same screen misreported working disks. Neither was a driver bug — both
were a boot sequence nobody could see the shape of.

`HAVEN_DRIVER()` puts a driver in a section the linker collects. Adding a
driver is adding a file.

### An unclaimed device is reported, not hidden

"Is this device supported" becomes answerable: a device is either claimed
by a named driver or reported as having none. HavenDOS's hardware list
showed recognised controllers with no indication of whether a driver
existed, which its documentation has to disclaim: *"Recognizing a
controller in a hardware list does not mean it has a working storage
driver."*

### The PIC stays, in virtual-wire mode

**Decision.** The LAPIC drives the per-CPU scheduler tick and IPIs;
device interrupts still come from the 8259 pair, with LAPIC LINT0 in
ExtINT mode.

**Rejected: an I/O APIC**, for now. A half-configured I/O APIC is worse
than a working PIC, and the LINT0 step is the one that is easy to miss —
omitting it makes every legacy device interrupt vanish the instant the
LAPIC is enabled, so the keyboard simply goes dead.

**Cost.** Device interrupts are delivered to the boot CPU only. The LAPIC
timer is per-CPU, so every CPU is still preempted.

---

## portability

### A 62-function HAL, checked by a script

Every operation the generic kernel needs from an architecture is declared
in `include/haven/arch.h` and nowhere else. The rule — nothing outside
`arch/` and `drivers/` may contain inline assembly, an architecture
conditional, an architecture-private header or a port-I/O call — is
enforced by `scripts/check-portability.sh` on every `make check`.

It has already caught one violation committed by mistake:
`kernel/main.c` was calling `acpi_init()` directly, which made the
portable boot sequence depend on an x86 firmware table. Finding the CPU
list is now the architecture's business.

### The ARM64 skeleton exists as evidence, not as a stub

`arch/arm64/` implements the operations whose AArch64 form is short and
unambiguous — barriers, DAIF interrupt masking, the generic timer, the
direct map, TLB maintenance — and **panics** for everything that needs a
table walker, a vector table or PSCI.

Panicking rather than returning a plausible value is the decision worth
recording. A stub `arch_pt_translate()` returning 0 would produce a
kernel that boots and then corrupts memory, with the bug appearing far
from its cause. Each panic names what is missing.

The skeleton's value is that the generic kernel compiles against it
unchanged. See [PORTING_ARM64.md](PORTING_ARM64.md).

---

## diagnostics

### The serial console comes up first

HavenDOS logged through `vga_printf()`, which formatted directly into
video memory. So nothing could be logged before the display was up,
nothing could be logged on a machine whose display did not work — which
its own UEFI bring-up notes describe as its hardest problem, with a boot
demonstrated by photographing a laptop screen — and the log existed only
on screen, where it scrolled away.

HavenOS formats once into a 64 KiB ring and fans out to registered sinks.
The 16550 UART needs no memory, no interrupts and no firmware
cooperation, so it registers before anything else. A sink that registers
later replays the whole ring, which is why the framebuffer console can
come up after memory management and still show the entire boot.

### Assertions are always compiled in

A kernel that keeps running after an invariant breaks corrupts data
silently, which is strictly worse than stopping. The branch costs less
than the class of bug `NDEBUG` would hide. The `arch_virt_to_phys()`
assertion is the clearest payoff: it turned a DMA bug into a one-line
diagnosis.

### Errors are negative errno, uniformly

HavenDOS carried two overlapping vocabularies — `HD_ERR_*` and bare magic
numbers (`-1`, `-2`, `-8`) returned from fs and installer code — so a
caller could not tell "not found" from "permission denied" without
reading the callee. Every HavenOS function returns 0 or a positive count
on success and `-HAVEN_E*` on failure, and `haven_strerror()` names them
so logs say `ENOENT` rather than `-2`.

### Both kinds of test, for different reasons

`tests/host/` compiles the real kernel sources natively and can attack an
allocator with millions of operations. That practice is inherited from
HavenDOS's own `tests/test_pmm.c`, whose reasoning still holds: *"a bug
here hands out memory that belongs to the kernel, so it is worth testing
directly rather than inferring from a successful boot."*

It cannot test whether a context switch returns to the right stack or
whether the timer actually preempts a spinning thread. Those need a real
CPU, so `kernel/test/selftest.c` runs on the machine, from a GRUB entry,
so a regression is caught by booting rather than by remembering to type
something.

Two build-flag problems were found in the host suite itself: without
`-fno-builtin` and `_FORTIFY_SOURCE=0`, GCC redirected `snprintf()` and
`strncpy()` to its own builtins and glibc's fortified versions, so the
suite reported passes while the kernel's own `lib/printf.c` and
`lib/string.c` never ran. `test_printf` now checks `%b`, a HavenOS
extension glibc does not implement, so a wrong link fails loudly.

---

## things deliberately not done

| Not done | Why |
|---|---|
| networking | HavenDOS had a full stack; porting it is Phase 3 and large. Claiming partial support would be worse than none. |
| I/O APIC | see above |
| `fork()` | needs copy-on-write to be worth having |
| long-name FAT writes | corrupts volumes other systems read, if subtly wrong |
| cryptographic RNG | needs an entropy pool and a real mixer; the current one says it is not one |
| ACPI beyond the MADT | an AML interpreter is a large subsystem HavenOS needs nothing from yet |
| load balancing | per-CPU queues make it possible; nothing measures imbalance yet |
| graphics beyond a console | Phase 4 |
