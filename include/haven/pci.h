/* SPDX-License-Identifier: MIT
 *
 * pci.h — PCI configuration access, for drivers on the PCI bus.
 */
#ifndef _HAVEN_PCI_H
#define _HAVEN_PCI_H

#include <haven/types.h>
#include <haven/device.h>

void pci_init(void);

uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func,
                          uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset, uint16_t value);

uint32_t pci_read_bar(const struct device *dev, unsigned index);

/* Enable a device's I/O and memory decoders, and optionally bus
   mastering.  Firmware does not always leave them on, and a driver that
   writes to a BAR without this sees its writes dropped and its reads
   return 0xFF -- indistinguishable from absent hardware. */
void     pci_enable_device(struct device *dev, bool bus_master);

/* Common vendor identifiers, for match functions. */
#define PCI_VENDOR_INTEL   0x8086
#define PCI_VENDOR_REDHAT  0x1AF4   /* VirtIO */
#define PCI_VENDOR_REALTEK 0x10EC
#define PCI_VENDOR_QEMU    0x1234

#endif /* _HAVEN_PCI_H */
