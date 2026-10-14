/* SPDX-License-Identifier: MIT
 *
 * acpi.h — just enough ACPI to find the other CPUs.
 *
 * Not an ACPI implementation: no AML interpreter, no device enumeration,
 * no power management.  HavenOS needs one thing from ACPI -- the list of
 * local APIC identifiers in the MADT, which is the only portable way to
 * learn how many processors a machine has.
 */
#ifndef _HAVEN_ARCH_X86_64_ACPI_H
#define _HAVEN_ARCH_X86_64_ACPI_H

#include <haven/types.h>

#define ACPI_MAX_CPUS    64
#define ACPI_MAX_IOAPICS 4

struct boot_info;

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

const struct acpi_info *acpi_init(const struct boot_info *bi);
const struct acpi_info *acpi_get_info(void);

#endif /* _HAVEN_ARCH_X86_64_ACPI_H */
