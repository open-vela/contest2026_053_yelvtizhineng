/****************************************************************************
 * sd_card.h — SD card driver interface
 *
 * SDIO 1-bit interface for ESP32-S3 (per board schematic):
 *   CMD = IO0, CLK = IO43, D0 = IO44
 * Block device: /dev/mmcsd1 (mmcsd_slotinitialize(1) in board bringup)
 * Mount point:  /mnt/sd (vfat)
 ****************************************************************************/

#ifndef __SD_CARD_H
#define __SD_CARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SD_MOUNTPOINT  "/mnt/sd"
#define SD_DEVICE      "/dev/mmcsd1"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Boot-time auto-mount. Non-fatal: prints a warning and returns -1 when
 * the card is absent or has no FAT filesystem; the app keeps running. */

int  sd_card_init(void);

/* Mount the SD card. Tolerates EBUSY (already mounted). Returns 0 on
 * success, -1 on failure (errno distinguishes the cause:
 * ENODEV/ENOENT = no card / no block device, ENOTBLK/ENOTSUP = no FAT). */

int  sd_card_mount(void);

int  sd_card_umount(void);

/* Returns 1 if /mnt/sd is currently mounted, 0 otherwise. */

int  sd_card_status(void);

/* Mount (if needed) + write + read + verify round-trip test. */

int  sd_card_test(void);

#endif /* __SD_CARD_H */
