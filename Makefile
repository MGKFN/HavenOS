# SPDX-License-Identifier: MIT
#
# HavenOS build system.
#
# DESIGN NOTE (HavenDOS -> HavenOS)
# HavenDOS's Makefile listed every source file by hand in one SRCS_C
# variable and needed a documented two-pass build, because the installer
# had to embed a copy of the kernel ELF inside the kernel.  Adding a file
# meant editing the list; forgetting to meant a link error at best.
#
# HavenOS discovers sources by directory, so adding a file to kernel/ or
# drivers/ is all that is required.  The arch directory is selected by one
# variable, which is what makes `make ARCH=arm64` a real target rather
# than a fork of the build.
#
#   make              build the kernel ELF
#   make iso          bootable BIOS+UEFI ISO
#   make run          boot the ISO in QEMU (BIOS)
#   make run-uefi     boot the ISO in QEMU (OVMF/UEFI)
#   make test         host unit tests
#   make check        tests + portability + static checks
#   make release      full release package and ZIP archive
#   make clean        remove build output

# ---------------------------------------------------------------- config

ARCH        ?= x86_64
BUILD       ?= build
V           ?= 0

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
NM      := $(CROSS_COMPILE)nm
PYTHON  ?= python3

VERSION      := $(shell sed -n 's/^#define HAVENOS_VERSION  *"\([^"]*\)".*/\1/p' include/haven/version.h)
CODENAME     := $(shell sed -n 's/^#define HAVENOS_CODENAME  *"\([^"]*\)".*/\1/p' include/haven/version.h)
BUILD_DATE   := $(shell date -u +%Y-%m-%d)
BUILD_REV    := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)

ifeq ($(V),1)
Q :=
else
Q := @
endif

# ------------------------------------------------------------- toolflags

# -ffreestanding        no hosted libc; we provide our own in lib/
# -mno-red-zone         mandatory in kernel mode: an interrupt landing in
#                       the 128-byte red zone would corrupt the frame
# -mcmodel=kernel       generate code for the top 2 GiB, matching the
#                       higher-half link address
# -mno-sse/-mno-mmx     the kernel itself uses no vector registers, so no
#                       FPU state has to be saved on every entry
# -fno-omit-frame-pointer  makes panic()'s backtrace work without DWARF
# -fno-stack-protector  the guard needs TLS, which does not exist this early
# -fno-pic/-no-pie      a kernel is loaded at a fixed address
WARNFLAGS := -Wall -Wextra -Werror \
             -Wno-unused-parameter \
             -Wundef -Wshadow -Wpointer-arith -Wcast-align \
             -Wwrite-strings -Wredundant-decls \
             -Wstrict-prototypes -Wmissing-prototypes \
             -Wnested-externs -Winline -Wvla \
             -Wframe-larger-than=2048

CFLAGS_COMMON := -std=gnu11 -O2 -g \
                 -ffreestanding -fno-builtin -nostdinc \
                 -fno-stack-protector -fno-strict-aliasing \
                 -fno-omit-frame-pointer \
                 -fno-pic -fno-pie \
                 -fno-asynchronous-unwind-tables -fno-unwind-tables \
                 -Iinclude -isystem $(shell $(CC) -print-file-name=include) \
                 $(WARNFLAGS)

CFLAGS_x86_64 := -m64 -mno-red-zone -mcmodel=kernel \
                 -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
                 -mgeneral-regs-only
LDFLAGS_x86_64 := -m elf_x86_64 -z max-page-size=0x1000 -z noexecstack

CFLAGS_arm64  := -mgeneral-regs-only -mstrict-align
LDFLAGS_arm64 := -z max-page-size=0x1000 -z noexecstack

