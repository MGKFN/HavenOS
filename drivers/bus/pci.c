/* SPDX-License-Identifier: MIT
 *
 * pci.c — PCI bus enumeration.
 *
 * DESIGN NOTE (HavenDOS → HavenOS)
 * HavenDOS's drivers/pci.c scanned the bus and printed what it found, and
 * its hardware-information screen listed those devices.  Its own
 * documentation then has to explain, twice, that the list means less than
 * it looks like it means: "Recognizing a controller in a hardware list
 * does not mean it has a working storage driver", written because the
 * tested HP laptop showed its NVMe controller and could not install to it.
 *
 * HavenOS enumerates into the device framework instead, so the question
 * "is this device supported" has a real answer: a device is either
 * claimed by a named driver or reported as having none.  The hardware
 * list and the driver list are the same list.
 *
 * RECURSIVE ENUMERATION, NOT A FLAT 0-255 SCAN
 * A flat scan of all 256 buses costs 256 * 32 * 8 configuration reads --
 * 65536 I/O port pairs -- and on some chipsets probing a bus that does
 * not exist behind any bridge is not merely slow but returns garbage.
 * Following the bridges visits only buses that are really there.
 */
#include <haven/device.h>
#include <haven/mm.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/printf.h>
#include <haven/arch/x86_64/io.h>
#include <haven/pci.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* Configuration-space register offsets. */
#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_STATUS         0x06
#define PCI_REVISION       0x08
#define PCI_PROG_IF        0x09
#define PCI_SUBCLASS       0x0A
#define PCI_CLASS_CODE     0x0B
#define PCI_HEADER_TYPE    0x0E
#define PCI_BAR0           0x10
#define PCI_SECONDARY_BUS  0x19
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_INTERRUPT_PIN  0x3D

#define PCI_HEADER_MULTIFUNCTION 0x80
#define PCI_HEADER_TYPE_MASK     0x7F
#define PCI_HEADER_BRIDGE        0x01

#define PCI_CLASS_BRIDGE   0x06
#define PCI_SUBCLASS_PCI_BRIDGE 0x04

#define PCI_COMMAND_IO        0x0001
#define PCI_COMMAND_MEMORY    0x0002
#define PCI_COMMAND_BUS_MASTER 0x0004

#define PCI_VENDOR_INVALID 0xFFFF

static unsigned devices_found;
static unsigned buses_scanned;

/*
 * Configuration-space access, mechanism 1 (the port pair).
 *
 * The address register encodes bus, slot, function and a dword-aligned
 * offset.  Bit 31 enables the cycle.  Reads must be dword-aligned, so a
 * byte or word read extracts from the containing dword -- doing a
 * narrower I/O access to 0xCFC directly works on most chipsets and
 * returns garbage on some, which is a nasty class of machine-specific bug
 * to chase.
 */
static uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                                  uint8_t offset)
{
	uint32_t address = (1u << 31)
	                 | ((uint32_t)bus  << 16)
	                 | ((uint32_t)(slot & 0x1F) << 11)
	                 | ((uint32_t)(func & 0x07) << 8)
	                 | (offset & 0xFC);

	outl(PCI_CONFIG_ADDRESS, address);
	return inl(PCI_CONFIG_DATA);
}

static void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t offset, uint32_t value)
{
	uint32_t address = (1u << 31)
	                 | ((uint32_t)bus  << 16)
	                 | ((uint32_t)(slot & 0x1F) << 11)
	                 | ((uint32_t)(func & 0x07) << 8)
	                 | (offset & 0xFC);

	outl(PCI_CONFIG_ADDRESS, address);
	outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset)
{
	uint32_t dword = pci_config_read32(bus, slot, func, offset);
	return (uint16_t)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func,
                         uint8_t offset)
{
	uint32_t dword = pci_config_read32(bus, slot, func, offset);
	return (uint8_t)((dword >> ((offset & 3) * 8)) & 0xFF);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset, uint16_t value)
{
	uint32_t dword = pci_config_read32(bus, slot, func, offset);
	unsigned shift = (offset & 2) * 8;
	dword &= ~(0xFFFFu << shift);
	dword |= (uint32_t)value << shift;
	pci_config_write32(bus, slot, func, offset & 0xFC, dword);
}

uint32_t pci_read_bar(const struct device *dev, unsigned index)
{
	if (index > 5)
		return 0;
	return pci_config_read32(dev->pci_bus, dev->pci_slot, dev->pci_func,
	                         (uint8_t)(PCI_BAR0 + index * 4));
}

