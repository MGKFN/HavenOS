/* SPDX-License-Identifier: MIT
 *
 * acpi.c — just enough ACPI to find the other CPUs.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS parsed no firmware tables: with one CPU and no power
 * management there was nothing in them it needed.  HavenOS needs exactly
 * one thing from ACPI -- the list of local APIC identifiers in the MADT,
 * because that is the only portable way to learn how many processors the
 * machine has and what to address them by.
 *
 * This is deliberately NOT an ACPI implementation.  There is no AML
 * interpreter, no device enumeration, no power management.  Those are
 * large subsystems whose absence costs HavenOS nothing today, and
 * pretending to support ACPI by parsing a few tables would be worse than
 * being clear about the scope.
 *
 * EVERY FIELD IS UNTRUSTED
 * Firmware tables are produced by firmware, and firmware has bugs.  A
 * table with a wrong length, a bad checksum or an entry that runs past
 * the end of the table is something real machines ship.  So the length is
 * bounded, the checksum is verified, and the entry walk cannot run off
 * the end or loop -- the same discipline as the Multiboot2 parser, for
 * the same reason.
 */
#include <haven/types.h>
#include <haven/boot.h>
#include <haven/arch.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/arch/x86_64/acpi.h>

struct HAVEN_PACKED acpi_rsdp {
	char     signature[8];      /* "RSD PTR " */
	uint8_t  checksum;          /* of the first 20 bytes */
	char     oem_id[6];
	uint8_t  revision;          /* 0 = ACPI 1.0, 2+ = 2.0 and later */
	uint32_t rsdt_address;
	/* ACPI 2.0 and later only; present when revision >= 2. */
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t  extended_checksum; /* of the whole structure */
	uint8_t  reserved[3];
};

