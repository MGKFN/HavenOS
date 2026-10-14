/* SPDX-License-Identifier: MIT
 *
 * virtio.h — VirtIO block devices.
 *
 * HavenDOS added a VirtIO block driver specifically because UTM SE
 * presents one and its ATA path reads a floating bus there; its own
 * source calls VirtIO "primary disk on UTM SE (QEMU TCG)".  HavenOS keeps
 * the capability for the same reason: a VirtIO disk is what a modern
 * hypervisor offers, and it is faster than emulated IDE.
 */
#ifndef _HAVEN_VIRTIO_H
#define _HAVEN_VIRTIO_H

#include <haven/types.h>

void     virtio_blk_init(void);
unsigned virtio_blk_count(void);

#endif /* _HAVEN_VIRTIO_H */