void pci_enable_device(struct device *dev, bool bus_master)
{
	uint16_t cmd = pci_config_read16(dev->pci_bus, dev->pci_slot,
	                                 dev->pci_func, PCI_COMMAND);

	/* Firmware does not always leave the decoders enabled.  A driver
	   that writes to a BAR without this sees its writes dropped and
	   reads return 0xFF -- which looks exactly like absent hardware. */
	cmd |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
	if (bus_master)
		cmd |= PCI_COMMAND_BUS_MASTER;

	pci_config_write16(dev->pci_bus, dev->pci_slot, dev->pci_func,
	                   PCI_COMMAND, cmd);
}

static enum device_class class_from_pci(uint8_t class_code, uint8_t subclass)
{
	switch (class_code) {
	case 0x01: return DEVCLASS_STORAGE_CTRL;
	case 0x02: return DEVCLASS_NET;
	case 0x03: return DEVCLASS_DISPLAY;
	case 0x06: return subclass == PCI_SUBCLASS_PCI_BRIDGE
	                ? DEVCLASS_BRIDGE : DEVCLASS_OTHER;
	case 0x09: return DEVCLASS_INPUT;
	case 0x0C: return DEVCLASS_OTHER;   /* serial bus: USB, SMBus */
	default:   return DEVCLASS_OTHER;
	}
}

/* A short human name for the common controllers, so the boot log is
   readable without a PCI ID database. */
static const char *known_name(uint16_t vendor, uint16_t device,
                              uint8_t class_code, uint8_t subclass)
{
	if (vendor == 0x1AF4) {
		/* VirtIO.  HavenDOS supported virtio-blk because UTM SE
		   presents it, and so does HavenOS. */
		switch (device) {
		case 0x1000: case 0x1041: return "virtio-net";
		case 0x1001: case 0x1042: return "virtio-blk";
		case 0x1002: case 0x1045: return "virtio-balloon";
		case 0x1003: case 0x1043: return "virtio-console";
		case 0x1009: case 0x1049: return "virtio-9p";
		default:                  return "virtio-device";
		}
	}
	if (vendor == 0x8086) {
		switch (device) {
		case 0x100E: case 0x10D3: return "e1000";
		case 0x7010: case 0x7111: return "piix-ide";
		case 0x2922: case 0x2829: return "ich-ahci";
		case 0x1237: case 0x29C0: return "i440fx-host";
		case 0x7000: case 0x2918: return "piix-isa";
		case 0x7113:              return "piix-acpi";
		default:                  break;
		}
	}
	if (vendor == 0x10EC && device == 0x8139)
		return "rtl8139";
	if (vendor == 0x1234 && device == 0x1111)
		return "bochs-vga";
	if (vendor == 0x1B36 && device == 0x0010)
		return "nvme";

	switch (class_code) {
	case 0x01:
		switch (subclass) {
		case 0x01: return "ide-controller";
		case 0x06: return "ahci-controller";
		case 0x08: return "nvme-controller";
		default:   return "storage-controller";
		}
	case 0x02: return "network-controller";
	case 0x03: return "display-controller";
	case 0x06: return "bridge";
	case 0x0C:
		switch (subclass) {
		case 0x03: return "usb-controller";
		default:   return "serial-bus";
		}
	default: return "pci-device";
	}
}

static void scan_bus(uint8_t bus);

