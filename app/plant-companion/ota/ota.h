/****************************************************************************
 * apps/plant-companion/ota/ota.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version
 * 2.0 (the "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 *
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_OTA_OTA_H
#define __APPS_PLANT_COMPANION_OTA_OTA_H

#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* OTA server configuration */

#define OTA_SERVER_HOST           "192.168.3.12"
#define OTA_SERVER_PORT           8080
#define OTA_VERSION_PATH          "/version.json"
#define OTA_FIRMWARE_DIR          "/firmware/"

#define OTA_MAGIC                 0x504C414E  /* "PLAN" */
#define OTA_CURRENT_HARDWARE_ID   0x01        /* ESP32S3-BOX */

#define OTA_SLOT_SIZE             (2 * 1024 * 1024)  /* 2MB per slot */
#define OTA_HTTP_TIMEOUT_MS       15000

/* Firmware version - change these before building new firmware */

#ifndef OTA_FW_VERSION_MAJOR
#  define OTA_FW_VERSION_MAJOR  1
#endif
#ifndef OTA_FW_VERSION_MINOR
#  define OTA_FW_VERSION_MINOR  3
#endif
#ifndef OTA_FW_VERSION_PATCH
#  define OTA_FW_VERSION_PATCH  29
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* OTA error codes */

enum ota_error_e
{
  OTA_OK                  =  0,
  OTA_ERR_NETWORK         = -1,
  OTA_ERR_NO_UPDATE       = -2,
  OTA_ERR_SIZE_EXCEEDED   = -3,
  OTA_ERR_FLASH_WRITE     = -4,
  OTA_ERR_SHA256          = -5,
  OTA_ERR_HARDWARE_ID     = -6
};

/* Firmware header (64 bytes at start of each slot) */

struct ota_firmware_header_s
{
  uint32_t magic;           /* OTA_MAGIC = 0x504C414E */
  uint32_t hardware_id;     /* Target hardware */
  uint32_t version_major;
  uint32_t version_minor;
  uint32_t version_patch;
  uint32_t firmware_size;   /* Size excluding header */
  uint32_t reserved[2];
  uint8_t  sha256[32];      /* SHA256 of firmware payload */
};

/* OTA status (runtime) */

struct ota_status_s
{
  int      state;
  uint32_t downloaded;
  uint32_t total;
  int      last_error;
};

/* Version info from server */

struct ota_version_info_s
{
  uint32_t version_major;
  uint32_t version_minor;
  uint32_t version_patch;
  uint32_t size;
  uint32_t hardware_id;
  uint8_t  sha256[32];
  char     url[128];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ota_init
 *
 * Description:
 *   Initialize OTA subsystem. Read the active slot from otadata.
 *
 * Returned Value:
 *   0 on success, negative errno on failure.
 ****************************************************************************/

int ota_init(void);

/****************************************************************************
 * Name: ota_check_update
 *
 * Description:
 *   Query server for new firmware. If available, download to target slot,
 *   verify SHA256, write otadata (state=NEW) to switch the boot and
 *   reboot.  The bootloader boots the new slot as PENDING_VERIFY and
 *   rolls back automatically if it is not confirmed with ota_confirm().
 *
 * Returned Value:
 *   OTA_OK on success, negative ota_error_e on failure.
 ****************************************************************************/

int ota_check_update(void);

/****************************************************************************
 * Name: ota_check_version
 *
 * Description:
 *   Query the server for new firmware and validate it, WITHOUT
 *   downloading / switching / rebooting.  Used by the UI "检查更新"
 *   button (阶段 A).  When OTA_OK is returned, *info holds the new
 *   firmware version information.
 *
 * Returned Value:
 *   OTA_OK (new version available, info filled), OTA_ERR_NO_UPDATE
 *   (already up to date), or a negative ota_error_e on failure.
 ****************************************************************************/

int ota_check_version(FAR struct ota_version_info_s *info);

/****************************************************************************
 * Name: ota_is_pending_verify
 *
 * Description:
 *   Return true when the currently booted slot is PENDING_VERIFY, i.e.
 *   the bootloader just switched to it after an OTA and it still awaits
 *   confirmation (ota_confirm).  Used by the UI to show the post-upgrade
 *   status page after reboot (阶段 A).
 ****************************************************************************/

bool ota_is_pending_verify(void);

/****************************************************************************
 * Name: ota_read_prev_version
 *
 * Description:
 *   Read the previous firmware version saved to /mnt/sd/ota_prev.txt
 *   just before the slot switch (see ota_save_prev_version).  Used by
 *   the post-upgrade status page to show "上一版本".  Returns negative
 *   errno when the file is missing/corrupt (caller hides that line).
 ****************************************************************************/

int ota_read_prev_version(FAR int *major, FAR int *minor,
                          FAR int *patch);

/****************************************************************************
 * Name: ota_stage_update
 *
 * Description:
 *   Like ota_check_update(), but stops after the firmware is downloaded to
 *   the target slot and verified: no otadata write and no reboot.  Used
 *   to validate the download path without any risk.
 *
 * Returned Value:
 *   OTA_OK on success, negative ota_error_e on failure.
 ****************************************************************************/

int ota_stage_update(void);

/****************************************************************************
 * Name: ota_get_status
 *
 * Description:
 *   Get current OTA status.
 *
 * Returned Value:
 *   0 on success, negative errno on failure.
 ****************************************************************************/

int ota_get_status(FAR struct ota_status_s *status);

/****************************************************************************
 * Name: ota_get_current_version
 *
 * Description:
 *   Get current running firmware version (compile-time constants).
 ****************************************************************************/

void ota_get_current_version(FAR int *major, FAR int *minor,
                             FAR int *patch);

/****************************************************************************
 * Name: ota_print_boot_status
 *
 * Description:
 *   Print the otadata state (both copies: seq + state) and the derived
 *   active/target slots.  otadata is the single source of truth for the
 *   boot decision.
 ****************************************************************************/

void ota_print_boot_status(void);

/****************************************************************************
 * Name: ota_confirm
 *
 * Description:
 *   Mark the currently booted slot as VALID in otadata (mirror of
 *   esp_ota_mark_app_valid_cancel_rollback).  Call this after the new
 *   firmware passes self-test; otherwise the bootloader rolls back to
 *   the previous slot on the next reset.
 *
 * Returned Value:
 *   0 on success, negative errno on failure.
 ****************************************************************************/

int ota_confirm(void);

#endif /* __APPS_PLANT_COMPANION_OTA_OTA_H */