# The build stamp (date, git revision, architecture) is a GENERATED HEADER,
# not a -D on the command line.
#
# It was a -D first, and that was a bug with a visible symptom: a macro
# value is not recorded in the .d dependency files, so changing it does not
# rebuild anything.  Objects compiled at one commit kept its revision while
# objects recompiled later carried the new one, and one kernel image ended
# up reporting two different revisions -- `uname` said one thing and
# /etc/os-release, written by a different translation unit, said another.
#
# As a header the stamp is an ordinary prerequisite: -MMD records it, so a
# new revision rebuilds every object that reads the version.  The recipe
# rewrites the file only when its contents actually change, so an ordinary
# incremental build does not relink the world on every invocation.
BUILDINFO := $(BUILD)/include/haven/buildinfo.h

CFLAGS  := $(CFLAGS_COMMON) $(CFLAGS_$(ARCH)) -I$(BUILD)/include
ASFLAGS := $(CFLAGS_$(ARCH)) -fno-pic -fno-pie -D__ASSEMBLY__ -Iinclude
LDFLAGS := -nostdlib -n --build-id=none $(LDFLAGS_$(ARCH))

LINKER_SCRIPT := arch/$(ARCH)/boot/linker.ld

# --------------------------------------------------------------- sources

# Portable kernel, library, filesystems and drivers, plus exactly one
# architecture directory.  Nothing here names a file individually.
SRC_DIRS := kernel kernel/mm kernel/sched kernel/sync kernel/irq kernel/time \
            kernel/proc kernel/syscall kernel/device kernel/test \
            kernel/shell \
            lib fs fs/fat \
            drivers/char drivers/video drivers/bus drivers/block drivers/input

ARCH_DIRS := $(shell find arch/$(ARCH) -type d 2>/dev/null)

