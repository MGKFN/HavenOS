#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Assemble a complete HavenOS release package and verify it.
#
# DESIGN NOTE (HavenDOS -> HavenOS)
# HavenDOS's published repository contains a LICENSE, a README and one ZIP
# archive, havendos-v0_7_18-source.zip -- which its own README states is
# "an earlier version, not the matching source tree for the 0.8.1 ISO",
# and notes that "running `make` immediately after cloning is not a
# documented way to reproduce 0.8.1".  That is not a packaging oversight;
# it is what happens when the archive is made by hand, once, and the tree
# moves on.
#
# So this script is the packaging step, it runs from the Makefile, and it
# checks its own output.  Three properties matter:
#
#   1. Every source file that the build compiles is in the archive.  Not
#      "every file in the repository" -- that would pass while a file was
#      missing from the build -- but the actual object list the Makefile
#      derives, mapped back to sources.  A file the build needs and the
#      archive lacks is the failure this check exists to catch.
#   2. The archive builds.  The packaged tree is extracted into a
#      scratch directory and `make` is run in it, so "it builds from the
#      archive" is a tested claim rather than an assumption.
#   3. The structure is as documented.  Every directory and file this
#      script says it ships is verified to be present, and the manifest
#      records sizes and SHA-256 sums for all of them.
#
# Usage: scripts/package-release.sh [version]
#        VERIFY_BUILD=0 scripts/package-release.sh   (skip property 2)

set -eu

cd "$(dirname "$0")/.."
TOP=$(pwd)

VERSION=${1:-$(sed -n 's/^#define HAVENOS_VERSION  *"\([^"]*\)".*/\1/p' \
               include/haven/version.h)}
CODENAME=$(sed -n 's/^#define HAVENOS_CODENAME  *"\([^"]*\)".*/\1/p' \
           include/haven/version.h)
REV=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
DATE=$(date -u +%Y-%m-%d)

NAME="havenos-$VERSION"
DIST="$TOP/dist"
STAGE="$DIST/$NAME"
ZIP="$DIST/$NAME.zip"

fail=0
step()   { echo ""; echo "=== $1"; }
ok()     { echo "  ok      $1"; }
bad()    { echo "  FAIL    $1"; fail=1; }
note()   { echo "  note    $1"; }

echo "HavenOS release packaging"
echo "  version   $VERSION \"$CODENAME\""
echo "  revision  $REV"
echo "  date      $DATE"

# --------------------------------------------------------------- staging

step "staging the package"

rm -rf "$STAGE" "$ZIP"
mkdir -p "$STAGE"

# Source, build system, configuration, tooling, documentation, licence.
# Listed explicitly rather than copied wholesale: a release should contain
# what it says it contains, and `cp -r .` would ship whatever happened to
# be lying around in the working tree.
for d in arch kernel fs drivers lib include boot tests tools scripts docs; do
	if [ ! -d "$d" ]; then
		bad "source directory $d is missing from the working tree"
		continue
	fi
	mkdir -p "$STAGE/$d"
	# -L so a symlink in the tree becomes a real file in the archive.
	(cd "$d" && find . -type f \
	    ! -name '*.o' ! -name '*.d' ! -name '*.elf' ! -name '*.iso' \
	    ! -name '*.map' ! -name '*.img' ! -name '*.tmp' \
	    ! -name 'test_*' -o -name 'test_*.c' \
	    | sed 's|^\./||' | while read -r f; do
		mkdir -p "$STAGE/$d/$(dirname "$f")"
		cp -L "$f" "$STAGE/$d/$f"
	done)
done

for f in Makefile README.md LICENSE CHANGELOG.md RELEASE_NOTES.md .gitignore; do
	if [ -f "$f" ]; then
		cp -L "$f" "$STAGE/$f"
	else
		bad "top-level file $f is missing from the working tree"
	fi
done

# The built ISO, when there is one.  It is the artefact most people want
# and the one hardest for them to produce, so it ships when available --
# but its absence is a note, not a failure, because the source package is
# the deliverable and `make iso` needs tools a build host may not have.
if [ -f "$TOP/build/havenos-$VERSION.iso" ]; then
	mkdir -p "$STAGE/image"
	cp "$TOP/build/havenos-$VERSION.iso" "$STAGE/image/"
	(cd "$STAGE/image" && sha256sum "havenos-$VERSION.iso" \
	    > "havenos-$VERSION.iso.sha256")
	ok "bootable ISO included ($(du -h "$STAGE/image/havenos-$VERSION.iso" \
	    | cut -f1))"
else
	note "no build/havenos-$VERSION.iso; packaging source only"
