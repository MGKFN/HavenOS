#!/usr/bin/env python3
"""Check the structural invariants of a linked HavenOS kernel image.

These are properties that a successful link does not guarantee but a
successful boot depends on.  Each one here has a specific failure mode
that is painful to diagnose from a dead machine:

  * the image must be loaded below 4 GiB, because the 32-bit trampoline
    in boot.S addresses it with 32-bit instructions;
  * the kernel link address must be in the top 2 GiB, because the code is
    compiled with -mcmodel=kernel and a rel32 reference cannot reach
    further;
  * the bounds symbols the PMM reserves from must exist and be ordered,
    since a reversed or missing pair would reserve nothing and hand the
    allocator the kernel's own memory — the exact bug HavenDOS hit twice;
  * .bss must not be in the file, or the ISO grows by the size of every
    static buffer;
  * no section may overlap another.
"""
import struct
import subprocess
import sys

KERNEL_WINDOW = 0xFFFFFFFF80000000


def nm_symbols(path):
    out = {}
    try:
        res = subprocess.run(["nm", "-n", path], capture_output=True,
                             text=True, check=True, timeout=60)
    except (subprocess.SubprocessError, OSError) as exc:
        raise SystemExit(f"cannot run nm on {path}: {exc}")
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            out[parts[2]] = int(parts[0], 16)
    return out


def sections(data):
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    strtab_off, = struct.unpack_from(
        "<Q", data, e_shoff + e_shstrndx * e_shentsize + 0x18)

    out = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags = struct.unpack_from("<IIQ", data, base)
        sh_addr, sh_offset, sh_size = struct.unpack_from("<QQQ", data,
                                                         base + 0x10)
        end = data.index(b"\0", strtab_off + sh_name)
        name = data[strtab_off + sh_name:end].decode()
        out.append({"name": name, "type": sh_type, "flags": sh_flags,
                    "addr": sh_addr, "offset": sh_offset, "size": sh_size})
    return out


def segments(data):
    e_phoff, = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
    out = []
    for i in range(e_phnum):
        base = e_phoff + i * e_phentsize
        p_type, = struct.unpack_from("<I", data, base)
        if p_type != 1:
            continue
        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from(
            "<QQQQQ", data, base + 0x08)
        out.append({"offset": p_offset, "vaddr": p_vaddr, "paddr": p_paddr,
                    "filesz": p_filesz, "memsz": p_memsz})
    return out


def main(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit(f"{path}: not a 64-bit ELF")

    errors, notes = [], []
    syms = nm_symbols(path)
    segs = segments(path and data)

    if not segs:
        errors.append("no PT_LOAD segment")

    for s in segs:
        if s["paddr"] >= 1 << 32:
            errors.append(
                f"PT_LOAD physical address 0x{s['paddr']:x} is above 4 GiB; "
                "the 32-bit boot trampoline cannot reach it")
        if s["vaddr"] < KERNEL_WINDOW:
            errors.append(
                f"PT_LOAD virtual address 0x{s['vaddr']:x} is below the "
                f"-mcmodel=kernel window 0x{KERNEL_WINDOW:x}")
        notes.append(
            f"PT_LOAD virt 0x{s['vaddr']:x} phys 0x{s['paddr']:x} "
            f"file {s['filesz']} mem {s['memsz']}")

    # Bounds symbols the PMM and the kernel-protection code rely on.
    pairs = [
        ("__kernel_start", "__kernel_end"),
        ("__kernel_phys_start", "__kernel_phys_end"),
        ("__text_start", "__text_end"),
        ("__rodata_start", "__rodata_end"),
        ("__data_start", "__data_end"),
        ("__bss_start", "__bss_end"),
    ]
    for lo, hi in pairs:
        if lo not in syms or hi not in syms:
            errors.append(f"missing bounds symbol(s): {lo} / {hi}")
            continue
        if syms[hi] <= syms[lo]:
            errors.append(
                f"{lo}=0x{syms[lo]:x} is not below {hi}=0x{syms[hi]:x}; "
                "a reversed pair makes the PMM reserve nothing")

    if "__kernel_phys_start" in syms and "__kernel_phys_end" in syms:
        size = syms["__kernel_phys_end"] - syms["__kernel_phys_start"]
        notes.append(f"image spans {size} bytes "
                     f"({size // 1024} KiB) of physical memory")
        if syms["__kernel_phys_start"] != 0x200000:
            errors.append(
                f"image loads at 0x{syms['__kernel_phys_start']:x}, "
                "expected 0x200000")

    # Driver/test/initcall registration tables must exist (possibly empty).
    for lo, hi in [("__haven_drivers_start", "__haven_drivers_end"),
                   ("__haven_tests_start", "__haven_tests_end"),
                   ("__haven_initcalls_start", "__haven_initcalls_end")]:
        if lo not in syms or hi not in syms:
            errors.append(f"missing registration table symbols {lo}/{hi}")
        elif syms[hi] < syms[lo]:
            errors.append(f"registration table {lo}..{hi} is reversed")

    # .bss must occupy no file space.
    for sec in sections(data):
        if sec["name"] == ".bss":
            if sec["type"] != 8:  # SHT_NOBITS
                errors.append(
                    f".bss has type {sec['type']}, expected SHT_NOBITS(8); "
                    f"it would add {sec['size']} bytes to the image on disk")
            notes.append(f".bss is {sec['size']} bytes, not stored in the file")

    # Allocated sections must not overlap in virtual memory.
    alloc = sorted((s for s in sections(data)
                    if s["flags"] & 0x2 and s["size"]),
                   key=lambda s: s["addr"])
    for a, b in zip(alloc, alloc[1:]):
        if a["addr"] + a["size"] > b["addr"]:
            errors.append(
                f"sections {a['name']} and {b['name']} overlap at "
                f"0x{b['addr']:x}")

    print(f"image: {path}")
    for n in notes:
        print(f"  note  {n}")
    for e in errors:
        print(f"  FAIL  {e}")
    if errors:
        return 1
    print(f"  ok    image layout is valid ({len(alloc)} allocated sections)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_image.py <kernel.elf>")
    sys.exit(main(sys.argv[1]))