struct HAVEN_PACKED acpi_header {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oem_id[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
};

struct HAVEN_PACKED acpi_madt {
	struct acpi_header header;
	uint32_t local_apic_address;
	uint32_t flags;
	uint8_t  entries[];
};

#define MADT_TYPE_LOCAL_APIC      0
#define MADT_TYPE_IO_APIC         1
#define MADT_TYPE_INT_OVERRIDE    2
#define MADT_TYPE_LOCAL_APIC_NMI  4
#define MADT_TYPE_LAPIC_ADDR_OVR  5
#define MADT_TYPE_LOCAL_X2APIC    9

struct HAVEN_PACKED madt_local_apic {
	uint8_t type;
	uint8_t length;
	uint8_t acpi_processor_id;
	uint8_t apic_id;
	uint32_t flags;
};

struct HAVEN_PACKED madt_local_x2apic {
	uint8_t  type;
	uint8_t  length;
	uint16_t reserved;
	uint32_t x2apic_id;
	uint32_t flags;
	uint32_t acpi_processor_uid;
};

struct HAVEN_PACKED madt_io_apic {
	uint8_t  type;
	uint8_t  length;
	uint8_t  io_apic_id;
	uint8_t  reserved;
	uint32_t address;
	uint32_t gsi_base;
};

#define MADT_LAPIC_ENABLED        0x1
#define MADT_LAPIC_ONLINE_CAPABLE 0x2

static struct acpi_info info;

static bool checksum_ok(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint8_t sum = 0;

	/* An ACPI table is valid when all its bytes sum to zero modulo 256.
	   A table that fails this is one firmware got wrong, and acting on
	   its contents means acting on whatever happens to be in memory. */
	for (size_t i = 0; i < len; i++)
		sum = (uint8_t)(sum + p[i]);

	return sum == 0;
}

/*
 * Find the RSDP.
 *
 * Multiboot2 hands it over directly when the loader found it, which is
 * the reliable path.  Failing that, the specification says it is either
 * in the first kilobyte of the Extended BIOS Data Area or between
 * 0xE0000 and 0xFFFFF, aligned to 16 bytes.
 */
static const struct acpi_rsdp *find_rsdp(const struct boot_info *bi)
{
	if (bi && bi->rsdp) {
		const struct acpi_rsdp *r =
			(const struct acpi_rsdp *)arch_phys_to_virt(bi->rsdp);
		if (memcmp(r->signature, "RSD PTR ", 8) == 0 &&
		    checksum_ok(r, 20))
			return r;
		kwarn("acpi: the loader-supplied RSDP at 0x%llx is invalid; "
		      "searching memory instead",
		      (unsigned long long)bi->rsdp);
	}

	/* The EBDA segment pointer lives at physical 0x40E. */
	uint16_t ebda_seg = *(const uint16_t *)arch_phys_to_virt(0x40E);
	paddr_t  ebda     = (paddr_t)ebda_seg << 4;

	if (ebda >= 0x400 && ebda < 0xA0000) {
		for (paddr_t p = ebda; p < ebda + 1024; p += 16) {
			const struct acpi_rsdp *r =
			        (const struct acpi_rsdp *)arch_phys_to_virt(p);
			if (memcmp(r->signature, "RSD PTR ", 8) == 0 &&
			    checksum_ok(r, 20))
				return r;
		}
	}

	for (paddr_t p = 0xE0000; p < 0x100000; p += 16) {
		const struct acpi_rsdp *r =
		        (const struct acpi_rsdp *)arch_phys_to_virt(p);
		if (memcmp(r->signature, "RSD PTR ", 8) == 0 &&
		    checksum_ok(r, 20))
			return r;
	}

	return NULL;
}

static const struct acpi_header *find_table(const struct acpi_rsdp *rsdp,
                                            const char signature[4])
{
	/* Prefer the XSDT when the firmware provides one: its entries are
	   64-bit, so a table above 4 GiB is reachable.  Using the RSDT on
	   such a machine would truncate the address. */
	bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;

	paddr_t root_pa = use_xsdt ? rsdp->xsdt_address : rsdp->rsdt_address;
	if (!root_pa)
		return NULL;

	const struct acpi_header *root =
		(const struct acpi_header *)arch_phys_to_virt(root_pa);

	if (root->length < sizeof(*root) || root->length > (1u << 20)) {
		kerr("acpi: the %s has an implausible length of %u bytes",
		     use_xsdt ? "XSDT" : "RSDT", root->length);
		return NULL;
	}
	if (!checksum_ok(root, root->length)) {
		kerr("acpi: the %s checksum is wrong",
		     use_xsdt ? "XSDT" : "RSDT");
		return NULL;
	}

	size_t entry_size = use_xsdt ? 8 : 4;
	size_t count      = (root->length - sizeof(*root)) / entry_size;
	const uint8_t *entries = (const uint8_t *)root + sizeof(*root);

	for (size_t i = 0; i < count; i++) {
		paddr_t pa;
		if (use_xsdt) {
			/* Read byte-wise: the XSDT's 64-bit entries are only
			   4-byte aligned, so a uint64_t load is unaligned. */
			uint64_t v = 0;
			for (int b = 0; b < 8; b++)
				v |= (uint64_t)entries[i * 8 + b] << (b * 8);
			pa = v;
		} else {
			uint32_t v = 0;
			for (int b = 0; b < 4; b++)
				v |= (uint32_t)entries[i * 4 + b] << (b * 8);
			pa = v;
		}

		if (!pa)
			continue;

		const struct acpi_header *h =
			(const struct acpi_header *)arch_phys_to_virt(pa);

		if (memcmp(h->signature, signature, 4) != 0)
			continue;
		if (h->length < sizeof(*h) || h->length > (1u << 20))
			continue;
		if (!checksum_ok(h, h->length)) {
			kwarn("acpi: the %.4s table checksum is wrong; "
			      "ignoring it", signature);
			continue;
		}
		return h;
	}

	return NULL;
}

static void parse_madt(const struct acpi_madt *madt)
{
	info.lapic_address = madt->local_apic_address;

	size_t offset = offsetof(struct acpi_madt, entries);
	size_t length = madt->header.length;

	while (offset + 2 <= length) {
		const uint8_t *entry = (const uint8_t *)madt + offset;
		uint8_t type = entry[0];
		uint8_t len  = entry[1];

		/* A zero length would loop forever, and an over-long one
		   would read past the table.  Both occur in the wild. */
		if (len < 2) {
			kwarn("acpi: MADT entry at offset %zu has length %u; "
			      "stopping the walk", offset, len);
			break;
		}
		if (offset + len > length) {
			kwarn("acpi: MADT entry at offset %zu runs past the "
			      "table; stopping the walk", offset);
			break;
		}

		switch (type) {
		case MADT_TYPE_LOCAL_APIC: {
			const struct madt_local_apic *la =
				(const struct madt_local_apic *)entry;

			if (info.cpu_count >= ACPI_MAX_CPUS) {
				kwarn("acpi: more than %d processors reported; "
				      "the rest are ignored", ACPI_MAX_CPUS);
				break;
			}

			/* A processor that is neither enabled nor
			   online-capable is one firmware has disabled.
			   Sending it a startup IPI does nothing, and counting
			   it would make the kernel wait for a CPU that will
			   never arrive. */
			bool usable = (la->flags & MADT_LAPIC_ENABLED) ||
			              (la->flags & MADT_LAPIC_ONLINE_CAPABLE);
			if (!usable) {
				kdebug("acpi: processor %u (APIC %u) is "
				       "disabled by firmware",
				       la->acpi_processor_id, la->apic_id);
				break;
			}

			info.apic_ids[info.cpu_count++] = la->apic_id;
			break;
		}

		case MADT_TYPE_LOCAL_X2APIC: {
			const struct madt_local_x2apic *x2 =
				(const struct madt_local_x2apic *)entry;

			if (info.cpu_count >= ACPI_MAX_CPUS)
				break;
			if (!(x2->flags & MADT_LAPIC_ENABLED))
				break;
			/* An x2APIC id above 255 cannot be addressed through
			   the xAPIC interface this kernel uses, so counting
			   such a CPU would mean trying to start one we cannot
			   reach. */
			if (x2->x2apic_id > 255) {
				kwarn("acpi: processor with x2APIC id %u needs "
				      "x2APIC addressing, which this kernel "
				      "does not yet use; skipping it",
				      x2->x2apic_id);
				break;
			}
			info.apic_ids[info.cpu_count++] = (uint8_t)x2->x2apic_id;
			break;
		}

		case MADT_TYPE_IO_APIC: {
			const struct madt_io_apic *io =
				(const struct madt_io_apic *)entry;
			if (info.ioapic_count < ACPI_MAX_IOAPICS) {
				info.ioapic_address[info.ioapic_count] =
					io->address;
				info.ioapic_gsi_base[info.ioapic_count] =
					io->gsi_base;
				info.ioapic_count++;
			}
			break;
		}

		case MADT_TYPE_LAPIC_ADDR_OVR: {
			/* A 64-bit override of the local APIC address.  When
			   present it supersedes the 32-bit field, and
			   ignoring it on a machine that provides one means
			   using the wrong address. */
			uint64_t v = 0;
			for (int b = 0; b < 8 && (size_t)(12 + b) <= len; b++)
				v |= (uint64_t)entry[4 + b] << (b * 8);
			if (v)
				info.lapic_address = v;
			break;
		}

		default:
			break;
		}

		offset += len;
	}
}

const struct acpi_info *acpi_init(const struct boot_info *bi)
{
	memset(&info, 0, sizeof(info));

	const struct acpi_rsdp *rsdp = find_rsdp(bi);
	if (!rsdp) {
		kwarn("acpi: no RSDP found; HavenOS will run on the boot CPU "
		      "only");
		info.cpu_count   = 1;
		info.apic_ids[0] = 0;
		return &info;
	}

	char oem[7];
	memcpy(oem, rsdp->oem_id, 6);
	oem[6] = '\0';

	info.present  = true;
	info.revision = rsdp->revision;

	const struct acpi_header *madt = find_table(rsdp, "APIC");
	if (!madt) {
		kwarn("acpi: no MADT; assuming a single processor");
		info.cpu_count   = 1;
		info.apic_ids[0] = 0;
		return &info;
	}

	parse_madt((const struct acpi_madt *)madt);

	if (info.cpu_count == 0) {
		/* The MADT exists but lists no usable processor.  The machine
		   is clearly running on something, so report one rather than
		   zero -- a zero would make the scheduler's per-CPU setup
		   allocate nothing. */
		kwarn("acpi: the MADT lists no usable processor; assuming one");
		info.cpu_count   = 1;
		info.apic_ids[0] = 0;
	}

	kinfo("acpi: ACPI %u.0 from '%s', %u processor%s, %u I/O APIC%s",
	      rsdp->revision >= 2 ? 2u : 1u, oem,
	      info.cpu_count, info.cpu_count == 1 ? "" : "s",
	      info.ioapic_count, info.ioapic_count == 1 ? "" : "s");

	for (uint32_t i = 0; i < info.cpu_count; i++)
		kdebug("acpi:   processor %u has local APIC id %u",
		       i, info.apic_ids[i]);

	return &info;
}

const struct acpi_info *acpi_get_info(void)
{
	return &info;
}
