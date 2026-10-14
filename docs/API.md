# HavenOS kernel API reference

Generated from the headers in `include/haven` by
`tools/gen_api_docs.py`; run `make docs` to regenerate it.
Edit the header comments, not this file.

HavenOS has no stable user-space ABI yet, so every interface here
is kernel-internal and may change. The one interface meant to be
stable is the hardware abstraction layer in `haven/arch.h`: a port
to a new architecture implements exactly that set of functions and
nothing else, which is checked mechanically by
`scripts/check-portability.sh`. See
[PORTING_ARM64.md](PORTING_ARM64.md).

| | Count |
|---|---|
| Headers | 53 |
| Functions | 470 |
| Types | 88 |
| Constants and macros | 341 |
| Exported variables | 6 |
| Declarations with documentation | 224 |

## Contents

- **Foundations** — [`compiler.h`](#havencompilerh), [`types.h`](#haventypesh), [`errno.h`](#havenerrnoh), [`list.h`](#havenlisth), [`string.h`](#havenstringh), [`printf.h`](#havenprintfh), [`panic.h`](#havenpanich), [`console.h`](#havenconsoleh), [`version.h`](#havenversionh), [`random.h`](#havenrandomh)
- **Boot and platform** — [`boot.h`](#havenbooth), [`multiboot2.h`](#havenmultiboot2h), [`cmdline.h`](#havencmdlineh), [`arch.h`](#havenarchh), [`vmlayout.h`](#havenvmlayouth), [`fault.h`](#havenfaulth)
- **Memory** — [`mm.h`](#havenmmh), [`dma.h`](#havendmah)
- **Execution** — [`sched.h`](#havenschedh), [`smp.h`](#havensmph), [`spinlock.h`](#havenspinlockh), [`mutex.h`](#havenmutexh), [`irq.h`](#havenirqh), [`time.h`](#haventimeh)
- **Processes and system calls** — [`proc.h`](#havenproch), [`elf.h`](#havenelfh), [`syscall.h`](#havensyscallh)
- **Filesystems** — [`vfs.h`](#havenvfsh), [`ramfs.h`](#havenramfsh), [`devfs.h`](#havendevfsh), [`fat.h`](#havenfath)
- **Devices and drivers** — [`device.h`](#havendeviceh), [`pci.h`](#havenpcih), [`block.h`](#havenblockh), [`ata.h`](#havenatah), [`virtio.h`](#havenvirtioh), [`input.h`](#haveninputh), [`fbcon.h`](#havenfbconh)
- **Diagnostics** — [`selftest.h`](#havenselftesth), [`shell.h`](#havenshellh)
- **Architecture-private headers** — [`haven/arch/arm64/memlayout.h`](#havenarcharm64memlayouth), [`haven/arch/x86_64/acpi.h`](#havenarchx86_64acpih), [`haven/arch/x86_64/boot.h`](#havenarchx86_64booth), [`haven/arch/x86_64/cpu.h`](#havenarchx86_64cpuh), [`haven/arch/x86_64/gdt.h`](#havenarchx86_64gdth), [`haven/arch/x86_64/idt.h`](#havenarchx86_64idth), [`haven/arch/x86_64/io.h`](#havenarchx86_64ioh), [`haven/arch/x86_64/lapic.h`](#havenarchx86_64lapich), [`haven/arch/x86_64/memlayout.h`](#havenarchx86_64memlayouth), [`haven/arch/x86_64/paging.h`](#havenarchx86_64pagingh), [`haven/arch/x86_64/percpu.h`](#havenarchx86_64percpuh), [`haven/arch/x86_64/percpu_asm.h`](#havenarchx86_64percpu_asmh), [`haven/arch/x86_64/regs.h`](#havenarchx86_64regsh)

## Foundations

### `haven/compiler.h`

compiler attribute and barrier abstractions.

*24 declarations, 2 carrying documentation.*

**`HAVEN_PACKED`** = `__attribute__((packed))`

**`HAVEN_ALIGNED(n)`**

**`HAVEN_NORETURN`** = `__attribute__((noreturn))`

**`HAVEN_UNUSED`** = `__attribute__((unused))`

**`HAVEN_USED`** = `__attribute__((used))`

**`HAVEN_PRINTF(f, a)`**

**`HAVEN_SECTION(s)`**

**`HAVEN_WEAK`** = `__attribute__((weak))`

**`HAVEN_ALWAYS_INLINE`** = `inline __attribute__((always_inline))`

**`HAVEN_NOINLINE`** = `__attribute__((noinline))`

**`HAVEN_MAY_ALIAS`** = `__attribute__((may_alias))`

**`likely(x)`**

**`unlikely(x)`**

**`barrier()`**

Compiler barrier: forbid reordering of memory accesses across this point. Not a CPU fence — see arch_mb()/arch_rmb()/arch_wmb() for those.

**`ARRAY_SIZE(a)`**

**`HAVEN_UNREACHABLE()`**

#### Static assertion usable at file scope in C11 and later.

**`HAVEN_STATIC_ASSERT(cond, msg)`**

**`container_of(ptr, type, member)`**

container_of: recover the enclosing struct from a member pointer.  The intrusive list in list.h is built on this, which is why HavenOS needs no per-type list code the way HavenDOS's fixed-size arrays did.

**`MIN(a, b)`**

**`MAX(a, b)`**

#### Rounding helpers.  align must be a power of two.

**`ALIGN_UP(x, align)`**

**`ALIGN_DOWN(x, align)`**

**`IS_ALIGNED(x, align)`**

**`DIV_ROUND_UP(n, d)`**

### `haven/types.h`

fundamental kernel types.

*13 declarations, 6 carrying documentation.*

**`typedef uint64_t paddr_t`**

Physical and virtual addresses are distinct types on purpose.  The single most common class of bug in HavenDOS-era code was passing one where the other was meant; with paging enabled that is no longer merely sloppy, it is a fault.  These stay integer typedefs (not pointers) so arithmetic on them is legal and so a physical address is never accidentally dereferenced without going through phys_to_virt().

**`typedef uint64_t vaddr_t`**

kernel/user virtual address

**`typedef int64_t ssize_t_k`**

signed size, named to avoid host ssize_t clash

**`typedef uint64_t haven_off_t`**

**`typedef int32_t pid_t_k`**

Process, thread and user identifiers.

**`typedef int32_t tid_t`**

**`typedef uint32_t uid_t_k`**

**`typedef uint32_t gid_t_k`**

**`typedef uint64_t nstime_t`**

Monotonic time, nanoseconds since boot.

**`HAVEN_PAGE_SHIFT`** = `12`

**`HAVEN_PAGE_SIZE`** = `(1UL << HAVEN_PAGE_SHIFT)`

4 KiB base page

**`HAVEN_PAGE_MASK`** = `(HAVEN_PAGE_SIZE - 1)`

**`HAVEN_STATIC_ASSERT(sizeof(paddr_t) == 8, "HavenOS targets 64-bit hosts")`**

### `haven/errno.h`

kernel error codes.

*36 declarations, 35 carrying documentation.*

**`HAVEN_EOK`** = `0`

success

**`HAVEN_EPERM`** = `1`

operation not permitted

**`HAVEN_ENOENT`** = `2`

no such file or directory

**`HAVEN_EIO`** = `5`

I/O error

**`HAVEN_ENXIO`** = `6`

no such device or address

**`HAVEN_E2BIG`** = `7`

argument list too long

**`HAVEN_ENOEXEC`** = `8`

exec format error

**`HAVEN_EBADF`** = `9`

bad file descriptor

**`HAVEN_ECHILD`** = `10`

no child processes

**`HAVEN_EAGAIN`** = `11`

try again

**`HAVEN_ENOMEM`** = `12`

out of memory

**`HAVEN_EACCES`** = `13`

permission denied

**`HAVEN_EFAULT`** = `14`

bad address

**`HAVEN_EBUSY`** = `16`

device or resource busy

**`HAVEN_EEXIST`** = `17`

file exists

**`HAVEN_EXDEV`** = `18`

cross-device link

**`HAVEN_ENODEV`** = `19`

no such device

**`HAVEN_ENOTDIR`** = `20`

not a directory

**`HAVEN_EISDIR`** = `21`

is a directory

**`HAVEN_EINVAL`** = `22`

invalid argument

**`HAVEN_ENFILE`** = `23`

file table overflow

**`HAVEN_EMFILE`** = `24`

too many open files

**`HAVEN_ENOSPC`** = `28`

no space left on device

**`HAVEN_ESPIPE`** = `29`

illegal seek

**`HAVEN_EROFS`** = `30`

read-only file system

**`HAVEN_EMLINK`** = `31`

too many links

**`HAVEN_EFBIG`** = `27`

file too large for the filesystem

**`HAVEN_ERANGE`** = `34`

result out of range

**`HAVEN_ENAMETOOLONG`** = `36`

file name too long

**`HAVEN_ENOSYS`** = `38`

function not implemented

**`HAVEN_ENOTEMPTY`** = `39`

directory not empty

**`HAVEN_ELOOP`** = `40`

too many symbolic links

**`HAVEN_EOVERFLOW`** = `75`

value too large for data type

**`HAVEN_ETIMEDOUT`** = `110`

connection/operation timed out

**`HAVEN_ECORRUPT`** = `117`

structure needs cleaning / corrupt metadata

#### Human-readable name for an error code.  Accepts either sign.

**`const char *haven_strerror(int err)`**

### `haven/list.h`

intrusive doubly-linked circular list.

*19 declarations, 0 carrying documentation.*

**`struct list_head`**

```c
struct list_head {
	struct list_head *next;
	struct list_head *prev;
};
```

**`LIST_HEAD_INIT(name)`**

**`LIST_HEAD(name)`**

**`static inline void list_init(struct list_head *list)`**

**`static inline void __list_add(struct list_head *new_node, struct list_head *prev, struct list_head *next)`**

#### Insert immediately after head (LIFO / stack order).

**`static inline void list_add(struct list_head *new_node, struct list_head *head)`**

#### Insert immediately before head (FIFO / queue order).

**`static inline void list_add_tail(struct list_head *new_node, struct list_head *head)`**

**`static inline void list_del(struct list_head *entry)`**

#### Remove and re-initialise, so the entry is immediately reusable.

**`static inline void list_del_init(struct list_head *entry)`**

**`static inline bool list_empty(const struct list_head *head)`**

#### Move an entry that is already on some list to the tail of another.

**`static inline void list_move_tail(struct list_head *entry, struct list_head *head)`**

**`static inline size_t list_length(const struct list_head *head)`**

**`list_entry(ptr, type, member)`**

**`list_first_entry(head, type, member)`**

**`list_last_entry(head, type, member)`**

**`list_for_each(pos, head)`**

#### Safe against list_del() of the current element.

**`list_for_each_safe(pos, n, head)`**

**`list_for_each_entry(pos, head, member)`**

**`list_for_each_entry_safe(pos, n, head, member)`**

### `haven/string.h`

freestanding string and memory routines.

*23 declarations, 3 carrying documentation.*

**`void *memset(void *dst, int c, size_t n)`**

**`void *memcpy(void *dst, const void *src, size_t n)`**

**`void *memmove(void *dst, const void *src, size_t n)`**

**`int memcmp(const void *a, const void *b, size_t n)`**

**`void *memchr(const void *s, int c, size_t n)`**

**`size_t strlen(const char *s)`**

**`size_t strnlen(const char *s, size_t max)`**

**`int strcmp(const char *a, const char *b)`**

**`int strncmp(const char *a, const char *b, size_t n)`**

**`int strcasecmp(const char *a, const char *b)`**

**`char *strncpy(char *dst, const char *src, size_t n)`**

**`char *strchr(const char *s, int c)`**

**`char *strrchr(const char *s, int c)`**

**`char *strstr(const char *hay, const char *needle)`**

**`char *strtok_r(char *str, const char *delim, char **saveptr)`**

**`size_t strlcpy(char *dst, const char *src, size_t size)`**

Bounded copy/concat.  Always NUL-terminate dst (when size > 0) and return the total length the operation wanted, so `if (strlcpy(d, s, sizeof d) >= sizeof d)` detects truncation.

**`size_t strlcat(char *dst, const char *src, size_t size)`**

**`void itoa64(int64_t val, char *buf, size_t bufsz, int base)`**

Numeric conversion.  base 2..16; buf must hold at least 65 bytes for base 2 of a 64-bit value plus sign and terminator.

**`void utoa64(uint64_t val, char *buf, size_t bufsz, int base)`**

**`long strtol_simple(const char *s, const char **end, int base)`**

**`int atoi(const char *s)`**

**`void str_upper(char *s, size_t n)`**

ASCII case folding, in place, bounded.

**`void str_lower(char *s, size_t n)`**

### `haven/printf.h`

formatted output.

*2 declarations, 0 carrying documentation.*

**`int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)`**

**`int snprintf(char *buf, size_t size, const char *fmt, ...)`**

### `haven/panic.h`

unrecoverable error handling and assertions.

*8 declarations, 1 carrying documentation.*

**`void panic(const char *fmt, ...)`**

Does not return.

#### Print `count` frames of the current call chain through klog().

**`void backtrace(unsigned count)`**

**`void panic_at(const char *file, int line, const char *func, const char *fmt, ...)`**

Does not return.

**`PANIC(...)`**

**`HAVEN_ASSERT(cond)`**

Assertions are always compiled in.  A kernel that keeps running after an invariant breaks corrupts data silently, which is strictly worse than stopping: the cost of the branch is not worth the class of bug that NDEBUG would hide.

**`HAVEN_ASSERT_MSG(cond, ...)`**

#### For switch defaults and branches that are structurally impossible.

**`HAVEN_BUG()`**

**`bool panic_in_progress(void)`**

### `haven/console.h`

kernel log and console sinks.

*20 declarations, 4 carrying documentation.*

**`enum log_level`**

```c
enum log_level {
	LOG_PANIC = 0,   /* the system is going down now */
	LOG_ERR,         /* a real failure, system continues */
	LOG_WARN,        /* something unexpected but handled */
	LOG_INFO,        /* normal progress reporting */
	LOG_DEBUG,       /* development detail, off by default */
	LOG_LEVEL_COUNT
};
```

**`struct console`**

A console sink.  write() receives already-formatted bytes and must not allocate, take the console lock, or call klog() — it runs inside the log path, including from panic context with interrupts off.

```c
struct console {
	const char       *name;
	void            (*write)(const char *buf, size_t len);
	void            (*flush)(void);     /* optional */
	struct console   *next;
	enum log_level    min_level;
	bool              enabled;
};
```

**`void console_register(struct console *con)`**

**`void console_unregister(struct console *con)`**

**`void console_flush_all(void)`**

**`void console_set_level(enum log_level level)`**

Global threshold.  Messages above this level are still recorded in the ring buffer but are not written to sinks, so `dmesg` can show detail the live console suppressed.

**`enum log_level console_get_level(void)`**

**`void klog(enum log_level level, const char *fmt, ...)`**

**`void kvlog(enum log_level level, const char *fmt, va_list ap)`**

**`void kputs_raw(const char *s)`**

Raw, unformatted, unprefixed output — for the panic path and for anything that must appear with no decoration.

**`void kwrite_raw(const char *buf, size_t len)`**

**`kerr(...)`**

**`kwarn(...)`**

**`kinfo(...)`**

**`kdebug(...)`**

#### Ring-buffer access, for the `dmesg` command and for crash dumps.

**`size_t console_ring_read(char *out, size_t outsz)`**

**`size_t console_ring_size(void)`**

#### Early serial bring-up, called before any allocator exists.

**`void serial_early_init(void)`**

**`bool serial_available(void)`**

**`int serial_getc_nonblock(void)`**

Non-blocking read of one byte from the serial console, or -1 if none is waiting.  This is the kernel's input of last resort: it works on a machine with no keyboard driver and no working display, which is how a headless or half-initialised board is debugged.

### `haven/version.h`

the single source of truth for the HavenOS version.

*9 declarations, 0 carrying documentation.*

**`HAVENOS_VERSION_MAJOR`** = `0`

**`HAVENOS_VERSION_MINOR`** = `1`

**`HAVENOS_VERSION_PATCH`** = `0`

**`HAVENOS_VERSION`** = `"0.1.0"`

**`HAVENOS_CODENAME`** = `"Foundation"`

**`HAVENOS_COPYRIGHT`** = `"Copyright (c) 2026 KUNIX / Tech Haven Studios - MIT licensed"`

**`HAVENOS_BUILD_DATE`** = `"unknown"`

**`HAVENOS_BUILD_REV`** = `"unknown"`

**`HAVENOS_ARCH`** = `"x86_64"`

### `haven/random.h`

kernel entropy.

*2 declarations, 0 carrying documentation.*

**`uint64_t haven_random64(void)`**

**`void mem_devices_init(void)`**

## Boot and platform

### `haven/boot.h`

architecture-neutral boot information.

*13 declarations, 0 carrying documentation.*

**`BOOT_MAX_MEMMAP`** = `64`

**`BOOT_MAX_MODULES`** = `8`

**`BOOT_CMDLINE_MAX`** = `256`

**`enum boot_mem_type`**

```c
enum boot_mem_type {
	BOOT_MEM_USABLE = 1,     /* free RAM the PMM may hand out */
	BOOT_MEM_RESERVED,       /* firmware-owned, never touch */
	BOOT_MEM_ACPI_RECLAIM,   /* ACPI tables; usable after ACPI parsing */
	BOOT_MEM_ACPI_NVS,       /* must be preserved across sleep */
	BOOT_MEM_BAD,            /* firmware reported defective */
	BOOT_MEM_BOOTLOADER,     /* loader structures; reclaimable later */
	BOOT_MEM_KERNEL,         /* the kernel image itself */
	BOOT_MEM_FRAMEBUFFER,    /* MMIO aperture for the display */
};
```

**`struct boot_memmap_entry`**

```c
struct boot_memmap_entry {
	paddr_t  base;
	uint64_t length;
	uint32_t type;           /* enum boot_mem_type */
	uint32_t _pad;
};
```

**`enum boot_fb_format`**

```c
enum boot_fb_format {
	BOOT_FB_NONE = 0,
	BOOT_FB_RGB,             /* linear packed pixels */
	BOOT_FB_EGA_TEXT,        /* 2 bytes per cell: char + attribute */
};
```

**`struct boot_framebuffer`**

```c
struct boot_framebuffer {
	paddr_t  address;
	uint32_t width;          /* pixels, or columns for EGA text */
	uint32_t height;         /* pixels, or rows for EGA text */
	uint32_t pitch;          /* bytes per scanline */
	uint8_t  bpp;
	uint8_t  format;         /* enum boot_fb_format */
	uint8_t  red_shift,   red_bits;
	uint8_t  green_shift, green_bits;
	uint8_t  blue_shift,  blue_bits;
	uint8_t  _pad[2];
};
```

**`struct boot_module`**

```c
struct boot_module {
	paddr_t  start;
	paddr_t  end;
	char     name[64];
};
```

**`enum boot_firmware`**

```c
enum boot_firmware {
	BOOT_FW_UNKNOWN = 0,
	BOOT_FW_BIOS,
	BOOT_FW_UEFI,
};
```

**`struct boot_info`**

```c
struct boot_info {
	/* Identification of how we got here. */
	uint32_t                 protocol_magic;
	uint8_t                  firmware;       /* enum boot_firmware */
	uint8_t                  _pad[3];
	char                     loader_name[64];
	char                     cmdline[BOOT_CMDLINE_MAX];

	/* Physical memory. */
	struct boot_memmap_entry memmap[BOOT_MAX_MEMMAP];
	uint32_t                 memmap_count;
	uint64_t                 ram_total;      /* sum of USABLE regions */
	uint64_t                 ram_highest;    /* top of the highest region */

	/* Where the kernel image physically lives, so the PMM can reserve it
	   from the linker's own symbols rather than from a hard-coded size.
	   HavenDOS's pmm.c carried two successive fixed reserve constants
	   (1 MiB, then 4 MiB) that both ended up smaller than the image. */
	paddr_t                  kernel_phys_start;
	paddr_t                  kernel_phys_end;

	/* Display handed over by firmware, if any. */
	struct boot_framebuffer  framebuffer;

	/* Loader-supplied modules (initrd and friends). */
	struct boot_module       modules[BOOT_MAX_MODULES];
	uint32_t                 module_count;

	/* Firmware tables, 0 when not provided. */
	paddr_t                  rsdp;           /* ACPI root pointer */
	paddr_t                  smbios;
	paddr_t                  dtb;            /* ARM64 device tree */
};
```

#### Filled by arch/<arch>/boot before haven_kmain() runs.

**`extern struct boot_info haven_boot_info`**

#### Architecture-neutral kernel entry.  Never returns.

**`void haven_kmain(struct boot_info *info)`**

Does not return.

**`const char *boot_mem_type_name(uint32_t type)`**

### `haven/multiboot2.h`

the Multiboot2 handover parser's only public entry point.

*1 declarations, 1 carrying documentation.*

**`int multiboot2_parse(uint32_t magic, uint32_t info_pa, struct boot_info *bi)`**

Translate the Multiboot2 information block at physical address `info_pa` into `bi`.  Returns 0 on success, -1 when the handover is unusable (wrong magic, malformed tags, or no usable RAM reported).

### `haven/cmdline.h`

the kernel command line.

*4 declarations, 3 carrying documentation.*

#### Is `flag` present as a whole word?

**`bool cmdline_has(const struct boot_info *bi, const char *flag)`**

**`bool cmdline_value(const struct boot_info *bi, const char *key, char *out, size_t outsz)`**

Copy the value of `key=value` into `out`.  `key` is given without the '=' sign.  Returns false and leaves `out` an empty string when the key is absent, so a caller can treat "missing" and "empty" alike.

**`void cmdline_apply_early(const struct boot_info *bi)`**

Options that must take effect before the rest of the kernel starts logging: `loglevel=` and `console=`.  Safe to call once the console exists and boot_info is filled in; harmless to call twice.

**`bool cmdline_console_serial_only(void)`**

True when `console=serial` was given, meaning a framebuffer found by the firmware should not be taken over as a console.  Asked by the architecture's display bring-up.

### `haven/arch.h`

the hardware abstraction layer HavenOS is portable through.

*73 declarations, 29 carrying documentation.*

#### CPU control

**`void arch_early_init(void)`**

before any allocation

**`void arch_init(void)`**

after MM is up

**`void arch_cpu_init_tables(void)`**

Install whatever descriptor/vector tables the architecture needs, in whatever order it requires.  Called once memory management is up and before irq_init().

**`void arch_halt(void)`**

Does not return.

**`void arch_reboot(void)`**

Does not return.

**`void arch_idle(void)`**

wait for the next interrupt

**`void arch_pause(void)`**

spin-loop yield hint

**`uint32_t arch_cpu_id(void)`**

logical ID of the running CPU

**`const char *arch_cpu_vendor(void)`**

**`const char *arch_cpu_model(void)`**

**`typedef uint64_t irqflags_t`**

──────────────────────── Interrupt state ─────────────────────── arch_irq_save()/arch_irq_restore() nest correctly: save returns the previous state as an opaque word and restore puts exactly that back. HavenDOS used bare `cli`/`sti` pairs, so any nesting re-enabled interrupts at the end of the inner region — the exact failure mode that makes a spinlock not actually a lock.

**`irqflags_t arch_irq_save(void)`**

**`void arch_irq_restore(irqflags_t flags)`**

**`void arch_irq_enable(void)`**

**`void arch_irq_disable(void)`**

**`bool arch_irq_enabled(void)`**

#### Memory barriers

**`void arch_mb(void)`**

full barrier

**`void arch_rmb(void)`**

load barrier

**`void arch_wmb(void)`**

store barrier

#### Paging / address space

**`VM_PROT_READ`** = `(1U << 0)`

Page-table permission and attribute bits, in architecture-neutral form. arch/<arch>/mm translates these to hardware bits; nothing in kernel/mm knows what a PTE looks like.

**`VM_PROT_WRITE`** = `(1U << 1)`

**`VM_PROT_EXEC`** = `(1U << 2)`

**`VM_PROT_USER`** = `(1U << 3)`

reachable from CPL 3 / EL0

**`VM_ATTR_NOCACHE`** = `(1U << 4)`

device / MMIO memory

**`VM_ATTR_WRITETHRU`** = `(1U << 5)`

**`VM_ATTR_GLOBAL`** = `(1U << 6)`

not flushed on address-space switch

#### Opaque per-address-space page-table root.

**`struct arch_pagetable`**

**`struct arch_pagetable *arch_pt_create(void)`**

**`void arch_pt_destroy(struct arch_pagetable *pt)`**

**`struct arch_pagetable *arch_pt_kernel(void)`**

**`void arch_pt_switch(struct arch_pagetable *pt)`**

**`int arch_pt_map(struct arch_pagetable *pt, vaddr_t va, paddr_t pa, size_t len, uint32_t prot)`**

Map/unmap a single page.  Returns 0 or -HAVEN_E*. arch_pt_map() allocates intermediate tables through pmm_alloc_page().

**`int arch_pt_unmap(struct arch_pagetable *pt, vaddr_t va, size_t len)`**

**`int arch_pt_protect(struct arch_pagetable *pt, vaddr_t va, size_t len, uint32_t prot)`**

**`paddr_t arch_pt_translate(struct arch_pagetable *pt, vaddr_t va)`**

**`void arch_tlb_flush_page(vaddr_t va)`**

**`void arch_tlb_flush_all(void)`**

**`void arch_pt_init_kernel(void)`**

Build the kernel address space: an empty root, then the direct map and the kernel image window, then activate it and discard the boot-time identity map.  Declared here (rather than only in the architecture's private header) because kernel/mm/vmm.c drives the sequence and must stay architecture-neutral.

**`int arch_pt_map_hhdm(paddr_t pa, size_t len)`**

**`int arch_pt_map_kernel_image(paddr_t phys_start, paddr_t phys_end)`**

**`void arch_pt_activate_kernel(void)`**

**`void arch_pt_drop_identity_map(void)`**

#### Page-table pages currently owned by the kernel, for `meminfo`.

**`uint64_t arch_pt_page_count(void)`**

**`vaddr_t arch_phys_to_virt(paddr_t pa)`**

The direct map of all physical RAM.  Both directions are pure arithmetic, which is what makes the PMM architecture-neutral.

**`paddr_t arch_virt_to_phys(vaddr_t va)`**

**`size_t arch_page_size_for(vaddr_t va, paddr_t pa, size_t len)`**

Largest page size this architecture supports for the given span, so the VMM can use 2 MiB / 1 GiB (or ARM64 block) mappings for the direct map.

#### Context switching

**`struct arch_context`**

opaque, defined per architecture

**`size_t arch_context_size(void)`**

**`void arch_context_init_kernel(struct arch_context *ctx, vaddr_t stack_top, void (*entry)(void *), void *arg)`**

Build a fresh kernel-thread context that will begin executing entry(arg) on the given stack.

#### Build a context that enters user mode at `entry` with stack `ustack`.

**`void arch_context_init_user(struct arch_context *ctx, vaddr_t kstack_top, vaddr_t entry, vaddr_t ustack)`**

**`void arch_context_switch(struct arch_context *from, struct arch_context *to)`**

Save `from`, load `to`.  Returns in the `from` context when it is next scheduled.  Implemented in assembly.

#### First entry into a context that has never run.

**`void arch_context_start(struct arch_context *to)`**

Does not return.

**`void arch_enter_user(vaddr_t entry, vaddr_t stack)`**

Leave ring 0 for the given user entry point and stack.  Never returns: the only way back into the kernel is a trap or a system call.

Does not return.

#### Per-CPU pointer the entry/exit paths use to find the current thread.

**`void arch_set_current_thread(void *thread)`**

**`void *arch_get_current_thread(void)`**

**`void arch_set_kernel_stack(vaddr_t stack_top)`**

**`ARCH_IRQ_MAX`** = `256`

**`void arch_irq_init(void)`**

**`void arch_irq_unmask(uint32_t irq)`**

**`void arch_irq_mask(uint32_t irq)`**

**`void arch_irq_eoi(uint32_t irq)`**

#### Timekeeping

**`void arch_timer_init(uint32_t hz)`**

periodic scheduler tick

**`nstime_t arch_time_ns(void)`**

The architecture's timer interrupt handler calls timer_tick() once per tick on the CPU that took it; see <haven/time.h> for its declaration.

**`void arch_delay_us(uint64_t us)`**

calibrated busy-wait

**`uint64_t arch_clock_hz(void)`**

Facts about the monotonic clock, in portable terms.  kernel/time needs to report them and must not know whether the clock is a TSC, a generic timer or anything else -- so it asks these rather than arch_tsc_hz(), which exists only inside arch/x86_64.

**`bool arch_clock_reliable(void)`**

false when it can drift with CPU frequency or power state

#### SMP

**`uint32_t arch_smp_cpu_count(void)`**

**`int arch_smp_start_cpus(void)`**

bring up application CPUs

**`void arch_smp_send_ipi(uint32_t cpu, uint32_t vector)`**

#### Syscall entry

**`void arch_syscall_init(void)`**

**`int arch_copy_from_user(void *dst, const void *user_src, size_t len)`**

───────────────── User/kernel memory boundary ──────────────── Copying across the privilege boundary must be fault-tolerant: a bad user pointer returns -HAVEN_EFAULT instead of panicking the kernel. HavenDOS had no user mode and so no equivalent; this is the primitive that makes "secure user and kernel separation" real rather than nominal.

**`int arch_copy_to_user(void *user_dst, const void *src, size_t len)`**

**`int arch_strncpy_from_user(char *dst, const char *user_src, size_t len)`**

### `haven/vmlayout.h`

the virtual address layout constants the generic VMM uses.

*0 declarations, 0 carrying documentation.*

### `haven/fault.h`

the architecture-neutral fault interface.

*6 declarations, 6 carrying documentation.*

**`VM_FAULT_WRITE`** = `(1u << 0)`

the access was a write

**`VM_FAULT_EXEC`** = `(1u << 1)`

the access was an instruction fetch

**`VM_FAULT_USER`** = `(1u << 2)`

the access came from user mode

**`VM_FAULT_PRESENT`** = `(1u << 3)`

the page was present: a permission violation, not a missing mapping

**`bool vm_handle_fault(vaddr_t addr, uint32_t flags)`**

Returns true when the fault was resolved and the faulting instruction should be retried.  The weak default returns false, so a kernel built without the VM fault handler still reports faults correctly instead of calling into an uninitialised subsystem.

**`bool proc_fault_kill(const char *reason, int signal)`**

Terminate the process responsible for an unrecoverable user-mode fault and schedule something else.  Returns true when a process was killed — false means there is no process layer or no current process, in which case the architecture handler must treat the fault as fatal, because a user-mode fault that nothing can account for means the kernel has lost track of what is running.

## Memory

### `haven/mm.h`

physical and virtual memory management.

*54 declarations, 19 carrying documentation.*

#### Physical memory

**`PMM_MAX_ORDER`** = `11`

Buddy orders: order n is 2^n contiguous pages.  MAX_ORDER 11 means the largest single allocation is 2^10 pages = 4 MiB, which covers every contiguous requirement HavenOS has (page tables, DMA rings, the framebuffer shadow) without the free lists growing unhelpfully long.

**`enum pmm_zone`**

Zones exist because not every device can address all of memory.  A 32-bit PCI device given a page above 4 GiB will DMA into the wrong place, and the failure appears as data corruption rather than as an allocation error — so the constraint belongs in the allocator.

```c
enum pmm_zone {
	PMM_ZONE_DMA = 0,    /* below 16 MiB: ISA DMA */
	PMM_ZONE_DMA32,      /* below 4 GiB:  32-bit PCI */
	PMM_ZONE_NORMAL,     /* everything else */
	PMM_ZONE_COUNT
};
```

**`PMM_ZONE_DMA_LIMIT`** = `(16ULL * 1024 * 1024)`

**`PMM_ZONE_DMA32_LIMIT`** = `(4ULL * 1024 * 1024 * 1024)`

**`PAGE_FLAG_FREE`** = `(1u << 0)`

Per-page metadata.  One of these exists for every page of RAM, which is what lets the allocator answer "is this page free, and if it is part of a larger block, how large" without a search — and what gives pages a reference count, so a page can be shared between address spaces.

**`PAGE_FLAG_RESERVED`** = `(1u << 1)`

firmware or kernel image

**`PAGE_FLAG_BUDDY`** = `(1u << 2)`

head of a free buddy block

**`PAGE_FLAG_SLAB`** = `(1u << 3)`

owned by a kmalloc cache

**`PAGE_FLAG_PGTABLE`** = `(1u << 4)`

holds a page table

**`struct page`**

```c
struct page {
	struct list_head node;    /* on a buddy free list, or a slab list */
	uint32_t         flags;
	uint8_t          order;   /* valid when PAGE_FLAG_BUDDY */
	uint8_t          zone;
	uint16_t         refcount;
};
```

**`void pmm_init(const struct boot_info *bi)`**

**`paddr_t pmm_alloc_pages_zone(unsigned order, enum pmm_zone zone)`**

Allocate 2^order contiguous pages from `zone` or any lower-addressed zone.  Returns the physical address, or 0 on failure — 0 is never a valid return because the first page of RAM is always reserved.

**`paddr_t pmm_alloc_pages(unsigned order)`**

**`paddr_t pmm_alloc_page(void)`**

**`paddr_t pmm_alloc_contig(size_t nr_pages, enum pmm_zone zone)`**

Allocate `nr` pages, rounding up to the next power of two.  Prefer pmm_alloc_pages() when the count is already a power of two: this wrapper can waste up to half the allocation.

**`void pmm_free_pages(paddr_t pa, unsigned order)`**

**`void pmm_free_page(paddr_t pa)`**

**`void pmm_free_contig(paddr_t pa, size_t nr_pages)`**

**`paddr_t pmm_alloc_page_zeroed(void)`**

Allocate a page and zero it.  Page tables must be zeroed before use and forgetting to is a fault with no useful diagnostic, so the operation is offered as one call.

**`struct page *pmm_page_for(paddr_t pa)`**

**`paddr_t pmm_page_addr(const struct page *pg)`**

#### Reference counting, for pages mapped into more than one address space.

**`void pmm_page_get(paddr_t pa)`**

**`bool pmm_page_put(paddr_t pa)`**

true when the page was freed

**`struct pmm_stats`**

```c
struct pmm_stats {
	uint64_t total_pages;
	uint64_t free_pages;
	uint64_t reserved_pages;
	uint64_t allocated_pages;
	uint64_t zone_free[PMM_ZONE_COUNT];
	uint64_t zone_total[PMM_ZONE_COUNT];
	uint64_t alloc_calls;
	uint64_t free_calls;
	uint64_t alloc_failures;
};
```

**`void pmm_get_stats(struct pmm_stats *out)`**

**`void pmm_dump(void)`**

**`void pmm_set_alloc_limit(paddr_t limit)`**

Bound what the allocator may return, or 0 to lift the bound.

Needed for exactly one window during boot: pmm_init() runs while the only direct map is the fixed one the boot trampoline built, so until vmm_init() extends it, any page above that window is unreachable through arch_phys_to_virt() and must not be handed out.  vmm_init() lifts the bound as its last act.  Nothing else should call this.

**`paddr_t pmm_alloc_limit(void)`**

**`bool pmm_check(void)`**

Walk every buddy free list and verify the invariants: every block is marked free, aligned to its order, inside RAM, and not overlapping its neighbours.  Returns true when the allocator is consistent.  This is the physical-memory counterpart of HavenDOS's heap_check(), which was one of the better ideas in its heap rewrite.

#### Virtual memory

**`struct arch_pagetable`**

**`void vmm_init(const struct boot_info *bi)`**

#### Map / unmap in the kernel address space.

**`int vmm_map(vaddr_t va, paddr_t pa, size_t len, uint32_t prot)`**

**`int vmm_unmap(vaddr_t va, size_t len)`**

**`int vmm_protect(vaddr_t va, size_t len, uint32_t prot)`**

**`vaddr_t vmm_alloc(size_t len, uint32_t prot)`**

Reserve `len` bytes of kernel virtual address space and back it with freshly allocated physical pages.  The pages need not be contiguous, which is the entire point: a 4 MiB allocation does not need 4 MiB of adjacent physical memory, and requiring it is why HavenDOS's large buffers had to be contiguous runs from the PMM.

**`void vmm_free(vaddr_t va, size_t len)`**

**`vaddr_t vmm_map_mmio(paddr_t pa, size_t len)`**

Map a device's MMIO aperture into the kernel's device window, uncached.  Returns 0 on failure.

**`void vmm_unmap_mmio(vaddr_t va, size_t len)`**

**`void vmm_protect_kernel(void)`**

Apply final permissions to the kernel image: .text read-execute, .rodata read-only and no-execute, .data/.bss read-write and no-execute.  Called once the boot-time mappings are no longer needed. HavenDOS ran entirely with a flat read-write-execute segment, so a stray write into its own code was simply a write.

**`vaddr_t vmm_alloc_kstack(void)`**

A kernel stack with an unmapped guard page below it, so a stack overflow faults at a known address instead of silently overwriting whatever is adjacent.  HavenDOS's 256 KiB boot stack had no guard, and the console replay bug caught during this project's own bring-up — a 64 KiB buffer on a 64 KiB stack — is exactly what a guard page turns from corruption into a diagnosable fault.

**`void vmm_free_kstack(vaddr_t stack_top)`**

**`struct vmm_stats`**

```c
struct vmm_stats {
	uint64_t mapped_pages;
	uint64_t arena_used_bytes;
	uint64_t arena_total_bytes;
	uint64_t mmio_used_bytes;
	uint64_t pagetable_pages;
};
```

**`void vmm_get_stats(struct vmm_stats *out)`**

#### Kernel allocator

**`void kmalloc_init(void)`**

**`void *kmalloc(size_t size)`**

**`void *kzalloc(size_t size)`**

**`void *kcalloc(size_t n, size_t size)`**

**`void *krealloc(void *ptr, size_t size)`**

**`void kfree(void *ptr)`**

**`size_t kmalloc_size(const void *ptr)`**

Allocation size as the allocator recorded it, for realloc and for leak accounting.  Returns 0 for a pointer the allocator does not own.

**`struct kmalloc_stats`**

```c
struct kmalloc_stats {
	uint64_t live_bytes;
	uint64_t live_objects;
	uint64_t total_allocs;
	uint64_t total_frees;
	uint64_t failed_allocs;
	uint64_t slab_pages;
};
```

**`void kmalloc_get_stats(struct kmalloc_stats *out)`**

**`bool kmalloc_check(void)`**

Validate every slab cache and the large-allocation list.  Keeps the corruption detection HavenDOS added to its heap in v0.6.8 — magic values on every header, with double-free and use-after-free detection — but over an allocator that can grow.

**`void kmalloc_dump(void)`**

### `haven/dma.h`

memory a device can reach.

*4 declarations, 2 carrying documentation.*

**`struct dma_region`**

```c
struct dma_region {
	vaddr_t  virt;     /* reachable by the CPU, through the direct map */
	paddr_t  phys;     /* what the device is told */
	size_t   bytes;
	size_t   pages;
};
```

**`int dma_alloc_coherent(struct dma_region *out, size_t bytes, paddr_t addr_limit)`**

Allocate `bytes` of physically contiguous memory a device can address.

`addr_limit` is the highest physical address the device can reach: PMM_ZONE_DMA32_LIMIT for a 32-bit PCI device, 0 for no limit.  Getting this wrong does not fail -- the device simply DMAs to a truncated address, corrupting whatever is there, which is why the limit is a required argument rather than an optional hint.

**`void dma_free_coherent(struct dma_region *region)`**

**`bool dma_addr_is_safe(vaddr_t addr, size_t len, paddr_t addr_limit)`**

Is `addr` already safe to hand to a device?

True only for an address inside the direct map, which is where PMM-allocated memory appears and where virt_to_phys() is meaningful. A driver can use this to skip the bounce copy when a caller happens to have passed DMA-safe memory.

## Execution

### `haven/sched.h`

threads and the scheduler.

*26 declarations, 7 carrying documentation.*

**`struct arch_context`**

**`struct process`**

**`enum thread_state`**

```c
enum thread_state {
	THREAD_NEW = 0,    /* created, never run */
	THREAD_READY,      /* on a run queue */
	THREAD_RUNNING,    /* on a CPU now */
	THREAD_BLOCKED,    /* on a wait queue, waiting for an event */
	THREAD_SLEEPING,   /* waiting for a time to arrive */
	THREAD_ZOMBIE,     /* finished; exit status not yet collected */
	THREAD_DEAD,       /* reaped */
};
```

**`THREAD_NAME_MAX`** = `32`

**`SCHED_SLICE_TICKS`** = `4`

A thread's time slice, in ticks.  Four ticks at 100 Hz is 40 ms: long enough that the switch cost is lost in the noise, short enough that eight runnable threads all get a turn within a third of a second.

**`struct thread`**

```c
struct thread {
	/* The context must come first.  arch_context_switch() is handed a
	   pointer to it, and putting it at offset 0 means the assembly can
	   use the thread pointer directly -- one fewer address calculation
	   on the hottest path in the kernel. */
	struct arch_context *context;

	tid_t              tid;
	char               name[THREAD_NAME_MAX];
	enum thread_state  state;

	uint8_t            priority;        /* 0..SCHED_PRIO_LEVELS-1 */
	uint8_t            base_priority;   /* restored after a boost */
	int32_t            slice_remaining; /* ticks left in this turn */

	vaddr_t            kstack_top;      /* for freeing, and for the TSS */
	struct process    *proc;            /* NULL for a kernel thread */

	/* Queue membership.  A thread is on exactly one of: a run queue, a
	   wait queue, or nothing.  `proc_node` is a second list entirely --
	   its process's thread list -- which is why these are intrusive
	   lists and not an array index. */
	struct list_head   queue_node;
	struct list_head   proc_node;

	/* Threads blocked in thread_join() on this thread.  A per-thread
	   wait queue rather than a single waiter pointer, because more than
	   one thread may legitimately join the same thread. */
	struct list_head   join_waiters;

	/* Wake-up bookkeeping. */
	nstime_t           wake_at;         /* for THREAD_SLEEPING */
	struct list_head  *wait_queue;      /* for THREAD_BLOCKED */
	bool               wake_pending;    /* woken before it blocked */

	/* Accounting, for `ps`. */
	nstime_t           cpu_time_ns;
	nstime_t           last_scheduled;
	uint64_t           switch_count;
	uint32_t           home_cpu;

	int                exit_code;
};
```

**`void sched_init(void)`**

**`void sched_init_ap(void)`**

Join an application processor to the scheduler: give it an idle thread and an empty run queue, and adopt its current context as a thread. Called from smp_ap_entry().

#### Create a kernel thread.  It starts READY, not running.

**`struct thread *thread_create(const char *name, void (*entry)(void *), void *arg, uint8_t priority)`**

**`struct thread *thread_current(void)`**

The running thread on this CPU.  Never NULL once sched_init() has run: a CPU with nothing to do runs its idle thread.

#### Give up the rest of this slice.

**`void sched_yield(void)`**

**`void sched_switch(void)`**

Pick and switch to the next thread.  Safe to call with a lock held only if that lock is released first -- holding a spinlock across a context switch deadlocks the moment the next thread wants it.

**`void sched_tick(void)`**

Called once per tick from the timer interrupt.  It only RECORDS that a switch is due; the switch itself cannot happen in interrupt context (see rule 1 in kernel/sched/sched.c).

**`void sched_preempt_if_needed(void)`**

Perform a switch the tick asked for, from a context where switching is safe.  Called from the idle loop and from the syscall return path.

#### Make a thread runnable.  Safe from interrupt context.

**`void sched_wake(struct thread *t)`**

#### Block the current thread on `queue` until something wakes it.

**`void sched_block_on(struct list_head *queue)`**

#### Wake one / all threads on `queue`.

**`void sched_wake_one(struct list_head *queue)`**

**`void sched_wake_all(struct list_head *queue)`**

#### Terminate the current thread with `code`.  Never returns.

**`void thread_exit(int code)`**

Does not return.

#### Block until `t` has exited; returns its exit code.

**`int thread_join(struct thread *t)`**

**`void thread_set_priority(struct thread *t, uint8_t priority)`**

**`const char *thread_state_name(enum thread_state s)`**

**`struct sched_stats`**

```c
struct sched_stats {
	uint64_t context_switches;
	uint64_t ticks;
	uint64_t preemptions;
	uint64_t voluntary_yields;
	uint64_t threads_created;
	uint64_t threads_exited;
	uint32_t threads_live;
	uint32_t threads_runnable;
};
```

**`void sched_get_stats(struct sched_stats *out)`**

**`void sched_dump(void)`**

**`bool sched_active(void)`**

Is the scheduler running?  Before it is, sleeping is a busy-wait and blocking is a bug -- so code that can run in both phases has to be able to ask.

### `haven/smp.h`

per-CPU state and multi-processor support.

*23 declarations, 6 carrying documentation.*

**`HAVEN_MAX_CPUS`** = `64`

**`struct thread`**

**`struct arch_pagetable`**

**`SCHED_PRIO_LEVELS`** = `8`

Scheduler priority bands.  See kernel/sched/sched.c.

**`SCHED_PRIO_IDLE`** = `(SCHED_PRIO_LEVELS - 1)`

**`SCHED_PRIO_NORMAL`** = `4`

**`SCHED_PRIO_HIGH`** = `1`

**`struct haven_cpu`**

```c
struct haven_cpu {
	/* Identity.  cpu_id is a dense logical index 0..ncpus-1; the
	   hardware APIC ID is sparse and must not be used as an array
	   index, which is a mistake that costs a boot on any machine with
	   a non-contiguous APIC numbering. */
	uint32_t cpu_id;
	uint32_t hw_id;

	/* Currently running and idle threads. */
	struct thread *current;
	struct thread *idle;

	/* Ring-0 stack this CPU's interrupt and syscall entry paths use. */
	vaddr_t  kernel_stack_top;

	/* Where the SYSCALL entry path parks the user stack pointer.  It
	   has to live somewhere the kernel can reach with one instruction
	   and before it owns a stack, which means per-CPU memory and not
	   a register.  arch/x86_64/cpu/entry.S reads this at a hard-coded
	   offset; percpu.c asserts that the offset is still right. */
	vaddr_t  user_rsp;

	/* Run queues, one list per priority band.  Per-CPU so that the
	   common case — picking the next thread — touches no shared state
	   and needs no cross-CPU lock. */
	struct list_head runqueue[SCHED_PRIO_LEVELS];
	uint32_t         runnable;         /* bitmask of non-empty bands */
	uint32_t         nr_runnable;

	/* Nesting depths.  preempt_count > 0 means "do not reschedule here";
	   irq_nesting > 0 means "we are in interrupt context".  Tracking
	   both is what lets the scheduler assert it is never entered from a
	   context that cannot survive a context switch. */
	uint32_t preempt_count;
	uint32_t irq_nesting;

	/* Set by the timer tick when the running thread's slice is spent,
	   or when a higher-priority thread becomes runnable.  Acted on at
	   the next point where switching is safe -- which, crucially,
	   includes the interrupt return path; see the comment on rule 1 in
	   kernel/sched/sched.c. */
	bool     need_resched;

	/* Statistics, for `cpuinfo`. */
	uint64_t ticks;
	uint64_t context_switches;
	uint64_t idle_ns;

	bool online;
	bool is_bsp;
};
```

**`struct haven_cpu *this_cpu(void)`**

The per-CPU block for the running CPU.  Cheap: one segment-relative load on x86_64, one system-register read on AArch64.

#### By logical index; NULL when out of range or not yet online.

**`struct haven_cpu *cpu_by_id(uint32_t cpu_id)`**

**`uint32_t smp_cpu_count(void)`**

**`uint32_t smp_online_count(void)`**

**`void smp_init_bsp(void)`**

Bring up the per-CPU area for the boot CPU.  Must run before anything calls this_cpu().

**`int smp_start_secondary(void)`**

Start the application processors.  Returns the number brought online, not counting the BSP.  A CPU that fails to start is reported and skipped: a machine that boots on 3 of 4 cores is far better than one that refuses to boot at all.

#### Entry point for a freshly started application processor.

**`void smp_ap_entry(uint32_t cpu_id)`**

Does not return.

**`IPI_VECTOR_RESCHEDULE`** = `50`

IPI purposes, named portably.  The architecture maps each to a vector; the generic layer never names a vector number.

**`IPI_VECTOR_TLB_FLUSH`** = `51`

**`IPI_VECTOR_HALT`** = `52`

**`IPI_VECTOR_CALL`** = `53`

**`void preempt_disable(void)`**

Preemption control.  These nest.

**`void preempt_enable(void)`**

**`bool preempt_enabled(void)`**

**`bool in_interrupt(void)`**

### `haven/spinlock.h`

mutual exclusion that does not sleep.

*8 declarations, 1 carrying documentation.*

**`struct spinlock`**

```c
struct spinlock {
	uint32_t    next_ticket;   /* the number the next arrival takes */
	uint32_t    now_serving;   /* the number currently admitted */
	const char *name;
	uint32_t    holder_cpu;    /* for diagnostics; valid while held */
	bool        held;
};
```

**`SPINLOCK_INIT(nm)`**

**`DEFINE_SPINLOCK(var, nm)`**

**`void spin_lock_init(struct spinlock *lock, const char *name)`**

**`irqflags_t spin_lock(struct spinlock *lock)`**

Acquire, returning the interrupt state to pass back to spin_unlock(). Interrupts are disabled for the duration.

**`void spin_unlock(struct spinlock *lock, irqflags_t flags)`**

#### Try once.  Returns true and fills `flags` on success.

**`bool spin_trylock(struct spinlock *lock, irqflags_t *flags)`**

**`bool spin_is_held(const struct spinlock *lock)`**

### `haven/mutex.h`

mutual exclusion that sleeps.

*18 declarations, 3 carrying documentation.*

**`struct thread`**

**`struct mutex`**

```c
struct mutex {
	struct spinlock   guard;      /* protects the fields below */
	struct thread    *owner;
	struct list_head  waiters;
	const char       *name;
	uint32_t          depth;      /* recursion depth, owner only */
	bool              recursive;
};
```

**`void mutex_init(struct mutex *m, const char *name)`**

**`void mutex_init_recursive(struct mutex *m, const char *name)`**

**`void mutex_lock(struct mutex *m)`**

**`void mutex_unlock(struct mutex *m)`**

**`bool mutex_trylock(struct mutex *m)`**

**`bool mutex_is_locked(const struct mutex *m)`**

**`struct semaphore`**

A counting semaphore, for producer/consumer and for bounding how many threads may be inside a resource at once.

```c
struct semaphore {
	struct spinlock  guard;
	int32_t          count;
	struct list_head waiters;
	const char      *name;
};
```

**`void semaphore_init(struct semaphore *s, int32_t initial, const char *name)`**

**`void semaphore_wait(struct semaphore *s)`**

**`bool semaphore_trywait(struct semaphore *s)`**

**`void semaphore_post(struct semaphore *s)`**

**`struct condvar`**

A condition variable, used with a mutex.  Required whenever a thread must wait for a predicate rather than for a resource -- which is most blocking in a filesystem or a driver.

```c
struct condvar {
	struct spinlock  guard;
	struct list_head waiters;
	const char      *name;
};
```

**`void condvar_init(struct condvar *cv, const char *name)`**

**`void condvar_wait(struct condvar *cv, struct mutex *m)`**

Atomically release `m`, block, and re-acquire `m` on wake.  The atomicity is the whole point: a thread that released the mutex and then blocked could miss a signal that arrived in between.

**`void condvar_signal(struct condvar *cv)`**

**`void condvar_broadcast(struct condvar *cv)`**

### `haven/irq.h`

architecture-neutral interrupt handler registry.

*15 declarations, 5 carrying documentation.*

**`enum irq_result`**

What a handler tells the kernel about the interrupt it just saw. Returning HANDLED vs NONE is what makes sharing work: the dispatcher can tell whether anybody claimed the interrupt, and report a line that nobody claims instead of silently acknowledging it forever.

```c
enum irq_result {
	IRQ_NONE = 0,     /* not my device */
	IRQ_HANDLED,      /* dealt with */
	IRQ_WAKE_THREAD,  /* dealt with the urgent part; wake the handler
	                     thread for the rest */
};
```

**`typedef enum irq_result (*irq_handler_fn)(uint32_t irq, void *context)`**

**`struct irq_action`**

```c
struct irq_action {
	struct list_head node;
	irq_handler_fn   handler;
	void            *context;
	const char      *name;
	uint64_t         count;
	uint64_t         spurious;
};
```

**`IRQ_LINE_LEGACY(n)`**

The generic layer's line number IS the interrupt vector; see the comment at the top of arch/x86_64/irq/arch_irq.c.  A driver that wants a legacy 8259 line writes IRQ_LINE_LEGACY(n), so the mapping is visible where it is used rather than hidden in a translation table.

**`void irq_init(void)`**

**`int irq_register(uint32_t irq, struct irq_action *action)`**

Register `action` on line `irq`.  Several actions may share a line. Returns 0 or -HAVEN_E*.

**`int irq_unregister(uint32_t irq, struct irq_action *action)`**

**`int irq_request(uint32_t irq, irq_handler_fn handler, void *context, const char *name)`**

Convenience form that allocates the action.

**`void irq_enable_line(uint32_t irq)`**

**`void irq_disable_line(uint32_t irq)`**

**`void irq_dispatch(uint32_t irq)`**

Called by the architecture's trap handler for a hardware interrupt. Walks the line's actions, counts what happened, and reports a line that nobody claims — which is how a wrongly routed interrupt gets found instead of wedging the machine with an unacknowledged line.

**`void irq_dump(void)`**

**`struct irq_stats`**

```c
struct irq_stats {
	uint64_t total;
	uint64_t spurious;
	uint64_t unhandled;
};
```

**`void irq_get_stats(struct irq_stats *out)`**

**`uint64_t irq_count_for(uint32_t irq)`**

### `haven/time.h`

timekeeping, delays and timers.

*22 declarations, 6 carrying documentation.*

**`HZ`** = `100`

The scheduler tick rate.  100 Hz gives a 10 ms slice, which is long enough that the switch cost is negligible and short enough that interactive work stays responsive.  HavenDOS also used 100 Hz.

**`void time_init(void)`**

**`nstime_t time_ns(void)`**

Monotonic nanoseconds since boot.  Never goes backward and never wraps in any realistic uptime.

**`uint64_t time_ms(void)`**

**`uint64_t time_ticks(void)`**

Ticks since boot, on the CPU that is asking.  Note this is NOT a system-wide count once there are several CPUs: each has its own timer. Use time_ns() for anything that must be comparable across CPUs.

#### Busy-wait.  See the note above on which of these to use.

**`void time_delay_us(uint64_t us)`**

**`void time_delay_ms(uint64_t ms)`**

#### Block the calling thread.  Returns 0, or -HAVEN_EINTR if woken early.

**`int time_sleep_ms(uint64_t ms)`**

**`int time_sleep_ns(nstime_t ns)`**

**`struct timer`**

One-shot timers.  The callback runs in interrupt context on the CPU whose tick expired it, so it must not block, allocate or take a mutex.

```c
struct timer {
	struct list_head node;
	nstime_t         expires;
	void           (*callback)(struct timer *t, void *arg);
	void            *arg;
	bool             armed;
};
```

**`void timer_init_entry(struct timer *t, void (*callback)(struct timer *t, void *arg), void *arg)`**

**`void timer_arm_ns(struct timer *t, nstime_t delay_ns)`**

**`void timer_arm_ms(struct timer *t, uint64_t delay_ms)`**

**`bool timer_cancel(struct timer *t)`**

#### Called once per tick by the architecture's timer interrupt.

**`void timer_tick(void)`**

**`struct time_stats`**

```c
struct time_stats {
	uint64_t ticks;
	uint64_t timers_armed;
	uint64_t timers_fired;
	uint64_t timers_cancelled;
	uint64_t clock_hz;
	bool     clock_is_tsc;
};
```

**`void time_get_stats(struct time_stats *out)`**

**`struct haven_tm`**

Wall-clock date and time, read from the platform's real-time clock. Separate from the monotonic clock on purpose: wall-clock time can jump backward and must never be used to measure an interval.

```c
struct haven_tm {
	uint16_t year;
	uint8_t  month;     /* 1-12 */
	uint8_t  day;       /* 1-31 */
	uint8_t  hour;      /* 0-23 */
	uint8_t  minute;
	uint8_t  second;
	uint8_t  weekday;   /* 0 = Sunday */
};
```

**`int time_get_wallclock(struct haven_tm *out)`**

**`void time_format(const struct haven_tm *tm, char *buf, size_t bufsz)`**

**`int platform_read_rtc(struct haven_tm *out)`**

Supplied by a platform real-time-clock driver (drivers/char/rtc.c on a PC).  Weakly defined in kernel/time/timer.c, so a platform with no RTC links and reports that it has no wall clock rather than returning a plausible wrong date.

#### Bring up the platform's real-time clock.

**`void rtc_init(void)`**

## Processes and system calls

### `haven/proc.h`

processes: an address space, a file-descriptor table, and the threads that run inside them.

*29 declarations, 5 carrying documentation.*

**`PROC_NAME_MAX`** = `64`

**`PROC_MAX_FDS`** = `64`

**`PROC_MAX_ARGS`** = `32`

**`PROC_ARG_MAX`** = `4096`

**`struct arch_pagetable`**

**`struct thread`**

**`enum proc_state`**

```c
enum proc_state {
	PROC_NEW = 0,
	PROC_RUNNING,
	PROC_ZOMBIE,     /* exited; status not yet collected by its parent */
	PROC_DEAD,
};
```

**`struct process`**

```c
struct process {
	pid_t_k          pid;
	pid_t_k          ppid;
	char             name[PROC_NAME_MAX];
	enum proc_state  state;
	int              exit_code;

	/* The address space.  NULL for a kernel-only process, which is how
	   the init process starts before it execs anything. */
	struct arch_pagetable *pagetable;

	/* The user half of the address space, tracked so exit can free it
	   and so brk/mmap know where they may place things. */
	vaddr_t          image_start;
	vaddr_t          image_end;
	vaddr_t          heap_start;
	vaddr_t          heap_end;     /* the brk */
	vaddr_t          mmap_next;    /* bump pointer for mmap */
	vaddr_t          stack_top;

	/* Credentials.  Checked by the kernel, not by the filesystem. */
	uid_t_k          uid;
	gid_t_k          gid;

	/* Open files.  Per process, so one process closing a descriptor
	   cannot affect another -- which a global table could not
	   guarantee. */
	struct vfs_file *fds[PROC_MAX_FDS];
	struct mutex     fd_lock;

	char             cwd[VFS_PATH_MAX];

	/* Threads in this process, and the process tree. */
	struct list_head threads;
	struct list_head children;
	struct list_head sibling;
	struct process  *parent;

	/* Threads waiting in waitpid() for a child of this process. */
	struct list_head wait_queue;

	struct list_head all_node;     /* the global process list */
	uint32_t         thread_count;
	nstime_t         start_time_ns;
};
```

**`void proc_init(void)`**

**`struct process *proc_current(void)`**

The process the running thread belongs to, or NULL for a pure kernel thread.

**`pid_t_k proc_current_pid(void)`**

#### Create a process with an empty address space.  It has no threads yet.

**`struct process *proc_create(const char *name, struct process *parent)`**

**`int proc_load_elf(struct process *proc, const char *path, const char *const argv[], const char *const envp[])`**

Load an ELF64 executable from `path` into `proc` and create its first thread.  This is execve() without the argument marshalling.

#### Spawn a process from an executable in one call: create, load, start.

**`int proc_spawn(const char *path, const char *const argv[], const char *const envp[], pid_t_k *out_pid)`**

**`void proc_exit(int code)`**

Does not return.

**`int proc_wait(pid_t_k pid, int *status_out)`**

**`int proc_kill(pid_t_k pid, int signal)`**

**`bool proc_fault_kill(const char *reason, int signal)`**

Terminate the current process because of an unrecoverable fault. Returns false when there is no current process, which tells the architecture's fault handler to treat the fault as fatal.

#### File descriptors

**`int proc_fd_install(struct vfs_file *file)`**

Install `file` in the lowest free descriptor.  Returns the descriptor or -HAVEN_EMFILE.  Takes ownership on success.

**`struct vfs_file *proc_fd_get(int fd)`**

Look up a descriptor, taking a reference.  Must be released with proc_fd_put(), which is what stops a close() on another thread from freeing the file mid-read.

**`void proc_fd_put(struct vfs_file *file)`**

**`int proc_fd_close(int fd)`**

**`int proc_fd_dup(int fd)`**

**`int proc_set_cwd(const char *path)`**

**`int proc_get_cwd(char *out, size_t outsz)`**

#### Resolve a possibly-relative path against the process's cwd.

**`int proc_resolve_path(const char *in, char *out, size_t outsz)`**

**`struct proc_stats`**

```c
struct proc_stats {
	uint32_t live;
	uint32_t zombies;
	uint64_t created;
	uint64_t exited;
};
```

**`void proc_get_stats(struct proc_stats *out)`**

**`void proc_dump(void)`**

### `haven/elf.h`

ELF64 structures and the loader interface.

*35 declarations, 3 carrying documentation.*

**`ELF_MAGIC0`** = `0x7F`

**`ELF_MAGIC1`** = `'E'`

**`ELF_MAGIC2`** = `'L'`

**`ELF_MAGIC3`** = `'F'`

**`ELFCLASS64`** = `2`

**`ELFDATA2LSB`** = `1`

**`EV_CURRENT`** = `1`

**`ET_NONE`** = `0`

**`ET_REL`** = `1`

**`ET_EXEC`** = `2`

**`ET_DYN`** = `3`

**`ET_CORE`** = `4`

**`EM_X86_64`** = `62`

**`EM_AARCH64`** = `183`

**`PT_NULL`** = `0`

**`PT_LOAD`** = `1`

**`PT_DYNAMIC`** = `2`

**`PT_INTERP`** = `3`

**`PT_NOTE`** = `4`

**`PT_PHDR`** = `6`

**`PT_TLS`** = `7`

**`PT_GNU_STACK`** = `0x6474E551`

**`PF_X`** = `0x1`

**`PF_W`** = `0x2`

**`PF_R`** = `0x4`

**`struct elf64_header`**

```c
struct elf64_header {
	uint8_t  e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} HAVEN_PACKED;
```

**`struct elf64_phdr`**

```c
struct elf64_phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} HAVEN_PACKED;
```

**`HAVEN_STATIC_ASSERT(sizeof(struct elf64_header) == 64, "ELF64 header is 64 bytes")`**

**`HAVEN_STATIC_ASSERT(sizeof(struct elf64_phdr) == 56, "ELF64 program header is 56 bytes")`**

**`struct elf_image`**

What the loader produces.

```c
struct elf_image {
	vaddr_t entry;
	vaddr_t load_start;    /* lowest mapped address */
	vaddr_t load_end;      /* highest mapped address, page-rounded */
	size_t  segments;
};
```

**`struct arch_pagetable`**

**`int elf_validate(const void *data, size_t len, uint16_t expect_machine)`**

Validate an ELF64 header without loading anything.  Separated so a caller can report "not an executable" cheaply and so the validation is directly testable -- which matters because every field here is attacker-controlled.  Returns 0 or -HAVEN_E*.

**`int elf_load(struct arch_pagetable *pt, const void *data, size_t len, struct elf_image *out)`**

Load every PT_LOAD segment of the image in `data` into `pt`.  The caller owns `pt` and must tear it down on failure; the loader does not attempt partial cleanup, because a half-loaded address space has no valid interpretation.

**`const char *elf_type_name(uint16_t type)`**

**`const char *elf_machine_name(uint16_t machine)`**

### `haven/syscall.h`

the system-call interface.

*74 declarations, 3 carrying documentation.*

#### Syscall numbers.  Append only.

**`SYS_exit`** = `0`

**`SYS_write`** = `1`

**`SYS_read`** = `2`

**`SYS_open`** = `3`

**`SYS_close`** = `4`

**`SYS_lseek`** = `5`

**`SYS_stat`** = `6`

**`SYS_getpid`** = `7`

**`SYS_gettid`** = `8`

**`SYS_yield`** = `9`

**`SYS_nanosleep`** = `10`

**`SYS_uname`** = `11`

**`SYS_brk`** = `12`

**`SYS_mmap`** = `13`

**`SYS_munmap`** = `14`

**`SYS_fork`** = `15`

**`SYS_execve`** = `16`

**`SYS_waitpid`** = `17`

**`SYS_kill`** = `18`

**`SYS_getcwd`** = `19`

**`SYS_chdir`** = `20`

**`SYS_mkdir`** = `21`

**`SYS_unlink`** = `22`

**`SYS_readdir`** = `23`

**`SYS_dup`** = `24`

**`SYS_pipe`** = `25`

**`SYS_time`** = `26`

**`SYS_sysinfo`** = `27`

**`SYS_thread_create`** = `28`

**`SYS_thread_join`** = `29`

**`SYS_MAX`** = `30`

**`SYSCALL_ARG_MAX`** = `6`

**`struct syscall_args`**

What a handler receives.  Passing a struct rather than six loose arguments means the dispatcher can validate and log uniformly, and a handler that wants fewer arguments does not have to declare six.

```c
struct syscall_args {
	uint64_t nr;
	uint64_t arg[SYSCALL_ARG_MAX];
};
```

**`typedef int64_t (*syscall_fn)(const struct syscall_args *a)`**

**`void syscall_init(void)`**

**`int64_t syscall_invoke(struct syscall_args *a)`**

The portable half of the dispatcher: validate the number, look up the handler, call it, and return the result.  The architecture's entry path unpacks its own register layout into struct syscall_args and calls this.

**`const char *syscall_name(uint64_t nr)`**

**`struct syscall_stats`**

```c
struct syscall_stats {
	uint64_t total;
	uint64_t bad_number;
	uint64_t not_implemented;
	uint64_t faults;
};
```

**`void syscall_get_stats(struct syscall_stats *out)`**

**`void syscall_dump(void)`**

**`int copy_from_user(void *dst, const void *user_src, size_t len)`**

Copying across the boundary.  These wrap the architecture's fault-tolerant primitives and are what every handler must use instead of dereferencing a user pointer directly.

**`int copy_to_user(void *user_dst, const void *src, size_t len)`**

**`int strncpy_from_user(char *dst, const char *user_src, size_t len)`**

**`int64_t sys_exit(const struct syscall_args *a)`**

**`int64_t sys_write(const struct syscall_args *a)`**

**`int64_t sys_read(const struct syscall_args *a)`**

**`int64_t sys_open(const struct syscall_args *a)`**

**`int64_t sys_close(const struct syscall_args *a)`**

**`int64_t sys_lseek(const struct syscall_args *a)`**

**`int64_t sys_stat(const struct syscall_args *a)`**

**`int64_t sys_getpid(const struct syscall_args *a)`**

**`int64_t sys_gettid(const struct syscall_args *a)`**

**`int64_t sys_yield(const struct syscall_args *a)`**

**`int64_t sys_nanosleep(const struct syscall_args *a)`**

**`int64_t sys_uname(const struct syscall_args *a)`**

**`int64_t sys_brk(const struct syscall_args *a)`**

**`int64_t sys_mmap(const struct syscall_args *a)`**

**`int64_t sys_munmap(const struct syscall_args *a)`**

**`int64_t sys_fork(const struct syscall_args *a)`**

**`int64_t sys_execve(const struct syscall_args *a)`**

**`int64_t sys_waitpid(const struct syscall_args *a)`**

**`int64_t sys_kill(const struct syscall_args *a)`**

**`int64_t sys_getcwd(const struct syscall_args *a)`**

**`int64_t sys_chdir(const struct syscall_args *a)`**

**`int64_t sys_mkdir(const struct syscall_args *a)`**

**`int64_t sys_unlink(const struct syscall_args *a)`**

**`int64_t sys_readdir(const struct syscall_args *a)`**

**`int64_t sys_dup(const struct syscall_args *a)`**

**`int64_t sys_pipe(const struct syscall_args *a)`**

**`int64_t sys_time(const struct syscall_args *a)`**

**`int64_t sys_sysinfo(const struct syscall_args *a)`**

**`int64_t sys_thread_create(const struct syscall_args *a)`**

**`int64_t sys_thread_join(const struct syscall_args *a)`**

**`struct haven_utsname`**

```c
struct haven_utsname {
	char sysname[32];
	char release[32];
	char version[64];
	char machine[32];
	char nodename[32];
};
```

## Filesystems

### `haven/vfs.h`

the virtual filesystem switch.

*67 declarations, 8 carrying documentation.*

**`VFS_NAME_MAX`** = `255`

**`VFS_PATH_MAX`** = `1024`

**`VFS_MAX_MOUNTS`** = `8`

**`enum vfs_node_type`**

```c
enum vfs_node_type {
	VFS_NODE_NONE = 0,
	VFS_NODE_FILE,
	VFS_NODE_DIR,
	VFS_NODE_CHARDEV,
	VFS_NODE_BLOCKDEV,
	VFS_NODE_SYMLINK,
	VFS_NODE_FIFO,
};
```

**`VFS_PERM_OTHER_X`** = `0001`

Permission bits, in the familiar octal arrangement.  HavenDOS had a single `admin_only` boolean, which cannot express "readable by anyone, writable by its owner" -- the permission almost every file wants.

**`VFS_PERM_OTHER_W`** = `0002`

**`VFS_PERM_OTHER_R`** = `0004`

**`VFS_PERM_GROUP_X`** = `0010`

**`VFS_PERM_GROUP_W`** = `0020`

**`VFS_PERM_GROUP_R`** = `0040`

**`VFS_PERM_OWNER_X`** = `0100`

**`VFS_PERM_OWNER_W`** = `0200`

**`VFS_PERM_OWNER_R`** = `0400`

**`VFS_PERM_DEFAULT_FILE`** = `0644`

**`VFS_PERM_DEFAULT_DIR`** = `0755`

**`O_RDONLY`** = `0x0000`

Open flags.

**`O_WRONLY`** = `0x0001`

**`O_RDWR`** = `0x0002`

**`O_ACCMODE`** = `0x0003`

**`O_CREAT`** = `0x0040`

**`O_EXCL`** = `0x0080`

**`O_TRUNC`** = `0x0200`

**`O_APPEND`** = `0x0400`

**`O_DIRECTORY`** = `0x10000`

**`SEEK_SET_`** = `0`

Seek origins.

**`SEEK_CUR_`** = `1`

**`SEEK_END_`** = `2`

**`struct vfs_node`**

**`struct vfs_mount`**

**`struct vfs_file`**

**`struct vfs_stat`**

```c
struct vfs_stat {
	uint64_t size;
	uint32_t type;        /* enum vfs_node_type */
	uint32_t perm;
	uid_t_k  uid;
	gid_t_k  gid;
	uint64_t mtime_ns;
	uint32_t nlink;
	uint32_t dev_major;
	uint32_t dev_minor;
};
```

#### One directory entry, as readdir() yields it.

**`struct vfs_dirent`**

```c
struct vfs_dirent {
	char     name[VFS_NAME_MAX + 1];
	uint64_t size;
	uint32_t type;
	uint32_t index;
};
```

**`struct vfs_ops`**

The per-filesystem operation table.

Every function returns 0 or a positive count on success and -HAVEN_E* on failure.  A NULL pointer means the filesystem does not implement the operation, and the VFS returns -ENOSYS -- which is the distinction a bare "return 0" with a not-implemented comment could not make, and which HavenDOS's vfs_mkdir() got wrong for FAT12.

```c
struct vfs_ops {
	const char *name;

	/* Look up a single path component inside `dir`.  The VFS walks
	   paths itself and calls this once per component, so a filesystem
	   never has to parse a path -- which is how HavenDOS ended up with
	   a slash inside an 8.3 filename. */
	int  (*lookup)(struct vfs_node *dir, const char *name,
	               struct vfs_node **out);

	int  (*create)(struct vfs_node *dir, const char *name, uint32_t perm,
	               enum vfs_node_type type, struct vfs_node **out);
	int  (*unlink)(struct vfs_node *dir, const char *name);
	int  (*rename)(struct vfs_node *dir, const char *from,
	               struct vfs_node *newdir, const char *to);

	ssize_t_k (*read)(struct vfs_node *node, void *buf, size_t len,
	                  haven_off_t offset);
	ssize_t_k (*write)(struct vfs_node *node, const void *buf, size_t len,
	                   haven_off_t offset);
	int  (*truncate)(struct vfs_node *node, uint64_t length);

	int  (*readdir)(struct vfs_node *dir, uint32_t index,
	                struct vfs_dirent *out);

	int  (*stat)(struct vfs_node *node, struct vfs_stat *out);
	int  (*chmod)(struct vfs_node *node, uint32_t perm);
	int  (*chown)(struct vfs_node *node, uid_t_k uid, gid_t_k gid);

	int  (*sync)(struct vfs_mount *mount);
	void (*release)(struct vfs_node *node);
	void (*unmount)(struct vfs_mount *mount);

	/* Character and block devices route through here, which is what
	   lets /dev/console be a file. */
	int  (*ioctl)(struct vfs_node *node, uint32_t cmd, void *arg);
};
```

**`struct vfs_node`**

```c
struct vfs_node {
	char               name[VFS_NAME_MAX + 1];
	enum vfs_node_type type;
	uint32_t           perm;
	uid_t_k            uid;
	gid_t_k            gid;
	uint64_t           size;
	uint64_t           mtime_ns;

	struct vfs_mount  *mount;     /* the filesystem that owns it */
	void              *fs_data;   /* the filesystem's own handle */
	uint32_t           refcount;

	/* Device nodes. */
	uint32_t           dev_major;
	uint32_t           dev_minor;
};
```

**`struct vfs_mount`**

```c
struct vfs_mount {
	char                  path[VFS_PATH_MAX];
	const struct vfs_ops *ops;
	struct vfs_node      *root;
	void                 *fs_data;
	bool                  read_only;
	bool                  in_use;
};
```

**`struct vfs_file`**

An open file.  Separate from the node, because two processes may have the same file open at different offsets -- which a single node cannot represent.

```c
struct vfs_file {
	struct vfs_node *node;
	haven_off_t      offset;
	uint32_t         flags;
	uint32_t         refcount;
	struct mutex     lock;       /* serialises offset updates */
};
```

**`void vfs_init(void)`**

#### Mount `ops` at `path`.  The first mount must be at "/".

**`int vfs_mount(const char *path, const struct vfs_ops *ops, void *fs_data, struct vfs_node *root)`**

**`int vfs_unmount(const char *path)`**

**`int vfs_resolve(const char *path, struct vfs_node **out)`**

Resolve a path to a node.  The returned node holds a reference the caller must release with vfs_node_put().

**`void vfs_node_get(struct vfs_node *node)`**

**`void vfs_node_put(struct vfs_node *node)`**

#### File-level interface, which is what the syscall layer uses.

**`int vfs_open(const char *path, uint32_t flags, uint32_t perm, struct vfs_file **out)`**

**`int vfs_close(struct vfs_file *file)`**

**`ssize_t_k vfs_read(struct vfs_file *file, void *buf, size_t len)`**

**`ssize_t_k vfs_write(struct vfs_file *file, const void *buf, size_t len)`**

**`int64_t vfs_seek(struct vfs_file *file, int64_t offset, int whence)`**

#### Positional forms, which do not touch the file offset.

**`ssize_t_k vfs_pread(struct vfs_node *node, void *buf, size_t len, haven_off_t offset)`**

**`ssize_t_k vfs_pwrite(struct vfs_node *node, const void *buf, size_t len, haven_off_t offset)`**

**`int vfs_stat_path(const char *path, struct vfs_stat *out)`**

**`int vfs_mkdir(const char *path, uint32_t perm)`**

**`int vfs_unlink(const char *path)`**

**`int vfs_rename(const char *from, const char *to)`**

**`int vfs_readdir_path(const char *path, uint32_t index, struct vfs_dirent *out)`**

**`int vfs_chmod(const char *path, uint32_t perm)`**

**`int vfs_chown(const char *path, uid_t_k uid, gid_t_k gid)`**

**`int vfs_truncate(const char *path, uint64_t length)`**

**`int vfs_sync(void)`**

**`ssize_t_k vfs_read_file(const char *path, void *buf, size_t bufsz)`**

Convenience: read or write a whole small file in one call.  Most kernel users of the filesystem want this rather than open/read/close.

**`ssize_t_k vfs_write_file(const char *path, const void *buf, size_t len)`**

**`bool vfs_exists(const char *path)`**

**`bool vfs_is_dir(const char *path)`**

**`int vfs_normalise_path(const char *in, char *out, size_t outsz)`**

Normalise a path: collapse "//", resolve "." and "..", and reject an attempt to escape above the root.  Exposed because it is worth testing directly -- a path-traversal bug here is a security hole, not just a correctness one.

**`const char *vfs_node_type_name(enum vfs_node_type t)`**

**`struct vfs_stats`**

```c
struct vfs_stats {
	uint64_t opens;
	uint64_t closes;
	uint64_t reads;
	uint64_t writes;
	uint64_t bytes_read;
	uint64_t bytes_written;
	uint64_t lookups;
	uint64_t lookup_misses;
	uint32_t mounts;
	uint32_t open_files;
};
```

**`void vfs_get_stats(struct vfs_stats *out)`**

**`void vfs_dump(void)`**

### `haven/ramfs.h`

the in-memory filesystem.

*3 declarations, 0 carrying documentation.*

**`struct vfs_mount`**

#### Create a fresh RAM filesystem and mount it at `path`.

**`int ramfs_mount_at(const char *path)`**

**`uint64_t ramfs_bytes_used(struct vfs_mount *mount)`**

### `haven/devfs.h`

the device filesystem.

*13 declarations, 1 carrying documentation.*

**`struct devfs_driver`**

```c
struct devfs_driver {
	const char *name;

	/* A NULL read or write means the device does not support that
	   direction, and the VFS returns -EBADF.  Leaving them NULL is the
	   way to express a write-only or read-only device; returning 0 from
	   a read would instead mean end of file. */
	ssize_t_k (*read)(void *data, void *buf, size_t len,
	                  haven_off_t offset);
	ssize_t_k (*write)(void *data, const void *buf, size_t len,
	                   haven_off_t offset);
	int       (*ioctl)(void *data, uint32_t cmd, void *arg);
};
```

**`int devfs_mount_at(const char *path)`**

**`int devfs_register(const char *name, const struct devfs_driver *driver, void *driver_data, enum vfs_node_type type, uint32_t major, uint32_t minor, uint32_t perm)`**

**`int devfs_unregister(const char *name)`**

**`uint32_t devfs_device_count(void)`**

**`DEV_MAJOR_CONSOLE`** = `1`

Device major numbers.  Fixed so a device node's identity is stable across boots, which matters the moment anything records one.

**`DEV_MAJOR_NULL`** = `2`

**`DEV_MAJOR_ZERO`** = `3`

**`DEV_MAJOR_RANDOM`** = `4`

**`DEV_MAJOR_SERIAL`** = `5`

**`DEV_MAJOR_KBD`** = `6`

**`DEV_MAJOR_BLOCK`** = `8`

**`DEV_MAJOR_RTC`** = `10`

### `haven/fat.h`

the FAT filesystem.

*5 declarations, 1 carrying documentation.*

**`struct block_device`**

**`enum fat_variant`**

```c
enum fat_variant {
	FAT_UNKNOWN = 0,
	FAT12,
	FAT16,
	FAT32,
};
```

**`int fat_mount(struct block_device *dev, const char *path, bool read_only)`**

Probe `dev` for a FAT filesystem and mount it at `path`. Returns 0, or -HAVEN_E* -- in particular -HAVEN_ENODEV when the device holds no recognisable FAT, which is information rather than a failure.

#### Identify the variant on `dev` without mounting.

**`enum fat_variant fat_probe(struct block_device *dev)`**

**`const char *fat_variant_name(enum fat_variant v)`**

## Devices and drivers

### `haven/device.h`

the modular driver framework.

*15 declarations, 3 carrying documentation.*

**`DEVICE_NAME_MAX`** = `48`

**`enum device_bus`**

```c
enum device_bus {
	BUS_PLATFORM = 0,   /* fixed-address legacy hardware: PS/2, PIT, RTC */
	BUS_PCI,
	BUS_VIRTIO,
	BUS_COUNT
};
```

**`enum device_class`**

```c
enum device_class {
	DEVCLASS_OTHER = 0,
	DEVCLASS_BLOCK,
	DEVCLASS_CHAR,
	DEVCLASS_NET,
	DEVCLASS_DISPLAY,
	DEVCLASS_INPUT,
	DEVCLASS_BRIDGE,
	DEVCLASS_STORAGE_CTRL,
};
```

**`struct device_id`**

How a bus identifies a device to a driver.  A union rather than a per-bus struct, so one driver table covers every bus.

```c
struct device_id {
	enum device_bus bus;
	union {
		struct {
			uint16_t vendor;
			uint16_t device;
			uint8_t  class_code;
			uint8_t  subclass;
			uint8_t  prog_if;
		} pci;
		struct {
			const char *name;
		} platform;
	};
};
```

**`struct device`**

```c
struct device {
	char               name[DEVICE_NAME_MAX];
	enum device_bus    bus;
	enum device_class  cls;
	struct device_id   id;

	/* Where the device lives.  Which fields are meaningful depends on
	   the bus; a PCI device has BARs, a platform device has a port
	   range. */
	paddr_t  mmio_base;
	size_t   mmio_len;
	vaddr_t  mmio_virt;      /* filled in by the bus if mapped */
	uint16_t io_base;
	uint16_t io_len;
	uint32_t irq_line;       /* an IRQ line number, or 0 for none */

	/* PCI topology, for reporting and for config access. */
	uint8_t  pci_bus, pci_slot, pci_func;

	void              *driver_data;
	struct driver     *driver;
	struct list_head   node;
	bool               claimed;
};
```

**`struct driver`**

```c
struct driver {
	const char       *name;
	enum device_bus   bus;

	/* Return true if this driver can handle `dev`.  Must not touch the
	   hardware: it is called for every device on the bus, including
	   ones another driver owns. */
	bool (*match)(const struct device *dev);

	/* Take ownership.  Return 0 on success.  A driver that fails here
	   must leave the device exactly as it found it, because another
	   driver may be offered the same device next. */
	int  (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
};
```

**`HAVEN_DRIVER(var)`**

Register a driver into the link-time table the linker script collects at __haven_drivers_start.  No central list to edit, which is the whole point: a driver is added by adding a file.

**`void device_init(void)`**

**`int device_register(struct device *dev)`**

Register a device a bus has discovered, and offer it to every driver that matches.  Returns 0 if a driver claimed it, -HAVEN_ENODEV if none did -- which is information, not a failure: an unclaimed device is simply hardware HavenOS does not support yet, and reporting it is how someone learns what to write next.

**`struct device *device_find_by_class(enum device_class cls, unsigned index)`**

**`unsigned device_count(void)`**

**`unsigned device_count_claimed(void)`**

**`void device_dump(void)`**

**`const char *device_bus_name(enum device_bus bus)`**

**`const char *device_class_name(enum device_class cls)`**

### `haven/pci.h`

PCI configuration access, for drivers on the PCI bus.

*10 declarations, 3 carrying documentation.*

**`void pci_init(void)`**

**`uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)`**

**`uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)`**

**`void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value)`**

**`uint32_t pci_read_bar(const struct device *dev, unsigned index)`**

**`void pci_enable_device(struct device *dev, bool bus_master)`**

Enable a device's I/O and memory decoders, and optionally bus mastering.  Firmware does not always leave them on, and a driver that writes to a BAR without this sees its writes dropped and its reads return 0xFF -- indistinguishable from absent hardware.

**`PCI_VENDOR_INTEL`** = `0x8086`

Common vendor identifiers, for match functions.

**`PCI_VENDOR_REDHAT`** = `0x1AF4`

VirtIO

**`PCI_VENDOR_REALTEK`** = `0x10EC`

**`PCI_VENDOR_QEMU`** = `0x1234`

### `haven/block.h`

the block device layer.

*34 declarations, 4 carrying documentation.*

**`BLOCK_NAME_MAX`** = `32`

**`BLOCK_SECTOR_SIZE`** = `512`

**`BLOCK_MAX_DEVICES`** = `16`

**`struct block_device`**

**`struct block_ops`**

```c
struct block_ops {
	/* Read or write `count` sectors starting at `lba`.  Returns the
	   number of sectors transferred, or -HAVEN_E*.  A short transfer is
	   reported as a short count, not as success -- which is the
	   distinction that lets a filesystem retry the tail rather than
	   believing data it never wrote. */
	ssize_t_k (*read)(struct block_device *dev, uint64_t lba,
	                  void *buf, uint32_t count);
	ssize_t_k (*write)(struct block_device *dev, uint64_t lba,
	                   const void *buf, uint32_t count);
	int       (*flush)(struct block_device *dev);
};
```

**`struct block_device`**

```c
struct block_device {
	char     name[BLOCK_NAME_MAX];
	const struct block_ops *ops;
	void    *driver_data;

	uint32_t sector_size;
	uint64_t sector_count;
	bool     read_only;
	bool     removable;

	/* One lock per device.  Two threads issuing overlapping transfers
	   to the same ATA channel would interleave their register writes
	   and each read the other's data. */
	struct mutex lock;

	struct list_head node;
	uint64_t sectors_read;
	uint64_t sectors_written;
	uint64_t errors;
};
```

**`void block_init(void)`**

#### Register a device.  Also publishes it as /dev/<name>.

**`int block_register(struct block_device *dev)`**

**`int block_unregister(struct block_device *dev)`**

**`struct block_device *block_find(const char *name)`**

**`struct block_device *block_by_index(unsigned index)`**

**`unsigned block_count(void)`**

#### Sector-granular access, which is what a filesystem uses.

**`ssize_t_k block_read(struct block_device *dev, uint64_t lba, void *buf, uint32_t count)`**

**`ssize_t_k block_write(struct block_device *dev, uint64_t lba, const void *buf, uint32_t count)`**

**`int block_flush(struct block_device *dev)`**

**`ssize_t_k block_read_bytes(struct block_device *dev, uint64_t offset, void *buf, size_t len)`**

Byte-granular access, for a caller that does not want to think in sectors.  Handles the read-modify-write of a partial sector, which is the part that is easy to get wrong.

**`ssize_t_k block_write_bytes(struct block_device *dev, uint64_t offset, const void *buf, size_t len)`**

**`void block_dump(void)`**

#### Partitions

**`MBR_MAX_PARTITIONS`** = `4`

**`struct partition`**

```c
struct partition {
	uint64_t start_lba;
	uint64_t sector_count;
	uint8_t  type;
	bool     bootable;
	bool     present;
};
```

**`int block_read_partitions(struct block_device *dev, struct partition out[MBR_MAX_PARTITIONS])`**

Read and parse the MBR partition table.  HavenDOS's installer wrote an MBR with a FAT16 partition and its fat16_init() looked for exactly that; parsing the table properly means a HavenOS install can coexist with other partitions on the same disk.

**`int block_register_partition(struct block_device *parent, unsigned index, const struct partition *part)`**

Register a partition as its own block device, named <disk>p<n>, so a filesystem mounts a partition rather than computing offsets.

**`MBR_TYPE_EMPTY`** = `0x00`

**`MBR_TYPE_FAT12`** = `0x01`

**`MBR_TYPE_FAT16_SM`** = `0x04`

**`MBR_TYPE_EXTENDED`** = `0x05`

**`MBR_TYPE_FAT16`** = `0x06`

**`MBR_TYPE_NTFS`** = `0x07`

**`MBR_TYPE_FAT32`** = `0x0B`

**`MBR_TYPE_FAT32_LBA`** = `0x0C`

**`MBR_TYPE_FAT16_LBA`** = `0x0E`

**`MBR_TYPE_LINUX`** = `0x83`

**`MBR_TYPE_HAVENOS`** = `0x7E`

HavenOS native, not yet implemented

**`const char *mbr_type_name(uint8_t type)`**

### `haven/ata.h`

ATA/SATA disks over programmed I/O.

*2 declarations, 0 carrying documentation.*

**`void ata_init(void)`**

**`unsigned ata_drive_count(void)`**

### `haven/virtio.h`

VirtIO block devices.

*2 declarations, 0 carrying documentation.*

**`void virtio_blk_init(void)`**

**`unsigned virtio_blk_count(void)`**

### `haven/input.h`

keyboard input.

*5 declarations, 1 carrying documentation.*

**`void ps2kbd_init(void)`**

**`bool kbd_available(void)`**

**`int kbd_getchar(void)`**

Block until a character is available.  Returns -1 when no keyboard is present or the scheduler is not running (in which case blocking would hang the only thread).

#### Return a character if one is waiting, or -1.  Safe from any context.

**`int kbd_getchar_nonblock(void)`**

**`void kbd_get_stats(uint64_t *seen, uint64_t *dropped)`**

### `haven/fbcon.h`

the display console.

*3 declarations, 1 carrying documentation.*

**`int fbcon_init(const struct boot_framebuffer *bfb)`**

Bring up the display described by `bfb` and register it as a console sink.  Returns 0 on success, -1 when firmware gave us nothing usable — which is not fatal: the serial console carries on alone.

#### Character-grid geometry, 0 when no display is up.

**`void fbcon_geometry(uint32_t *cols, uint32_t *rows)`**

**`bool fbcon_available(void)`**

## Diagnostics

### `haven/selftest.h`

in-kernel tests that need real hardware.

*2 declarations, 1 carrying documentation.*

**`bool selftest_run_all(void)`**

Run every in-kernel test.  Returns true when all checks passed.  Safe to call on a live kernel: no test leaves state behind, and a test whose prerequisites are absent reports skipped rather than failing.

**`void selftest_summary(unsigned *run, unsigned *failed)`**

### `haven/shell.h`

the kernel shell.

*2 declarations, 0 carrying documentation.*

#### Start the shell as a kernel thread.  Returns 0 or -HAVEN_E*.

**`int shell_start(void)`**

#### Run the shell on the calling thread, returning when it exits.

**`void shell_run(void)`**

## Architecture-private headers

Reachable only from `arch/<arch>/` and from the architecture's
own code. Generic kernel code that includes one of these is a
portability bug, and `scripts/check-portability.sh` reports it.

### `haven/arch/arm64/memlayout.h`

the AArch64 virtual address layout.

*23 declarations, 10 carrying documentation.*

**`KERNEL_VMA_BASE`** = `0xFFFFFFFF80000000ULL`

Kernel image window.  The same value as x86_64 deliberately: it keeps the kernel's own addresses recognisable across both ports, and AArch64 has no equivalent of -mcmodel=kernel's 2 GiB constraint to satisfy.

**`KERNEL_LMA_BASE`** = `0x0000000040200000ULL`

typical AArch64 RAM base

**`HHDM_BASE`** = `0xFFFF000000000000ULL`

**`HHDM_SIZE`** = `0x0000400000000000ULL`

64 TiB

**`KHEAP_BASE`** = `0xFFFF400000000000ULL`

**`KHEAP_SIZE`** = `0x0000100000000000ULL`

16 TiB

**`MMIO_BASE`** = `0xFFFF500000000000ULL`

**`MMIO_SIZE`** = `0x0000100000000000ULL`

16 TiB

**`KSTACK_BASE`** = `0xFFFF600000000000ULL`

**`KSTACK_SIZE`** = `0x0000100000000000ULL`

**`KSTACK_PAGES`** = `4`

**`KSTACK_BYTES`** = `(KSTACK_PAGES * HAVEN_PAGE_SIZE)`

**`USER_MIN`** = `0x0000000000001000ULL`

**`USER_MAX`** = `0x0001000000000000ULL`

48-bit TTBR0 range

**`USER_STACK_TOP`** = `0x0000FFFFFFFFF000ULL`

**`USER_STACK_PAGES`** = `32`

**`PAGE_SIZE_4K`** = `0x1000ULL`

AArch64 page granules.  4 KiB is the one every implementation supports; 16 KiB and 64 KiB are optional and change the translation-table shape entirely, so the port starts with 4 KiB.

**`PAGE_SIZE_2M`** = `0x200000ULL`

a level-2 block

**`PAGE_SIZE_1G`** = `0x40000000ULL`

a level-1 block

**`PT_ENTRIES`** = `512`

**`static inline bool is_canonical(uint64_t addr)`**

With TTBR0 and TTBR1 both configured for a 48-bit range, an address is valid when its top 16 bits are all zeros (user, TTBR0) or all ones (kernel, TTBR1).  Anything else raises a translation fault, so the same check the x86_64 port calls "canonical" applies here too.

**`static inline bool is_user_addr(uint64_t addr)`**

**`static inline bool is_kernel_addr(uint64_t addr)`**

### `haven/arch/x86_64/acpi.h`

just enough ACPI to find the other CPUs.

*6 declarations, 0 carrying documentation.*

**`ACPI_MAX_CPUS`** = `64`

**`ACPI_MAX_IOAPICS`** = `4`

**`struct boot_info`**

**`struct acpi_info`**

```c
struct acpi_info {
	bool     present;
	uint8_t  revision;

	uint32_t cpu_count;
	uint8_t  apic_ids[ACPI_MAX_CPUS];

	uint64_t lapic_address;

	uint32_t ioapic_count;
	uint64_t ioapic_address[ACPI_MAX_IOAPICS];
	uint32_t ioapic_gsi_base[ACPI_MAX_IOAPICS];
};
```

**`const struct acpi_info *acpi_init(const struct boot_info *bi)`**

**`const struct acpi_info *acpi_get_info(void)`**

### `haven/arch/x86_64/boot.h`

x86_64 boot entry points shared between boot.S and C.

*6 declarations, 1 carrying documentation.*

**`void x86_boot_main(uint32_t magic, uint32_t mbinfo_pa)`**

Called from boot.S once the kernel is executing 64-bit code in the higher half.  Never returns: it ends in haven_kmain().

Does not return.

#### Multiboot2 handover values, captured in 32-bit mode by boot.S.

**`extern uint32_t boot_magic`**

**`extern uint32_t boot_mbinfo`**

#### Boot page tables and stack, defined in boot.S.

**`extern uint64_t boot_pml4[]`**

**`extern uint8_t boot_stack_bottom[]`**

**`extern uint8_t boot_stack_top[]`**

### `haven/arch/x86_64/cpu.h`

x86_64-private CPU facts and control-register helpers.

*24 declarations, 5 carrying documentation.*

#### Feature predicates, valid after arch_early_init().

**`bool arch_cpu_has_nx(void)`**

**`bool arch_cpu_has_1g_pages(void)`**

**`bool arch_cpu_has_apic(void)`**

**`bool arch_cpu_has_x2apic(void)`**

**`bool arch_cpu_has_invariant_tsc(void)`**

**`bool arch_cpu_has_tsc_deadline(void)`**

**`bool arch_cpu_has_smap(void)`**

**`bool arch_cpu_has_smep(void)`**

**`struct trapframe`**

Redirect a kernel-mode fault that landed inside a user-copy routine to that routine's recovery path, so a bad user pointer returns -EFAULT instead of panicking.  Returns false when the fault was somewhere else, in which case it is a real kernel bug.

**`bool uaccess_fixup(struct trapframe *tf)`**

#### Timekeeping internals shared between arch/x86_64 translation units.

**`void arch_time_init(void)`**

**`uint64_t arch_tsc_hz(void)`**

**`bool arch_tsc_usable(void)`**

**`void pit_start(uint32_t hz)`**

The 8253/8254 timer: calibration reference always, scheduler tick when there is no usable Local APIC.

**`uint32_t pit_rate(void)`**

**`uint64_t pit_tick_count(void)`**

#### Start the scheduler tick on an application processor.

**`void arch_timer_init_ap(uint32_t hz)`**

**`void exceptions_init(void)`**

Descriptor tables and the exception/IRQ plumbing. The GDT and TSS interface is in <haven/arch/x86_64/gdt.h>, and the IDT interface in <haven/arch/x86_64/idt.h>.

**`void arch_cpu_init_tables_ap(uint32_t cpu)`**

arch_cpu_init_tables() is declared in <haven/arch.h>, since kernel/main.c drives the boot sequence and must stay portable.  The application-processor form is x86-private.

**`void pic_init(void)`**

The 8259 PIC.  The Local APIC interface is in <haven/arch/x86_64/lapic.h>.

**`void pic_disable(void)`**

**`void pic_mask_all(void)`**

**`void pic_eoi(uint32_t irq)`**

**`void pic_set_mask(uint32_t irq, bool masked)`**

### `haven/arch/x86_64/gdt.h`

GDT slot and selector numbering.

*21 declarations, 9 carrying documentation.*

**`GDT_SLOT_NULL`** = `0`

**`GDT_SLOT_KERNEL_CODE`** = `1`

**`GDT_SLOT_KERNEL_DATA`** = `2`

**`GDT_SLOT_USER_DATA`** = `3`

**`GDT_SLOT_USER_CODE`** = `4`

**`GDT_SLOT_TSS`** = `5`

occupies slots 5 and 6: 16-byte descriptor with a 64-bit base

**`GDT_SEL_KERNEL_CODE`** = `(GDT_SLOT_KERNEL_CODE * 8)`

0x08

**`GDT_SEL_KERNEL_DATA`** = `(GDT_SLOT_KERNEL_DATA * 8)`

0x10

**`GDT_SEL_USER_DATA`** = `((GDT_SLOT_USER_DATA * 8) | 3)`

0x1B

**`GDT_SEL_USER_CODE`** = `((GDT_SLOT_USER_CODE * 8) | 3)`

0x23

**`GDT_SEL_TSS`** = `(GDT_SLOT_TSS * 8)`

0x28

**`IST_DOUBLE_FAULT`** = `1`

Interrupt Stack Table entries.  Each gets its own stack, used for traps that cannot rely on the interrupted context having a usable one.

**`IST_NMI`** = `2`

an NMI can arrive anywhere, including mid-stack-switch

**`IST_DEBUG`** = `3`

**`IST_DEPTH`** = `4`

entries actually backed with stacks

**`void gdt_init(void)`**

**`void gdt_init_cpu(uint32_t cpu)`**

**`void gdt_init_ap(uint32_t cpu)`**

**`void tss_init(uint32_t cpu, vaddr_t kstack_top)`**

**`void tss_set_kstack(uint32_t cpu, vaddr_t kstack_top)`**

**`vaddr_t tss_get_kstack(uint32_t cpu)`**

### `haven/arch/x86_64/idt.h`

interrupt vector assignments.

*32 declarations, 4 carrying documentation.*

#### 0-31 are CPU exceptions, fixed by the architecture.

**`VEC_DIVIDE_ERROR`** = `0`

**`VEC_DEBUG`** = `1`

**`VEC_NMI`** = `2`

**`VEC_BREAKPOINT`** = `3`

**`VEC_OVERFLOW`** = `4`

**`VEC_BOUND_RANGE`** = `5`

**`VEC_INVALID_OPCODE`** = `6`

**`VEC_DEVICE_NA`** = `7`

**`VEC_DOUBLE_FAULT`** = `8`

**`VEC_INVALID_TSS`** = `10`

**`VEC_SEGMENT_NP`** = `11`

**`VEC_STACK_FAULT`** = `12`

**`VEC_GP_FAULT`** = `13`

**`VEC_PAGE_FAULT`** = `14`

**`VEC_FPU_ERROR`** = `16`

**`VEC_ALIGNMENT`** = `17`

**`VEC_MACHINE_CHECK`** = `18`

**`VEC_SIMD_FP`** = `19`

**`VEC_VIRTUALISATION`** = `20`

**`VEC_CONTROL_PROT`** = `21`

**`VEC_IRQ_BASE`** = `32`

32-47: the legacy PIC's 16 lines, remapped out of the exception range. They must be remapped: the 8259 defaults to vectors 8-15, which collide with #DF, #TS, #NP, #SS, #GP, #PF and the FPU error -- so a timer tick would arrive as a double fault.

**`VEC_IRQ_COUNT`** = `16`

**`VEC_LAPIC_TIMER`** = `48`

48-: APIC and IPI vectors.  Kept above the PIC range so a system can run both during the handover from PIC to APIC.

**`VEC_LAPIC_ERROR`** = `49`

**`VEC_LAPIC_SPURIOUS`** = `255`

the APIC spec fixes the low nibble at 0xF; 255 is the conventional choice

**`VEC_IPI_RESCHEDULE`** = `50`

**`VEC_IPI_TLB_FLUSH`** = `51`

**`VEC_IPI_HALT`** = `52`

**`VEC_IPI_CALL`** = `53`

**`VEC_SYSCALL`** = `0x80`

The SYSCALL path does not go through the IDT, but it labels its synthetic trap frame with this so a backtrace can tell a syscall from an interrupt.

**`void idt_init(void)`**

**`void idt_load(void)`**

### `haven/arch/x86_64/io.h`

x86 port I/O, MSR and MMIO access.

*19 declarations, 2 carrying documentation.*

**`static HAVEN_ALWAYS_INLINE void outb(uint16_t port, uint8_t val)`**

**`static HAVEN_ALWAYS_INLINE void outw(uint16_t port, uint16_t val)`**

**`static HAVEN_ALWAYS_INLINE void outl(uint16_t port, uint32_t val)`**

**`static HAVEN_ALWAYS_INLINE uint8_t inb(uint16_t port)`**

**`static HAVEN_ALWAYS_INLINE uint16_t inw(uint16_t port)`**

**`static HAVEN_ALWAYS_INLINE uint32_t inl(uint16_t port)`**

**`static HAVEN_ALWAYS_INLINE void io_wait(void)`**

One I/O-bus delay, for legacy devices (8259, 8042) that need settling time between consecutive accesses.  Port 0x80 is the POST diagnostic port and is harmless to write.

**`static HAVEN_ALWAYS_INLINE uint64_t rdmsr(uint32_t msr)`**

**`static HAVEN_ALWAYS_INLINE void wrmsr(uint32_t msr, uint64_t val)`**

**`static HAVEN_ALWAYS_INLINE uint64_t rdtsc(void)`**

**`static HAVEN_ALWAYS_INLINE void cpuid_raw(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)`**

**`static HAVEN_ALWAYS_INLINE uint8_t mmio_read8(vaddr_t addr)`**

MMIO accessors.  volatile is load-bearing: without it the compiler is free to coalesce or elide device accesses.

**`static HAVEN_ALWAYS_INLINE uint16_t mmio_read16(vaddr_t addr)`**

**`static HAVEN_ALWAYS_INLINE uint32_t mmio_read32(vaddr_t addr)`**

**`static HAVEN_ALWAYS_INLINE uint64_t mmio_read64(vaddr_t addr)`**

**`static HAVEN_ALWAYS_INLINE void mmio_write8(vaddr_t addr, uint8_t v)`**

**`static HAVEN_ALWAYS_INLINE void mmio_write16(vaddr_t addr, uint16_t v)`**

**`static HAVEN_ALWAYS_INLINE void mmio_write32(vaddr_t addr, uint32_t v)`**

**`static HAVEN_ALWAYS_INLINE void mmio_write64(vaddr_t addr, uint64_t v)`**

### `haven/arch/x86_64/lapic.h`

Local APIC interface.

*8 declarations, 0 carrying documentation.*

**`bool lapic_init(void)`**

**`bool lapic_available(void)`**

**`uint32_t lapic_id(void)`**

**`void lapic_eoi(void)`**

**`void lapic_timer_init(uint32_t hz)`**

**`void lapic_timer_stop(void)`**

**`void lapic_send_ipi(uint32_t apic_id, uint32_t vector)`**

**`void lapic_send_init_sipi(uint32_t apic_id, paddr_t trampoline)`**

### `haven/arch/x86_64/memlayout.h`

the x86_64 virtual address space map.

*23 declarations, 10 carrying documentation.*

#### Kernel image window.  Must match arch/x86_64/boot/linker.ld.

**`KERNEL_VMA_BASE`** = `0xFFFFFFFF80000000ULL`

**`KERNEL_LMA_BASE`** = `0x0000000000200000ULL`

**`HHDM_BASE`** = `0xFFFF800000000000ULL`

Direct map of physical memory.

**`HHDM_SIZE`** = `0x0000400000000000ULL`

64 TiB

#### Kernel virtual allocation arena (non-contiguous physical backing).

**`KHEAP_BASE`** = `0xFFFFC00000000000ULL`

**`KHEAP_SIZE`** = `0x0000100000000000ULL`

16 TiB

**`MMIO_BASE`** = `0xFFFFD00000000000ULL`

Device / MMIO window.

**`MMIO_SIZE`** = `0x0000100000000000ULL`

16 TiB

#### Kernel stacks, each followed by an unmapped guard page.

**`KSTACK_BASE`** = `0xFFFFE00000000000ULL`

**`KSTACK_SIZE`** = `0x0000100000000000ULL`

**`KSTACK_PAGES`** = `4`

16 KiB per thread

**`KSTACK_BYTES`** = `(KSTACK_PAGES * HAVEN_PAGE_SIZE)`

**`USER_MIN`** = `0x0000000000001000ULL`

User address space.

**`USER_MAX`** = `0x0000800000000000ULL`

**`USER_STACK_TOP`** = `0x00007FFFFFFFF000ULL`

**`USER_STACK_PAGES`** = `32`

128 KiB

**`PAGE_SIZE_4K`** = `0x1000ULL`

Paging geometry.

**`PAGE_SIZE_2M`** = `0x200000ULL`

**`PAGE_SIZE_1G`** = `0x40000000ULL`

**`PT_ENTRIES`** = `512`

**`static inline bool is_canonical(uint64_t addr)`**

An address is canonical when bits 63:47 are all copies of bit 47. The CPU faults on a non-canonical access, so checking here turns a user-supplied garbage pointer into -EFAULT instead of a #GP.

**`static inline bool is_user_addr(uint64_t addr)`**

**`static inline bool is_kernel_addr(uint64_t addr)`**

### `haven/arch/x86_64/paging.h`

x86_64-private page-table facts.

*1 declarations, 1 carrying documentation.*

**`paddr_t arch_pt_root(const struct arch_pagetable *pt)`**

The physical address of an address space's PML4, for loading into CR3 from assembly entry paths and for the SMP trampoline.

### `haven/arch/x86_64/percpu.h`

x86_64-private per-CPU bring-up.

*3 declarations, 1 carrying documentation.*

#### Initialise CPU `cpu_id`'s block.  Does not install it.

**`void percpu_init_block(uint32_t cpu_id, uint32_t hw_id, bool is_bsp)`**

**`void percpu_install(uint32_t cpu_id)`**

Point this CPU's GS base at block `cpu_id`.  After this, this_cpu() answers from the register rather than from the fallback block.

#### Record how many CPUs the firmware tables describe.

**`void percpu_set_count(uint32_t n)`**

### `haven/arch/x86_64/percpu_asm.h`

byte offsets into the per-CPU block, for assembly.

*4 declarations, 2 carrying documentation.*

#### GS base points at struct percpu_anchor: { self, block }.

**`PERCPU_ANCHOR_SELF`** = `0`

**`PERCPU_ANCHOR_BLOCK`** = `8`

**`PERCPU_OFF_KSTACK`** = `24`

Offsets within struct haven_cpu.

**`PERCPU_OFF_USER_RSP`** = `32`

user_rsp

### `haven/arch/x86_64/regs.h`

the x86_64 trap frame.

*11 declarations, 6 carrying documentation.*

**`struct trapframe`**

```c
struct trapframe {
	/* Pushed by the stub, in reverse order, so r15 is at the lowest
	   address — which is where RSP points when the handler is called. */
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

	/* Pushed by the stub: which vector we are in, and the error code.
	   Vectors that the CPU does not give an error code for get a zero
	   pushed in its place, so every frame has the same shape and the
	   common handler needs no special cases.  HavenDOS did the same
	   thing and it was the right call. */
	uint64_t vector;
	uint64_t error_code;

	/* Pushed by the CPU.  rsp and ss are always present in long mode,
	   even for a same-privilege interrupt — unlike 32-bit protected
	   mode, where they appeared only on a privilege change.  That
	   removes a whole class of frame-layout confusion. */
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
};
```

**`HAVEN_STATIC_ASSERT(sizeof(struct trapframe) == 22 * 8, "trapframe layout must match arch/x86_64/cpu/entry.S")`**

**`static inline bool trapframe_from_user(const struct trapframe *tf)`**

Did this trap come from user mode?  The low two bits of CS are the privilege level the interrupted code was running at.  This is the test that decides whether the entry path must SWAPGS, and getting it wrong either leaks a kernel pointer to user space or makes the kernel treat user-controlled memory as its per-CPU block.

#### x86 page-fault error-code bits, in CR2's companion word.

**`PF_PRESENT`** = `(1u << 0)`

fault was a protection violation, not a missing page

**`PF_WRITE`** = `(1u << 1)`

the access was a write

**`PF_USER`** = `(1u << 2)`

the access came from ring 3

**`PF_RESERVED`** = `(1u << 3)`

a reserved page-table bit was set

**`PF_FETCH`** = `(1u << 4)`

the access was an instruction fetch

**`PF_PROTKEY`** = `(1u << 5)`

**`PF_SHADOW`** = `(1u << 6)`

**`PF_SGX`** = `(1u << 15)`
