/* SPDX-License-Identifier: MIT
 *
 * version.h — the single source of truth for the HavenOS version.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS shipped a release whose interface still said "Beta" and whose
 * ETC/SYSCONF.TXT reported VERSION=0.6.1 while the kernel was 0.7.18,
 * because the version appeared as a literal in several places.  Its
 * v0.7.18 Makefile added a tools/make_version.py step specifically so
 * "version.txt and the GRUB menu titles are generated from
 * include/version.h so they cannot drift from the running kernel again".
 *
 * HavenOS starts from that conclusion: this header is the only place a
 * version number is written, and the ISO's GRUB menu, the release
 * artefacts and the kernel banner are all generated from it by
 * tools/make_version.py.
 */
#ifndef _HAVEN_VERSION_H
#define _HAVEN_VERSION_H

#define HAVENOS_VERSION_MAJOR 0
#define HAVENOS_VERSION_MINOR 1
#define HAVENOS_VERSION_PATCH 0

#define HAVENOS_VERSION   "0.1.0"
#define HAVENOS_CODENAME  "Foundation"
#define HAVENOS_COPYRIGHT "Copyright (c) 2026 KUNIX / Tech Haven Studios - MIT licensed"

/* Set by the build; see the Makefile's VERSION_FLAGS. */
#ifndef HAVENOS_BUILD_DATE
#define HAVENOS_BUILD_DATE "unknown"
#endif
#ifndef HAVENOS_BUILD_REV
#define HAVENOS_BUILD_REV "unknown"
#endif
#ifndef HAVENOS_ARCH
#define HAVENOS_ARCH "x86_64"
#endif

#endif /* _HAVEN_VERSION_H */