static void examine_function(uint8_t bus, uint8_t slot, uint8_t func)
{
	uint16_t vendor = pci_config_read16(bus, slot, func, PCI_VENDOR_ID);
	if (vendor == PCI_VENDOR_INVALID || vendor == 0x0000)
		return;

	uint16_t device_id = pci_config_read16(bus, slot, func, PCI_DEVICE_ID);
	uint8_t  class_code = pci_config_read8(bus, slot, func, PCI_CLASS_CODE);
	uint8_t  subclass   = pci_config_read8(bus, slot, func, PCI_SUBCLASS);
	uint8_t  prog_if    = pci_config_read8(bus, slot, func, PCI_PROG_IF);
	uint8_t  header     = pci_config_read8(bus, slot, func,
	                                       PCI_HEADER_TYPE);
	uint8_t  irq        = pci_config_read8(bus, slot, func,
	                                       PCI_INTERRUPT_LINE);

	struct device *dev = kzalloc(sizeof(*dev));
	if (!dev) {
		kerr("pci: out of memory enumerating %02x:%02x.%x",
		     bus, slot, func);
		return;
	}

	const char *nm = known_name(vendor, device_id, class_code, subclass);
	snprintf(dev->name, sizeof(dev->name), "%s@%02x:%02x.%x",
	         nm, bus, slot, func);

	dev->bus              = BUS_PCI;
	dev->cls              = class_from_pci(class_code, subclass);
	dev->id.bus           = BUS_PCI;
	dev->id.pci.vendor    = vendor;
	dev->id.pci.device    = device_id;
	dev->id.pci.class_code = class_code;
	dev->id.pci.subclass  = subclass;
	dev->id.pci.prog_if   = prog_if;
	dev->pci_bus          = bus;
	dev->pci_slot         = slot;
	dev->pci_func         = func;

	/* An IRQ line of 0xFF means "not routed".  Recording it as line 255
	   would have a driver register a handler on a vector nothing uses. */
	dev->irq_line = (irq == 0xFF || irq == 0) ? 0 : irq;

	/* Decode BAR0 enough for a driver to find its registers.  The full
	   BAR sizing dance (write all ones, read back the mask) is left to
	   the driver that needs it, because doing it here would disturb a
	   device another driver may own. */
	uint32_t bar0 = pci_config_read32(bus, slot, func, PCI_BAR0);
	if (bar0 & 1) {
		dev->io_base = (uint16_t)(bar0 & ~0x3u);
	} else {
		dev->mmio_base = bar0 & ~0xFu;
	}

	devices_found++;

	/* device_register() returns -ENODEV when nothing claims the device.
	   That is reported there, not an error here. */
	(void)device_register(dev);

	/* Follow a PCI-to-PCI bridge to the bus behind it. */
	if (class_code == PCI_CLASS_BRIDGE &&
	    subclass == PCI_SUBCLASS_PCI_BRIDGE &&
	    (header & PCI_HEADER_TYPE_MASK) == PCI_HEADER_BRIDGE) {
		uint8_t secondary = pci_config_read8(bus, slot, func,
		                                     PCI_SECONDARY_BUS);
		/* A bridge whose secondary bus is its own bus would recurse
		   forever.  Firmware does not normally produce that, but a
		   misconfigured or emulated bridge can. */
		if (secondary != bus && secondary != 0)
			scan_bus(secondary);
	}
}

static bool bus_visited[256];

static void scan_bus(uint8_t bus)
{
	if (bus_visited[bus])
		return;
	bus_visited[bus] = true;
	buses_scanned++;

	for (uint8_t slot = 0; slot < 32; slot++) {
		uint16_t vendor = pci_config_read16(bus, slot, 0,
		                                    PCI_VENDOR_ID);
		if (vendor == PCI_VENDOR_INVALID)
			continue;

		examine_function(bus, slot, 0);

		/* Functions 1-7 exist only on a multifunction device.
		   Probing them unconditionally works on most hardware and
		   returns aliased copies of function 0 on some, which would
		   register the same device eight times. */
		uint8_t header = pci_config_read8(bus, slot, 0,
		                                  PCI_HEADER_TYPE);
		if (!(header & PCI_HEADER_MULTIFUNCTION))
			continue;

		for (uint8_t func = 1; func < 8; func++)
			examine_function(bus, slot, func);
	}
}

/* Is a PCI bus present at all?  On a machine with no PCI, the
   configuration ports read back 0xFFFFFFFF, and a host bridge at 00:00.0
   is the universal sign that the mechanism works. */
static bool pci_present(void)
{
	outl(PCI_CONFIG_ADDRESS, 0x80000000);
	if (inl(PCI_CONFIG_ADDRESS) != 0x80000000)
		return false;

	uint16_t vendor = pci_config_read16(0, 0, 0, PCI_VENDOR_ID);
	return vendor != PCI_VENDOR_INVALID && vendor != 0x0000;
}

void pci_init(void)
{
	memset(bus_visited, 0, sizeof(bus_visited));
	devices_found = 0;
	buses_scanned = 0;

	if (!pci_present()) {
		kwarn("pci: no PCI host bridge found; only platform devices "
		      "will be available");
		return;
	}

	/* Start at bus 0 and follow the bridges.  A multifunction host
	   bridge indicates additional host controllers, each rooting its
	   own bus. */
	uint8_t header = pci_config_read8(0, 0, 0, PCI_HEADER_TYPE);
	if (header & PCI_HEADER_MULTIFUNCTION) {
		for (uint8_t func = 0; func < 8; func++) {
			if (pci_config_read16(0, 0, func, PCI_VENDOR_ID) ==
			    PCI_VENDOR_INVALID)
				continue;
			scan_bus(func);
		}
	} else {
		scan_bus(0);
	}

	kinfo("pci: %u devices on %u buses", devices_found, buses_scanned);
}
