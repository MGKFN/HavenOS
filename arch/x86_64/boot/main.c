/* SPDX-License-Identifier: MIT
 *
 * main.c — the first C code HavenOS runs, and the last arch-specific step
 * before the generic kernel takes over.
 *
 * ORDERING RULES (and why they are written down)
 * HavenDOS's kernel_main() accumulated ordering bugs that its own source
 * comments catalogue: net_init() ran *after* the POST screen that reported
 * whether a NIC was found, so every boot printed "NIC .... Not detected"
 * regardless of the hardware; virtio_scan_drives() ran lazily, so the same
 * screen reported "VirtIO BLK .... Not detected" on machines whose disk
 * worked two lines later.  Neither was a driver bug — both were a boot
 * sequence nobody could see the shape of.
 *
 * So the sequence here is explicit, numbered, and each step states what it
 * depends on.  A step may only use facilities established above it.
 */
#include <haven/boot.h>
#include <haven/console.h>
#include <haven/panic.h>
#include <haven/arch.h>
#include <haven/string.h>
#include <haven/arch/x86_64/cpu.h>
#include <haven/fbcon.h>
#include <haven/multiboot2.h>
#include <haven/cmdline.h>
#include <haven/arch/x86_64/boot.h>
#include <haven/arch/x86_64/bootfb.h>

void x86_boot_main(uint32_t magic, uint32_t mbinfo_pa)
{
	/* ── 1. Serial first, before anything that can fail ──────────────
	   Nothing above this line can report a problem.  Everything below
	   it can, including on a machine with no working display, which is
	   the case HavenDOS could not debug. */
	serial_early_init();

	kinfo("HavenOS x86_64 kernel starting");
	kinfo("boot: long mode entered, higher half active at %p",
	      (void *)(vaddr_t)&x86_boot_main);

	/* ── 2. CPU features and hardware protections ────────────────────
	   Must precede paging work: whether NX and 1 GiB pages are usable
	   decides what the page tables may contain. */
	arch_early_init();

	/* ── 3. Firmware handover → struct boot_info ─────────────────────
	   Depends on: the direct map (established by boot.S) for reading
	   the info block, and the console for reporting a malformed one. */
	if (multiboot2_parse(magic, mbinfo_pa, &haven_boot_info) != 0)
		panic("boot: unusable firmware handover; cannot continue");

	kinfo("boot: loader '%s', firmware %s",
	      haven_boot_info.loader_name,
	      haven_boot_info.firmware == BOOT_FW_UEFI ? "UEFI" : "BIOS");
	if (haven_boot_info.cmdline[0])
		kinfo("boot: cmdline '%s'", haven_boot_info.cmdline);

	/* Options that change what gets logged have to be honoured before
	   the logging happens, which is why this is here and not in
	   haven_kmain(). */
	cmdline_apply_early(&haven_boot_info);

	/* ── 4. Display, if firmware gave us one ─────────────────────────
	   Depends on: only the boot_info framebuffer description.  It is
	   mapped through its own window (bootfb.c) rather than the direct
	   map, so it works wherever firmware put it.

	   This comes BEFORE timekeeping, and that order is the lesson of
	   the first boot on real hardware: everything below this line can
	   hang or fault on a machine that is not QEMU, and on a laptop the
	   screen is the only place the reason can be shown.  Registering
	   the console replays the log so far, so nothing above is lost. */
	if (cmdline_console_serial_only()) {
		kinfo("boot: display left untouched (console=serial)");
	} else if (haven_boot_info.framebuffer.format == BOOT_FB_NONE) {
		kwarn("boot: the loader passed no framebuffer; output is on the "
		      "serial port only");
	} else if (x86_bootfb_map(&haven_boot_info.framebuffer)) {
		fbcon_init(&haven_boot_info.framebuffer);
	}

	/* ── 5. Timekeeping ──────────────────────────────────────────────
	   Depends on: CPU features (invariant TSC).  Done early so the log
	   lines below carry a real timestamp, and so delays used by driver
	   bring-up are calibrated rather than guessed. */
	arch_time_init();

	/* ── 6. Hand over to the architecture-neutral kernel ─────────────
	   Everything from here on is portable code.  An ARM64 port
	   reaches this same call with its own boot_info and never touches
	   kernel/. */
	haven_kmain(&haven_boot_info);
}