C_SRCS := $(foreach d,$(SRC_DIRS) $(ARCH_DIRS),$(wildcard $(d)/*.c))
S_SRCS := $(foreach d,$(ARCH_DIRS),$(wildcard $(d)/*.S))

OBJS := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o) $(S_SRCS:.S=.o))
DEPS := $(OBJS:.o=.d)

KERNEL  := $(BUILD)/havenos.elf
KERNEL_MAP := $(BUILD)/havenos.map
ISO     := $(BUILD)/havenos-$(VERSION).iso

# ----------------------------------------------------------------- rules

.PHONY: all
all: $(KERNEL)

.PHONY: FORCE
FORCE:

$(BUILDINFO): FORCE
	$(Q)mkdir -p $(dir $@)
	$(Q)printf '%s\n' \
	    '/* Generated by the Makefile.  Do not edit, do not commit. */' \
	    '#ifndef _HAVEN_BUILDINFO_H' \
	    '#define _HAVEN_BUILDINFO_H' \
	    '#define HAVENOS_BUILD_DATE "$(BUILD_DATE)"' \
	    '#define HAVENOS_BUILD_REV  "$(BUILD_REV)"' \
	    '#define HAVENOS_ARCH       "$(ARCH)"' \
	    '#endif' > $@.tmp
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else \
	    echo "  STAMP   $(VERSION) rev $(BUILD_REV) ($(ARCH))"; \
	    mv $@.tmp $@; fi

$(BUILD)/%.o: %.c $(BUILDINFO)
	$(Q)mkdir -p $(dir $@)
	$(Q)echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	$(Q)mkdir -p $(dir $@)
	$(Q)echo "  AS      $<"
	$(Q)$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(KERNEL): $(OBJS) $(LINKER_SCRIPT)
	$(Q)mkdir -p $(dir $@)
	$(Q)echo "  LD      $@"
	$(Q)$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -Map=$(KERNEL_MAP) -o $@ $(OBJS)
	$(Q)echo "  VERIFY  multiboot2 header"
	$(Q)$(PYTHON) tools/verify_multiboot2.py $@
	$(Q)echo "  VERIFY  image layout"
	$(Q)$(PYTHON) tools/verify_image.py $@
	$(Q)echo ""
	$(Q)echo "  HavenOS $(VERSION) \"$(CODENAME)\" ($(ARCH), rev $(BUILD_REV))"
	$(Q)size -A $@ | awk '/^\.(boot|text|rodata|data|bss)/ {printf "    %-10s %8d bytes\n", $$1, $$2}'
	$(Q)echo "    total      $$(stat -c%s $@) bytes on disk"
	$(Q)echo ""

# ------------------------------------------------------------------- ISO
#
# One ISO carries both firmware paths, which is the arrangement HavenDOS
# proved on real UEFI hardware: grub-mkrescue writes an El Torito BIOS
# boot image and an EFI System Partition containing bootx64.efi, so the
# same medium boots a 2009 desktop and a modern laptop.

GRUB_CFG := $(BUILD)/isodir/boot/grub/grub.cfg

.PHONY: iso
iso: $(ISO)

$(ISO): $(KERNEL) boot/grub/grub.cfg.in
	$(Q)echo "  GEN     grub.cfg"
	$(Q)mkdir -p $(BUILD)/isodir/boot/grub
	$(Q)$(PYTHON) tools/make_version.py include/haven/version.h \
	       boot/grub/grub.cfg.in $(GRUB_CFG) $(BUILD)/isodir/version.txt
	$(Q)cp $(KERNEL) $(BUILD)/isodir/boot/havenos.elf
	$(Q)echo "  ISO     $@"
	$(Q)grub-mkrescue --compress=xz -o $@ $(BUILD)/isodir 2>/dev/null \
	  || grub-mkrescue -o $@ $(BUILD)/isodir
	$(Q)echo "  ISO     $$(du -h $@ | cut -f1)  $@"

# ------------------------------------------------------------------ QEMU

QEMU        ?= qemu-system-x86_64
CPUS        ?= 1
MEM         ?= 512
QEMU_COMMON := -m $(MEM) -smp $(CPUS) -cdrom $(ISO) -boot d \
               -serial stdio -no-reboot -no-shutdown
OVMF        ?= /usr/share/ovmf/OVMF.fd

.PHONY: run run-uefi run-smp run-nographic
run: $(ISO)
	$(QEMU) $(QEMU_COMMON) -display none

run-graphic: $(ISO)
	$(QEMU) $(QEMU_COMMON)

run-uefi: $(ISO)
	$(QEMU) $(QEMU_COMMON) -bios $(OVMF) -display none

run-smp: $(ISO)
	$(MAKE) run CPUS=4

# Boot straight into the self-test menu entry and report its verdict.
# Entry 3 in boot/grub/grub.cfg.in is "run kernel self-tests".
.PHONY: run-selftest selftest
run-selftest:
	$(Q)rm -f $(ISO)
	$(Q)GRUB_DEFAULT=3 $(MAKE) --no-print-directory iso
	$(QEMU) $(QEMU_COMMON) -display none

# Build the self-test image, boot it, and fail if any check failed.
# This is the target CI runs: it exercises the real boot path on a real
# CPU, which no host test can do.
selftest: $(KERNEL)
	$(Q)./scripts/run-selftest.sh

# ------------------------------------------------------------------ test

.PHONY: test
test:
	$(Q)$(MAKE) -C tests/host --no-print-directory run

.PHONY: check
check: test portability
	$(Q)$(MAKE) --no-print-directory all
	$(Q)./scripts/run-selftest.sh

.PHONY: portability
portability:
	$(Q)./scripts/check-portability.sh

# --------------------------------------------------------------- release

.PHONY: release
release: check iso
	$(Q)./scripts/package-release.sh $(VERSION)

.PHONY: docs
docs:
	$(Q)$(PYTHON) tools/gen_api_docs.py include/haven docs/API.md

# ----------------------------------------------------------------- misc

.PHONY: clean distclean
clean:
	$(Q)rm -rf $(BUILD)
	$(Q)$(MAKE) -C tests/host --no-print-directory clean

distclean: clean
	$(Q)rm -rf dist

.PHONY: info
info:
	@echo "HavenOS $(VERSION) \"$(CODENAME)\""
	@echo "  arch      : $(ARCH)"
	@echo "  compiler  : $(shell $(CC) --version | head -1)"
	@echo "  sources   : $(words $(C_SRCS)) C, $(words $(S_SRCS)) assembly"
	@echo "  objects   : $(words $(OBJS))"

-include $(DEPS)
