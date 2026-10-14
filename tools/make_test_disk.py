#!/usr/bin/env python3
"""Build a partitioned, FAT-formatted disk image for the self-test run.

The in-kernel filesystem tests are worth far more against a volume some
OTHER implementation produced: a driver tested only against filesystems
it wrote itself will happily agree with its own mistakes.  So the image
is partitioned here and formatted with mtools, and the tests read files
mtools wrote before writing any of their own.

usage: make_test_disk.py <output.img> [size-MiB]
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SECTOR = 512
PART_START_LBA = 2048          # the conventional 1 MiB alignment
MBR_TYPE_FAT16_LBA = 0x0E


def need(tool):
    if shutil.which(tool) is None:
        print(f"  SKIP    {tool} not installed; cannot build a test disk")
        return False
    return True


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)

    out = argv[1]
    size_mib = int(argv[2]) if len(argv) > 2 else 64

    if not need("mformat") or not need("mcopy"):
        return 2

    total_sectors = size_mib * 1024 * 1024 // SECTOR
    part_sectors = total_sectors - PART_START_LBA

    with open(out, "wb") as fh:
        fh.truncate(total_sectors * SECTOR)

    # Partition table. A real MBR, so the kernel's parser has something
    # genuine to read rather than a bare filesystem at offset zero.
    with open(out, "r+b") as fh:
        fh.seek(446)
        fh.write(struct.pack("<B3sB3sII",
                             0x80,                    # bootable
                             b"\x00\x02\x00",         # CHS first (legacy)
                             MBR_TYPE_FAT16_LBA,
                             b"\xfe\xff\xff",         # CHS last (legacy)
                             PART_START_LBA,
                             part_sectors))
        fh.seek(510)
        fh.write(b"\x55\xaa")

    # Format the partition separately and splice it in, because mtools
    # addresses a filesystem image rather than a partition within a disk.
    with tempfile.TemporaryDirectory() as tmp:
        part = os.path.join(tmp, "part.img")
        with open(part, "wb") as fh:
            fh.truncate(part_sectors * SECTOR)

        subprocess.run(["mformat", "-i", part, "-F", "-v", "HAVENTEST",
                        "-T", str(part_sectors), "::"],
                       check=True, capture_output=True)

        readme = os.path.join(tmp, "README.TXT")
        with open(readme, "w", encoding="ascii") as fh:
            fh.write("This volume was formatted and written by mtools,\r\n"
                     "not by HavenOS. The in-kernel self-tests read it to\r\n"
                     "prove the FAT driver agrees with another\r\n"
                     "implementation rather than only with itself.\r\n")

        payload = os.path.join(tmp, "DATA.BIN")
        with open(payload, "wb") as fh:
            # A multi-cluster file with a checkable pattern.
            fh.write(bytes((i * 31 + 7) & 0xFF for i in range(40 * 1024)))

        subprocess.run(["mcopy", "-i", part, readme, "::README.TXT"],
                       check=True, capture_output=True)
        subprocess.run(["mcopy", "-i", part, payload, "::DATA.BIN"],
                       check=True, capture_output=True)
        subprocess.run(["mmd", "-i", part, "::SUBDIR"],
                       check=True, capture_output=True)
        subprocess.run(["mcopy", "-i", part, readme, "::SUBDIR/NESTED.TXT"],
                       check=True, capture_output=True)

        with open(part, "rb") as src, open(out, "r+b") as dst:
            dst.seek(PART_START_LBA * SECTOR)
            shutil.copyfileobj(src, dst)

    print(f"  GEN     {out} ({size_mib} MiB, MBR + FAT, "
          f"README.TXT DATA.BIN SUBDIR/)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
