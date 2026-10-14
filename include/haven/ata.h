/* SPDX-License-Identifier: MIT
 *
 * ata.h — ATA/SATA disks over programmed I/O.
 */
#ifndef _HAVEN_ATA_H
#define _HAVEN_ATA_H

#include <haven/types.h>

void     ata_init(void);
unsigned ata_drive_count(void);

#endif /* _HAVEN_ATA_H */
