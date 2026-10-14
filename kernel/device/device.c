/* SPDX-License-Identifier: MIT
 *
 * device.c — device registration and driver matching.
 */
#include <haven/device.h>
#include <haven/mm.h>
#include <haven/console.h>
#include <haven/errno.h>
#include <haven/string.h>
#include <haven/spinlock.h>

/* Collected by the linker script from every HAVEN_DRIVER(). */
extern const struct driver *const __haven_drivers_start[];
extern const struct driver *const __haven_drivers_end[];

static LIST_HEAD(devices);
static DEFINE_SPINLOCK(device_lock, "devices");
static unsigned device_total;
static unsigned device_claimed;

const char *device_bus_name(enum device_bus bus)
{
	switch (bus) {
	case BUS_PLATFORM: return "platform";
	case BUS_PCI:      return "pci";
	case BUS_VIRTIO:   return "virtio";
	default:           return "?";
	}
}

const char *device_class_name(enum device_class cls)
{
	switch (cls) {
	case DEVCLASS_BLOCK:         return "block";
	case DEVCLASS_CHAR:          return "char";
	case DEVCLASS_NET:           return "net";
	case DEVCLASS_DISPLAY:       return "display";
	case DEVCLASS_INPUT:         return "input";
	case DEVCLASS_BRIDGE:        return "bridge";
	case DEVCLASS_STORAGE_CTRL:  return "storage-controller";
	case DEVCLASS_OTHER:
	default:                     return "other";
	}
}

int device_register(struct device *dev)
{
	if (!dev)
		return -HAVEN_EINVAL;

	irqflags_t f = spin_lock(&device_lock);
	list_init(&dev->node);
	list_add_tail(&dev->node, &devices);
	device_total++;
	spin_unlock(&device_lock, f);

	/* Offer the device to every driver registered for its bus.  The
	   match callbacks run with no lock held, because a probe may
	   allocate, map MMIO or sleep -- none of which is legal under a
	   spinlock. */
	size_t ndrivers = (size_t)(__haven_drivers_end - __haven_drivers_start);

	for (size_t i = 0; i < ndrivers; i++) {
		const struct driver *drv = __haven_drivers_start[i];
		if (!drv || drv->bus != dev->bus || !drv->match || !drv->probe)
			continue;
		if (!drv->match(dev))
			continue;

		int rc = drv->probe(dev);
		if (rc == 0) {
			dev->driver  = (struct driver *)drv;
			dev->claimed = true;

			f = spin_lock(&device_lock);
			device_claimed++;
			spin_unlock(&device_lock, f);

			kinfo("device: %s claimed by driver '%s'",
			      dev->name, drv->name);
			return 0;
		}

		/* A driver that matched but failed to probe is worth
		   reporting: it means the hardware is present and
		   recognised, which is quite different from absent. */
		kwarn("device: driver '%s' matched %s but failed to probe: %s",
		      drv->name, dev->name, haven_strerror(rc));
	}

	/* Not a failure.  An unclaimed device is hardware HavenOS does not
	   support yet, and naming it is how somebody learns what to write.
	   HavenDOS's hardware list showed recognised controllers with no
	   indication of whether a driver existed -- which its own
	   documentation calls out as the NVMe confusion: "Recognizing a
	   controller in a hardware list does not mean it has a working
	   storage driver." */
	kinfo("device: %s (%s, %s) has no driver", dev->name,
	      device_bus_name(dev->bus), device_class_name(dev->cls));
	return -HAVEN_ENODEV;
}

struct device *device_find_by_class(enum device_class cls, unsigned index)
{
	irqflags_t f = spin_lock(&device_lock);

	unsigned seen = 0;
	struct device *dev;
	list_for_each_entry(dev, &devices, node) {
		if (dev->cls != cls)
			continue;
		if (seen++ != index)
			continue;
		spin_unlock(&device_lock, f);
		return dev;
	}

	spin_unlock(&device_lock, f);
	return NULL;
}

unsigned device_count(void)        { return device_total; }
unsigned device_count_claimed(void) { return device_claimed; }

void device_dump(void)
{
	size_t ndrivers = (size_t)(__haven_drivers_end - __haven_drivers_start);

	kinfo("device: %u devices, %u claimed, %zu drivers linked in",
	      device_total, device_claimed, ndrivers);

	irqflags_t f = spin_lock(&device_lock);
	struct device *dev;
	list_for_each_entry(dev, &devices, node) {
		if (dev->bus == BUS_PCI) {
			kinfo("device:   %02x:%02x.%x %-10s %04x:%04x %-18s %s",
			      dev->pci_bus, dev->pci_slot, dev->pci_func,
			      device_class_name(dev->cls),
			      dev->id.pci.vendor, dev->id.pci.device,
			      dev->name,
			      dev->claimed ? dev->driver->name : "(no driver)");
		} else {
			kinfo("device:   %-8s %-10s %-18s %s",
			      device_bus_name(dev->bus),
			      device_class_name(dev->cls), dev->name,
			      dev->claimed ? dev->driver->name : "(no driver)");
		}
	}
	spin_unlock(&device_lock, f);
}

void device_init(void)
{
	list_init(&devices);
	spin_lock_init(&device_lock, "devices");

	size_t ndrivers = (size_t)(__haven_drivers_end - __haven_drivers_start);
	kinfo("device: %zu drivers registered at link time", ndrivers);

	for (size_t i = 0; i < ndrivers; i++) {
		const struct driver *drv = __haven_drivers_start[i];
		if (drv)
			kdebug("device:   driver '%s' on the %s bus",
			       drv->name, device_bus_name(drv->bus));
	}
}
