/* SPDX-License-Identifier: MIT
 *
 * device.h — the modular driver framework.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS called every driver's init function by name from
 * kernel_main(), in a fixed sequence twenty lines long: pci_init(),
 * virtio_blk_init(), virtio_scan_drives(), ata_init(), fat16_init(),
 * fat12_init(), vfs_init(), net_init().  Two problems follow from that,
 * and its own source documents both:
 *
 *   - the order was invisible, so it drifted wrong.  net_init() ran AFTER
 *     the POST screen that reported whether a NIC was found, so every
 *     boot printed "NIC ..... Not detected" regardless of the hardware.
 *     virtio_scan_drives() ran lazily, so the same screen reported
 *     "VirtIO BLK .... Not detected" on machines whose disk worked two
 *     lines later.  Neither was a driver bug.
 *   - adding a driver meant editing kernel_main(), so a driver could not
 *     be compiled in or out without touching the kernel's entry point.
 *
 * HavenOS has drivers register themselves into a link-time table with
 * HAVEN_DRIVER(), declare which bus and which device identifiers they
 * match, and get probed by the bus that finds a matching device.  Adding
 * a driver is adding a file.  Nothing in kernel/main.c names a device.
 */
#ifndef _HAVEN_DEVICE_H
#define _HAVEN_DEVICE_H

#include <haven/types.h>
#include <haven/list.h>

#define DEVICE_NAME_MAX 48

enum device_bus {
	BUS_PLATFORM = 0,   /* fixed-address legacy hardware: PS/2, PIT, RTC */
	BUS_PCI,
	BUS_VIRTIO,
	BUS_COUNT
};

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

/* How a bus identifies a device to a driver.  A union rather than a
   per-bus struct, so one driver table covers every bus. */
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

/*
 * Register a driver into the link-time table the linker script collects
 * at __haven_drivers_start.  No central list to edit, which is the whole
 * point: a driver is added by adding a file.
 */
#define HAVEN_DRIVER(var) \
	static const struct driver *const __haven_driver_##var \
		HAVEN_USED HAVEN_SECTION(".haven.drivers") = &var

void device_init(void);

/* Register a device a bus has discovered, and offer it to every driver
   that matches.  Returns 0 if a driver claimed it, -HAVEN_ENODEV if
   none did -- which is information, not a failure: an unclaimed device
   is simply hardware HavenOS does not support yet, and reporting it is
   how someone learns what to write next. */
int device_register(struct device *dev);

struct device *device_find_by_class(enum device_class cls, unsigned index);
unsigned       device_count(void);
unsigned       device_count_claimed(void);
void           device_dump(void);

const char *device_bus_name(enum device_bus bus);
const char *device_class_name(enum device_class cls);

#endif /* _HAVEN_DEVICE_H */