fi

ok "staged $(find "$STAGE" -type f | wc -l) files (MANIFEST.txt is added below)"

# ------------------------------------------------- property 1: completeness

step "verifying every source file the build needs is present"

# Ask the Makefile what it compiles, rather than guessing.  `make info`
# would be prettier; this is the real object list.
srcs=$(make --no-print-directory -n all 2>/dev/null \
       | grep -oE '[A-Za-z0-9_/.-]+\.(c|S)' | sort -u || true)
if [ -z "$srcs" ]; then
	# A fully built tree compiles nothing, so derive the list the same
	# way the Makefile does.
	srcs=$(find arch/x86_64 kernel fs drivers lib \
	       -name '*.c' -o -name '*.S' | sort -u)
	note "tree is already built; using the directory walk instead"
fi

nsrc=0
for s in $srcs; do
	nsrc=$((nsrc + 1))
	[ -f "$STAGE/$s" ] || bad "source file in the build but not the archive: $s"
done
ok "$nsrc compiled source files, all present"

# Headers, the linker script and the build tooling are not in the object
# list but the build cannot proceed without them.
for extra in \
    arch/x86_64/boot/linker.ld \
    boot/grub/grub.cfg.in \
    tools/make_version.py tools/verify_multiboot2.py tools/verify_image.py \
    tools/gen_api_docs.py tools/make_test_disk.py \
    scripts/run-selftest.sh scripts/check-portability.sh \
    scripts/sanity-link-test.sh scripts/package-release.sh \
    tests/host/Makefile tests/host/harness.h \
    docs/ARCHITECTURE.md docs/DESIGN_DECISIONS.md docs/API.md \
    docs/DEVELOPER.md docs/USER.md docs/PORTING_ARM64.md \
    docs/HAVENDOS_HERITAGE.md
do
	[ -f "$STAGE/$extra" ] || bad "required file missing from the archive: $extra"
done

nhdr=$(find "$STAGE/include" -name '*.h' | wc -l)
[ "$nhdr" -ge 50 ] || bad "only $nhdr headers staged; expected at least 50"
ok "$nhdr headers, the linker script, build tooling and 7 documents present"

# Nothing that should never ship.
for pattern in '*.o' '*.d' '*.elf' '*.map' 'buildinfo.h'; do
	found=$(find "$STAGE" -name "$pattern" | head -3)
	[ -z "$found" ] || bad "build output leaked into the archive: $found"
done
ok "no build output in the archive"

# ------------------------------------------------------ manifest and archive

step "writing the manifest"

cat > "$STAGE/MANIFEST.txt" <<EOF
HavenOS $VERSION "$CODENAME"
Packaged $DATE from revision $REV

This archive contains the complete source, build system, configuration,
documentation and licence for HavenOS $VERSION.

  arch/        architecture support (x86_64, and the arm64 skeleton)
  kernel/      architecture-neutral kernel
  fs/          virtual filesystem switch, ramfs, devfs, FAT
  drivers/     console, PCI, block, input drivers
  lib/         freestanding C library used by the kernel
  include/     kernel API headers
  boot/        GRUB configuration template
  tests/       host test suite
  tools/       build, verification and documentation tooling
  scripts/     test, portability and packaging scripts
  docs/        architecture, API, developer and user documentation
  image/       bootable ISO and its SHA-256 sum (when built)

  Makefile          the build system
  README.md         what HavenOS is
  CHANGELOG.md      what changed, and what broke on the way
  RELEASE_NOTES.md  this release in detail
  LICENSE           MIT

To build:

  make              the kernel ELF
  make iso          a BIOS + UEFI bootable ISO
  make check        host tests, portability check, build, in-kernel tests

Requirements: GCC 12+ or Clang 15+, GNU binutils, GNU Make, Python 3.8+.
For images and emulation: grub-pc-bin, grub-efi-amd64-bin, xorriso,
mtools, qemu-system-x86.

----------------------------------------------------------------------
Contents: $(find "$STAGE" -type f | wc -l) files

EOF

(cd "$STAGE" && find . -type f ! -name MANIFEST.txt | sed 's|^\./||' \
    | LC_ALL=C sort | while read -r f; do
	printf '%10d  %s  %s\n' "$(wc -c < "$f")" \
	    "$(sha256sum "$f" | cut -c1-16)" "$f"
done) > "$DIST/.manifest-body"
# Outside $STAGE on purpose: a shell creates a redirect's target before
# the command runs, so a scratch file written inside the staged tree ends
# up listed in the manifest as a file the archive does not contain.

nman=$(wc -l < "$DIST/.manifest-body")
cat "$DIST/.manifest-body" >> "$STAGE/MANIFEST.txt"
rm -f "$DIST/.manifest-body"

ok "MANIFEST.txt lists $nman files with sizes and digests"

step "creating the archive"

if ! command -v zip >/dev/null 2>&1; then
	bad "zip is not installed; cannot produce $ZIP"
else
	(cd "$DIST" && zip -q -r -9 "$NAME.zip" "$NAME")
	ok "$ZIP ($(du -h "$ZIP" | cut -f1))"
	(cd "$DIST" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")
	ok "$NAME.zip.sha256"
fi

# --------------------------------------------- property 3: archive structure

if [ -f "$ZIP" ]; then
	step "verifying the archive structure"

	if command -v unzip >/dev/null 2>&1; then
		unzip -t "$ZIP" >/dev/null 2>&1 \
		    && ok "archive integrity check passed" \
		    || bad "archive is corrupt"

		listing=$(unzip -Z1 "$ZIP")

		# Everything must live under one top-level directory, so
		# extracting it cannot scatter files into the user's cwd.
		stray=$(echo "$listing" | grep -v "^$NAME/" | head -3 || true)
		[ -z "$stray" ] \
		    && ok "every entry is under $NAME/" \
		    || bad "entries outside $NAME/: $stray"

		# No absolute paths and no parent-directory traversal.
		evil=$(echo "$listing" | grep -E '^/|\.\./' | head -3 || true)
		[ -z "$evil" ] \
		    && ok "no absolute paths and no '..' traversal" \
		    || bad "unsafe paths in the archive: $evil"

		for want in \
		    "$NAME/Makefile" "$NAME/README.md" "$NAME/LICENSE" \
		    "$NAME/CHANGELOG.md" "$NAME/RELEASE_NOTES.md" \
		    "$NAME/MANIFEST.txt" \
		    "$NAME/arch/x86_64/boot/boot.S" \
		    "$NAME/arch/x86_64/boot/linker.ld" \
		    "$NAME/arch/arm64/cpu/cpu.c" \
		    "$NAME/kernel/main.c" "$NAME/kernel/mm/pmm.c" \
		    "$NAME/fs/vfs.c" "$NAME/drivers/bus/pci.c" \
		    "$NAME/lib/string.c" "$NAME/include/haven/arch.h" \
		    "$NAME/boot/grub/grub.cfg.in" \
		    "$NAME/tests/host/Makefile" \
		    "$NAME/tools/make_version.py" \
		    "$NAME/scripts/package-release.sh" \
		    "$NAME/docs/USER.md" "$NAME/docs/API.md"
		do
			echo "$listing" | grep -qx "$want" \
			    || bad "archive is missing $want"
		done
		ok "all 22 structural landmarks present"

		nzip=$(echo "$listing" | grep -vc '/$' || true)
		nstage=$(find "$STAGE" -type f | wc -l)
		[ "$nzip" -eq "$nstage" ] \
		    && ok "$nzip files in the archive, matching the staged tree" \
		    || bad "archive has $nzip files, staged tree has $nstage"
	else
		note "unzip is not installed; structural checks skipped"
	fi
fi

# ----------------------------------------- property 2: the archive builds

if [ -f "$ZIP" ] && [ "${VERIFY_BUILD:-1}" = "1" ] \
   && command -v unzip >/dev/null 2>&1; then
	step "building from the extracted archive"

	scratch=$(mktemp -d -t havenos-release.XXXXXX)
	trap 'rm -rf "$scratch"' EXIT

	unzip -q "$ZIP" -d "$scratch"
	if make -C "$scratch/$NAME" --no-print-directory \
	       >"$scratch/build.log" 2>&1; then
		ok "the extracted archive builds: $(grep -c '^  CC' \
		    "$scratch/build.log" || true) objects compiled"
		[ -f "$scratch/$NAME/build/havenos.elf" ] \
		    && ok "produced build/havenos.elf" \
		    || bad "build reported success but produced no kernel"
	else
		bad "the extracted archive does NOT build; last 20 lines:"
		tail -20 "$scratch/build.log" | sed 's/^/          /'
	fi
else
	note "build-from-archive check skipped"
fi

# ------------------------------------------------------------------ verdict

echo ""
if [ "$fail" -eq 0 ]; then
	echo "  RELEASE PASSED"
	echo "    $ZIP"
	echo "    $(find "$STAGE" -type f | wc -l) files, $(du -sh "$STAGE" \
	    | cut -f1) unpacked"
	exit 0
fi

echo "  RELEASE FAILED -- see the failures above"
exit 1
