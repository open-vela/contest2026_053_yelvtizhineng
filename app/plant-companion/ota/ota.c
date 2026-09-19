/****************************************************************************
 * apps/plant-companion/ota/ota.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mtd/mtd.h>
#include <crypto/sha2.h>

#include "ota.h"

#ifdef CONFIG_PLANT_SD_CARD
#  include "../components/system_monitor/device_cfg.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define OTA_RECV_BUF_SIZE  1024
#define OTA_HEADER_MAX     512

/* Slot safety:
 *
 * The running firmware is executed directly from SPI flash (XIP).  In the
 * A/B deployment the second-stage bootloader boots one of the two slots,
 * so the running image lives inside either /dev/ota0 or /dev/ota1, and
 * erasing/writing that slot destroys the code currently executing and
 * hangs the system (observed).  The download target must therefore always
 * be the slot that is NOT currently booted.
 *
 * Which slot is booted is decided by the bootloader from the otadata
 * partition (highest valid ota_seq; slot = (seq - 1) % app_count).  The
 * active slot is read back at runtime to pick the safe target.
 *
 * In the legacy deployment (whole image flashed at 0x0, no bootloader)
 * the running image overlaps /dev/ota0, so /dev/ota1 is the safe target.
 */

#define OTA_SAFE_TARGET_SLOT  "/dev/ota1"

#define OTA_OTADATA_DEVPATH      "/dev/otadata"

/* otadata entry states - same values as esp_ota_img_states_t in esp-idf:
 *   NEW(0x00) -> bootloader rewrites to PENDING_VERIFY(0x01) and boots it
 *   VALID(0x02) = image confirmed working
 *   INVALID(0x03)/ABORTED(0x04) = excluded from selection (rollback target)
 */

#define OTA_OTA_STATE_NEW            0x00
#define OTA_OTA_STATE_PENDING_VERIFY 0x01
#define OTA_OTA_STATE_VALID          0x02
#define OTA_OTA_STATE_INVALID        0x03
#define OTA_OTA_STATE_ABORTED        0x04

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ota_ctx_s
{
  struct ota_status_s status;
  bool                initialized;
  FAR const char     *active_slot;   /* "/dev/ota0" or "/dev/ota1" */
  FAR const char     *target_slot;   /* opposite of active_slot */
};

/* esp_ota_select_entry_t layout (otadata partition, 2 copies @ 0 / 0x1000) */

struct ota_otadata_entry_s
{
  uint32_t ota_seq;
  uint8_t  seq_label[20];
  uint32_t ota_state;
  uint32_t crc;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ota_ctx_s g_ota_ctx;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static FAR const char *ota_get_target_slot(void);
static FAR const char *ota_get_active_slot(void);
static uint32_t ota_crc32(FAR const uint8_t *data, size_t len);
static bool ota_verify_sha256(FAR const char *devpath, size_t size,
                              FAR const uint8_t *expected);
static int  ota_otadata_read(FAR struct ota_otadata_entry_s *e0,
                             FAR struct ota_otadata_entry_s *e1);
static int  ota_write_otadata(FAR const char *target);
static int  ota_mark_current_valid(void);
static int  ota_http_get_json(FAR const char *host, int port,
                              FAR const char *path,
                              FAR char *buf, size_t buflen);
static int  ota_http_download(FAR const char *host, int port,
                              FAR const char *path,
                              FAR const char *devpath, uint32_t size);
static int  ota_parse_version_json(FAR const char *json,
                                   FAR struct ota_version_info_s *info);
static bool ota_is_newer_version(uint32_t major, uint32_t minor,
                                 uint32_t patch);
static int  ota_query_version(FAR struct ota_version_info_s *info);
static void ota_save_prev_version(void);

/****************************************************************************
 * Private Functions - Slot Management
 ****************************************************************************/

/* Return true when an otadata entry is bootable.  This mirrors the
 * bootloader (bootloader_common_ota_select_valid in esp-idf): the entry
 * must have a valid sequence, a matching CRC (over ota_seq only), and a
 * state that is not INVALID/ABORTED.  PENDING_VERIFY counts as bootable
 * because we are running that very image while it awaits confirmation.
 */

static bool ota_otadata_selectable(FAR const struct ota_otadata_entry_s *e)
{
  if (e->ota_seq == 0xFFFFFFFF)
    {
      return false;  /* erased */
    }

  if (e->ota_state == OTA_OTA_STATE_INVALID ||
      e->ota_state == OTA_OTA_STATE_ABORTED)
    {
      return false;
    }

  return e->crc == ota_crc32((FAR const uint8_t *)&e->ota_seq, 4);
}

/* Read both otadata copies and return the entry with the highest ota_seq
 * among the bootable ones (same selection the bootloader makes).  When
 * nothing is bootable, NULL is returned.  *idx receives the copy index
 * (0 or 1) of the selected entry, or -1 when there is none.
 */

static FAR struct ota_otadata_entry_s *
ota_otadata_active(FAR struct ota_otadata_entry_s *e0,
                   FAR struct ota_otadata_entry_s *e1, FAR int *idx)
{
  FAR struct ota_otadata_entry_s *sel = NULL;
  int ret;

  if (idx != NULL)
    {
      *idx = -1;
    }

  ret = ota_otadata_read(e0, e1);
  if (ret < 0)
    {
      return NULL;
    }

  if (ota_otadata_selectable(e0))
    {
      sel = e0;
      if (idx != NULL)
        {
          *idx = 0;
        }
    }

  if (ota_otadata_selectable(e1) &&
      (sel == NULL || e1->ota_seq > sel->ota_seq))
    {
      sel = e1;
      if (idx != NULL)
        {
          *idx = 1;
        }
    }

  return sel;
}

static FAR const char *ota_get_active_slot(void)
{
  struct ota_otadata_entry_s e0;
  struct ota_otadata_entry_s e1;
  FAR struct ota_otadata_entry_s *sel;

  sel = ota_otadata_active(&e0, &e1, NULL);
  if (sel == NULL)
    {
      return NULL;  /* otadata uninitialized (legacy deployment) */
    }

  /* slot = (seq - 1) % app_count; app_count = 2 (ota_0, ota_1) */

  return ((sel->ota_seq - 1) & 1) != 0 ? "/dev/ota1" : "/dev/ota0";
}

static FAR const char *ota_get_target_slot(void)
{
  FAR const char *active = ota_get_active_slot();

  if (active == NULL)
    {
      /* otadata uninitialized: legacy 0x0 deployment, ota1 is safe */

      return OTA_SAFE_TARGET_SLOT;
    }

  return strcmp(active, "/dev/ota0") == 0 ? "/dev/ota1" : "/dev/ota0";
}

/****************************************************************************
 * Private Functions - SHA256 Verification
 *
 * The slot is read back through the raw MTD interface (find_mtddriver),
 * bypassing the BCH/FTL character-device proxy so the verify is fast and
 * reads exactly the number of bytes that were downloaded.
 ****************************************************************************/

static bool ota_verify_sha256(FAR const char *devpath, size_t size,
                              FAR const uint8_t *expected)
{
  FAR struct inode *inode;
  FAR struct mtd_dev_s *mtd;
  SHA2_CTX ctx;
  uint8_t hash[SHA256_DIGEST_LENGTH];
  uint8_t buf[1024];
  size_t remaining = size;
  size_t offset = 0;
  int ret;

  ret = find_mtddriver(devpath, &inode);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[OTA] find_mtddriver %s failed: %d\n", devpath, ret);
      return false;
    }

  mtd = inode->u.i_mtd;
  sha256init(&ctx);

  while (remaining > 0)
    {
      size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;

      ret = MTD_READ(mtd, offset, chunk, buf);
      if (ret != (int)chunk)
        {
          syslog(LOG_ERR, "[OTA] verify read failed: %d\n", ret);
          close_mtddriver(inode);
          return false;
        }

      sha256update(&ctx, buf, chunk);
      offset += chunk;
      remaining -= chunk;
    }

  close_mtddriver(inode);
  sha256final(hash, &ctx);

  if (memcmp(hash, expected, SHA256_DIGEST_LENGTH) != 0)
    {
      syslog(LOG_ERR, "[OTA] SHA256 mismatch\n");
      return false;
    }

  syslog(LOG_INFO, "[OTA] SHA256 verified OK\n");
  return true;
}

/****************************************************************************
 * Private Functions - A/B Boot Metadata (otadata)
 *
 * The second-stage bootloader selects the boot slot from the "otadata"
 * partition (0xD000, 8KB).  It contains two 32-byte copies of
 * esp_ota_select_entry_t at offset 0 and 0x1000:
 *
 *   ota_seq     - sequence number; slot = (seq - 1) % app_count
 *   seq_label   - unused (zeros)
 *   ota_state   - ESP_OTA_IMG_VALID = 0x02 (boot directly)
 *   crc         - CRC32 (zlib style) of the ota_seq field only, which is
 *                 exactly what bootloader_common_ota_select_crc() checks
 *
 * The bootloader boots the copy with the highest valid ota_seq, so the
 * new sequence must be larger than both current copies.
 ****************************************************************************/

/* CRC32 over the 4-byte ota_seq, computed EXACTLY like the bootloader's
 * bootloader_common_ota_select_crc() = esp_rom_crc32_le(UINT32_MAX,
 * &seq, 4).  The ROM function inverts the initial value before
 * processing and inverts the result at the end, so with init
 * UINT32_MAX the effective start value is 0 and the final result is
 * ~crc (0x4743989A for seq=1, NOT the zlib 0x99F8B879 nor the raw
 * 0x66074786).  Any other variant fails the bootloader's CRC check.
 */

static uint32_t ota_crc32(FAR const uint8_t *data, size_t len)
{
  uint32_t crc;

  crc = ~0xFFFFFFFFu;  /* esp_rom_crc32_le inverts the init value */

  while (len--)
    {
      crc ^= *data++;
      for (int i = 0; i < 8; i++)
        {
          crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
        }
    }

  return ~crc;  /* final inversion, same as esp_rom_crc32_le */
}

static int ota_otadata_read(FAR struct ota_otadata_entry_s *e0,
                            FAR struct ota_otadata_entry_s *e1)
{
  FAR struct inode *inode;
  FAR struct mtd_dev_s *mtd;
  struct mtd_geometry_s geo;
  struct ota_otadata_entry_s entry;
  int ret;
  int i;

  ret = find_mtddriver(OTA_OTADATA_DEVPATH, &inode);
  if (ret < 0)
    {
      return ret;
    }

  mtd = inode->u.i_mtd;

  ret = MTD_IOCTL(mtd, MTDIOC_GEOMETRY,
                  (unsigned long)((uintptr_t)&geo));
  if (ret < 0)
    {
      close_mtddriver(inode);
      return ret;
    }

  for (i = 0; i < 2; i++)
    {
      ret = MTD_READ(mtd, i * geo.erasesize, sizeof(entry),
                     (FAR uint8_t *)&entry);
      if (ret != (int)sizeof(entry))
        {
          close_mtddriver(inode);
          return ret;
        }

      if (i == 0)
        {
          *e0 = entry;
        }
      else
        {
          *e1 = entry;
        }
    }

  close_mtddriver(inode);
  return 0;
}

static int ota_write_otadata(FAR const char *target)
{
  FAR struct inode *inode;
  FAR struct mtd_dev_s *mtd;
  struct mtd_geometry_s geo;
  struct ota_otadata_entry_s e0;
  struct ota_otadata_entry_s e1;
  struct ota_otadata_entry_s entry;
  FAR struct ota_otadata_entry_s *sel;
  uint32_t max_seq;
  uint32_t new_seq;
  uint32_t target_idx;
  int active_idx = -1;
  int write_idx;
  int ret;

  /* target slot index: 0 -> ota_0, 1 -> ota_1 */

  target_idx = (strcmp(target, "/dev/ota0") == 0) ? 0 : 1;

  sel = ota_otadata_active(&e0, &e1, &active_idx);
  if (sel == NULL)
    {
      /* otadata uninitialized (legacy deployment): start from scratch */

      max_seq = 0;
      write_idx = 0;
    }
  else
    {
      max_seq = sel->ota_seq;

      /* Write ONLY the copy that is not the current active one.  The
       * active copy keeps the previous entry untouched as the rollback
       * reference: if the new image fails to confirm, the bootloader
       * aborts the PENDING_VERIFY entry and falls back to the
       * highest-seq VALID entry (the one preserved here).  Writing both
       * copies would break rollback (the second NEW copy would
       * re-select the broken image on the next boot, causing a reboot
       * loop instead of a fallback).
       */

      write_idx = (active_idx == 0) ? 1 : 0;
    }

  /* New sequence must be larger than any previous one and map to the
   * target slot: slot = (seq - 1) % app_count.
   */

  new_seq = max_seq + 1;
  if (((new_seq - 1) & 1) != target_idx)
    {
      new_seq++;
    }

  ret = find_mtddriver(OTA_OTADATA_DEVPATH, &inode);
  if (ret < 0)
    {
      printf("[OTA] find_mtddriver %s failed: %d\n",
             OTA_OTADATA_DEVPATH, ret);
      return ret;
    }

  mtd = inode->u.i_mtd;

  ret = MTD_IOCTL(mtd, MTDIOC_GEOMETRY,
                  (unsigned long)((uintptr_t)&geo));
  if (ret < 0)
    {
      printf("[OTA] otadata geometry failed: %d\n", ret);
      close_mtddriver(inode);
      return ret;
    }

  memset(&entry, 0, sizeof(entry));
  entry.ota_seq   = new_seq;
  entry.ota_state = OTA_OTA_STATE_NEW;
  entry.crc       = ota_crc32((FAR const uint8_t *)&entry.ota_seq, 4);

  ret = MTD_ERASE(mtd, (off_t)write_idx, 1);
  if (ret < 0)
    {
      printf("[OTA] otadata erase failed: %d\n", ret);
      close_mtddriver(inode);
      return ret;
    }

  ret = MTD_WRITE(mtd, write_idx * geo.erasesize, sizeof(entry),
                  (FAR const uint8_t *)&entry);
  if (ret != (int)sizeof(entry))
    {
      printf("[OTA] otadata write failed: %d\n", ret);
      close_mtddriver(inode);
      return ret;
    }

  close_mtddriver(inode);
  if (sel == NULL)
    {
      printf("[OTA] otadata: seq=%u state=NEW -> boot %s on next reset\n",
             new_seq, target);
    }
  else
    {
      printf("[OTA] otadata: seq=%u state=NEW -> boot %s on next reset\n"
             "[OTA] NOTE: new firmware boots as PENDING_VERIFY;\n"
             "      run 'plant ota confirm' after it is up, or the next\n"
             "      reset rolls back to %s automatically\n",
             new_seq, target,
             (((sel->ota_seq - 1) & 1) != 0 ? "/dev/ota1" : "/dev/ota0"));
    }

  return 0;
}

/* Mirror of esp_ota_mark_app_valid_cancel_rollback(): mark the entry the
 * device is currently booted from (highest-seq bootable entry) as VALID,
 * so the bootloader keeps booting it instead of rolling back.  The CRC
 * covers only ota_seq, so changing the state does not invalidate it.
 */

static int ota_mark_current_valid(void)
{
  FAR struct inode *inode;
  FAR struct mtd_dev_s *mtd;
  struct mtd_geometry_s geo;
  struct ota_otadata_entry_s e0;
  struct ota_otadata_entry_s e1;
  FAR struct ota_otadata_entry_s *sel;
  int idx = -1;
  int ret;

  sel = ota_otadata_active(&e0, &e1, &idx);
  if (sel == NULL || idx < 0)
    {
      printf("[OTA] otadata read failed / no bootable entry\n");
      return -EIO;
    }

  if (sel->ota_state == OTA_OTA_STATE_VALID)
    {
      printf("[OTA] Current slot already VALID\n");
      return 0;
    }

  sel->ota_state = OTA_OTA_STATE_VALID;

  ret = find_mtddriver(OTA_OTADATA_DEVPATH, &inode);
  if (ret < 0)
    {
      printf("[OTA] find_mtddriver %s failed: %d\n",
             OTA_OTADATA_DEVPATH, ret);
      return ret;
    }

  mtd = inode->u.i_mtd;

  ret = MTD_IOCTL(mtd, MTDIOC_GEOMETRY,
                  (unsigned long)((uintptr_t)&geo));
  if (ret < 0)
    {
      printf("[OTA] otadata geometry failed: %d\n", ret);
      close_mtddriver(inode);
      return ret;
    }

  ret = MTD_ERASE(mtd, (off_t)idx, 1);
  if (ret < 0)
    {
      printf("[OTA] otadata erase failed: %d\n", ret);
      close_mtddriver(inode);
      return ret;
    }

  ret = MTD_WRITE(mtd, idx * geo.erasesize, sizeof(*sel),
                  (FAR const uint8_t *)sel);
  if (ret != (int)sizeof(*sel))
    {
      printf("[OTA] otadata write failed: %d\n", ret);
      close_mtddriver(inode);
      return ret;
    }

  close_mtddriver(inode);
  printf("[OTA] Current slot marked VALID (rollback cancelled)\n");
  return 0;
}

/****************************************************************************
 * Private Functions - HTTP Client
 ****************************************************************************/

static int ota_http_get_json(FAR const char *host, int port,
                             FAR const char *path,
                             FAR char *buf, size_t buflen)
{
  struct sockaddr_in addr;
  struct timeval tv;
  char request[512];
  int  fd;
  int  ret;
  int  total = 0;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  tv.tv_sec  = OTA_HTTP_TIMEOUT_MS / 1000;
  tv.tv_usec = (OTA_HTTP_TIMEOUT_MS % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(port);
  addr.sin_addr.s_addr = inet_addr(host);

  ret = connect(fd, (FAR struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0)
    {
      close(fd);
      return -errno;
    }

  snprintf(request, sizeof(request),
           "GET %s HTTP/1.1\r\n"
           "Host: %s:%d\r\n"
           "Connection: close\r\n"
           "\r\n",
           path, host, port);

  ret = send(fd, request, strlen(request), 0);
  if (ret < 0)
    {
      close(fd);
      return -errno;
    }

  while (total < (int)buflen - 1)
    {
      ret = recv(fd, buf + total, buflen - total - 1, 0);
      if (ret <= 0)
        {
          break;
        }

      total += ret;
    }

  buf[total] = '\0';
  close(fd);

  return total > 0 ? total : -EIO;
}

/****************************************************************************
 * Private Functions - HTTP Download
 *
 * The firmware is written straight to the raw MTD partition that backs
 * /dev/ota0 (/dev/ota1):
 *
 *   1. find_mtddriver() gives direct access to the MTD device, bypassing
 *      the BCH/FTL character-device proxy.  With the proxy, every small
 *      write triggered a full 4KB read-erase-write cycle, which made the
 *      download crawl and freeze the system (the OTA hang).
 *   2. Only the erase blocks that will be written are erased up front,
 *      one 4KB block at a time.
 *   3. recv -> MTD byte write.  The flash is already erased, so plain
 *      programming is enough; no alignment or block-size constraints.
 ****************************************************************************/

static int ota_http_download(FAR const char *host, int port,
                             FAR const char *path,
                             FAR const char *devpath, uint32_t size)
{
  FAR struct inode *inode = NULL;
  FAR struct mtd_dev_s *mtd;
  struct mtd_geometry_s geo;
  struct sockaddr_in addr;
  struct timeval tv;
  fd_set readfds;
  char request[512];
  char buf[OTA_RECV_BUF_SIZE];
  char hdr[OTA_HEADER_MAX];
  size_t hdr_len = 0;
  uint32_t erase_blocks;
  uint32_t i;
  int  fd;
  int  ret;
  int  nrecv;
  int  total = 0;
  int  expected = (int)size;
  bool header_done = false;

  /* 1. Get direct access to the raw MTD partition of the target slot */

  ret = find_mtddriver(devpath, &inode);
  if (ret < 0)
    {
      printf("[OTA] find_mtddriver %s failed: %d\n", devpath, ret);
      return OTA_ERR_FLASH_WRITE;
    }

  mtd = inode->u.i_mtd;

  ret = MTD_IOCTL(mtd, MTDIOC_GEOMETRY,
                  (unsigned long)((uintptr_t)&geo));
  if (ret < 0)
    {
      printf("[OTA] MTDIOC_GEOMETRY failed: %d\n", ret);
      close_mtddriver(inode);
      return ret;
    }

  printf("[OTA] Slot %s: blocksize=%u erasesize=%u eraseblocks=%u\n",
         devpath, geo.blocksize, geo.erasesize, geo.neraseblocks);

  /* Safety guard: never erase/write the slot the device is currently
   * booted from (erasing it destroys the executing code -> hang).
   */

  if (g_ota_ctx.active_slot != NULL &&
      strcmp(devpath, g_ota_ctx.active_slot) == 0)
    {
      printf("[OTA] Refusing to write %s: it is the currently booted "
             "slot (writing it would erase the running firmware)\n",
             devpath);
      close_mtddriver(inode);
      return OTA_ERR_FLASH_WRITE;
    }

  /* Sanity: the firmware image must fit inside the slot */

  if (expected <= 0 ||
      (uint32_t)expected > geo.neraseblocks * geo.erasesize)
    {
      printf("[OTA] Firmware size %d does not fit in %s\n", expected,
             devpath);
      close_mtddriver(inode);
      return OTA_ERR_SIZE_EXCEEDED;
    }

  /* 2. Erase only the blocks that will be written (4KB at a time) */

  erase_blocks = ((uint32_t)expected + geo.erasesize - 1) / geo.erasesize;
  printf("[OTA] Erasing %u x %u bytes...\n", erase_blocks, geo.erasesize);

  for (i = 0; i < erase_blocks; i++)
    {
      ret = MTD_ERASE(mtd, (off_t)i, 1);
      if (ret < 0)
        {
          printf("[OTA] Erase block %u failed: %d\n", i, ret);
          close_mtddriver(inode);
          return OTA_ERR_FLASH_WRITE;
        }
    }

  printf("[OTA] Erase done\n");

  /* 3. Connect to the server */

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      close_mtddriver(inode);
      return -errno;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(port);
  addr.sin_addr.s_addr = inet_addr(host);

  ret = connect(fd, (FAR struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0)
    {
      printf("[OTA] connect failed: %d\n", errno);
      close(fd);
      close_mtddriver(inode);
      return -errno;
    }

  snprintf(request, sizeof(request),
           "GET %s HTTP/1.1\r\n"
           "Host: %s:%d\r\n"
           "Connection: close\r\n"
           "\r\n",
           path, host, port);

  ret = send(fd, request, strlen(request), 0);
  if (ret < 0)
    {
      printf("[OTA] send failed: %d\n", errno);
      close(fd);
      close_mtddriver(inode);
      return -errno;
    }

  printf("[OTA] Downloading to %s...\n", devpath);

  /* 4. recv + write loop */

  while (1)
    {
      FD_ZERO(&readfds);
      FD_SET(fd, &readfds);
      tv.tv_sec  = 15;
      tv.tv_usec = 0;

      ret = select(fd + 1, &readfds, NULL, NULL, &tv);
      if (ret <= 0)
        {
          printf("[OTA] select timeout: ret=%d errno=%d\n", ret, errno);
          break;
        }

      nrecv = recv(fd, buf, sizeof(buf), 0);
      if (nrecv <= 0)
        {
          printf("[OTA] recv end: ret=%d errno=%d\n", nrecv, errno);
          break;
        }

      if (!header_done)
        {
          /* Accumulate the header until its end marker is found */

          size_t room = sizeof(hdr) - 1 - hdr_len;
          size_t n = (size_t)nrecv < room ? (size_t)nrecv : room;
          size_t chunk_left = (size_t)nrecv - n;
          FAR char *body;
          FAR char *cl;
          size_t body_len;

          memcpy(hdr + hdr_len, buf, n);
          hdr_len += n;
          hdr[hdr_len] = '\0';

          body = memmem(hdr, hdr_len, "\r\n\r\n", 4);
          if (body == NULL)
            {
              if (hdr_len >= sizeof(hdr) - 1)
                {
                  printf("[OTA] HTTP header too large\n");
                  break;
                }

              continue;  /* need more header data */
            }

          /* Report the Content-Length when the server provides it */

          cl = memmem(hdr, hdr_len, "Content-Length:", 15);
          if (cl != NULL)
            {
              printf("[OTA] Content-Length: %d\n", atoi(cl + 15));
            }

          body += 4;
          body_len = hdr_len - (body - hdr);

          if (body_len > 0)
            {
              ret = MTD_WRITE(mtd, total, body_len,
                              (FAR const uint8_t *)body);
              if (ret != (int)body_len)
                {
                  printf("[OTA] write failed: %d\n", ret);
                  break;
                }

              total += ret;
            }

          /* Any body bytes of this chunk that did not fit in hdr */

          if (chunk_left > 0)
            {
              ret = MTD_WRITE(mtd, total, chunk_left,
                              (FAR const uint8_t *)(buf + n));
              if (ret != (int)chunk_left)
                {
                  printf("[OTA] write failed: %d\n", ret);
                  break;
                }

              total += ret;
            }

          header_done = true;
          printf("[OTA] Header done\n");
          continue;
        }

      /* Firmware body: program straight into the pre-erased flash */

      ret = MTD_WRITE(mtd, total, (size_t)nrecv,
                      (FAR const uint8_t *)buf);
      if (ret != nrecv)
        {
          printf("[OTA] write failed: %d\n", ret);
          break;
        }

      total += ret;

      /* 阶段 A：逐块更新共享进度（UI 轮询 ota_get_status 显示 %） */

      if (g_ota_ctx.initialized)
        {
          g_ota_ctx.status.downloaded = total;
        }

      if ((total % (64 * 1024)) < OTA_RECV_BUF_SIZE)
        {
          printf("[OTA] Progress: %d / %d bytes\n", total, expected);
        }
    }

  close(fd);
  close_mtddriver(inode);

  if (total != expected)
    {
      printf("[OTA] Incomplete download: %d / %d bytes\n", total, expected);
      return -EIO;
    }

  printf("[OTA] Downloaded %d / %d bytes to %s\n", total, expected, devpath);
  return total;
}

/****************************************************************************
 * Private Functions - Version Parsing
 ****************************************************************************/

static int ota_parse_version_json(FAR const char *json,
                                  FAR struct ota_version_info_s *info)
{
  FAR char *ptr;
  FAR char *end;

  memset(info, 0, sizeof(struct ota_version_info_s));

  ptr = strstr(json, "\"version\"");
  if (ptr == NULL)
    {
      return -EINVAL;
    }

  ptr = strchr(ptr, ':');
  if (ptr == NULL)
    {
      return -EINVAL;
    }

  ptr = strchr(ptr, '"');
  if (ptr == NULL)
    {
      return -EINVAL;
    }

  ptr++;
  info->version_major = strtoul(ptr, &end, 10);
  if (*end != '.')
    {
      return -EINVAL;
    }

  ptr = end + 1;
  info->version_minor = strtoul(ptr, &end, 10);
  if (*end != '.')
    {
      return -EINVAL;
    }

  ptr = end + 1;
  info->version_patch = strtoul(ptr, &end, 10);

  ptr = strstr(json, "\"size\"");
  if (ptr != NULL)
    {
      ptr = strchr(ptr, ':');
      if (ptr != NULL)
        {
          info->size = strtoul(ptr + 1, NULL, 10);
        }
    }

  ptr = strstr(json, "\"hardware_id\"");
  if (ptr != NULL)
    {
      ptr = strchr(ptr, ':');
      if (ptr != NULL)
        {
          info->hardware_id = strtoul(ptr + 1, NULL, 10);
        }
    }

  ptr = strstr(json, "\"url\"");
  if (ptr != NULL)
    {
      ptr = strchr(ptr, ':');
      if (ptr != NULL)
        {
          ptr = strchr(ptr, '"');
          if (ptr != NULL)
            {
              ptr++;
              end = strchr(ptr, '"');
              if (end != NULL)
                {
                  size_t len = end - ptr;
                  if (len >= sizeof(info->url))
                    {
                      len = sizeof(info->url) - 1;
                    }

                  memcpy(info->url, ptr, len);
                  info->url[len] = '\0';
                }
            }
        }
    }

  ptr = strstr(json, "\"sha256\"");
  if (ptr != NULL)
    {
      ptr = strchr(ptr, ':');
      if (ptr != NULL)
        {
          ptr = strchr(ptr, '"');
          if (ptr != NULL)
            {
              char tmp[3];
              ptr++;
              for (int i = 0; i < 32 && *ptr != '"'; i++)
                {
                  tmp[0] = *ptr++;
                  tmp[1] = *ptr++;
                  tmp[2] = '\0';
                  info->sha256[i] = (uint8_t)strtoul(tmp, NULL, 16);
                }
            }
        }
    }

  return 0;
}

static bool ota_is_newer_version(uint32_t major, uint32_t minor,
                                 uint32_t patch)
{
  if (major > OTA_FW_VERSION_MAJOR)
    {
      return true;
    }

  if (major == OTA_FW_VERSION_MAJOR && minor > OTA_FW_VERSION_MINOR)
    {
      return true;
    }

  if (major == OTA_FW_VERSION_MAJOR && minor == OTA_FW_VERSION_MINOR &&
      patch > OTA_FW_VERSION_PATCH)
    {
      return true;
    }

  return false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ota_init(void)
{
  if (g_ota_ctx.initialized)
    {
      return 0;
    }

  memset(&g_ota_ctx, 0, sizeof(struct ota_ctx_s));

  /* Determine the currently booted slot from otadata (the bootloader
   * writes/uses it at every reset).  There is no KVDB/bootctl layer:
   * otadata alone decides the boot slot and therefore the write target.
   */

  g_ota_ctx.active_slot = ota_get_active_slot();
  if (g_ota_ctx.active_slot == NULL)
    {
      printf("[OTA] No active slot, defaulting to /dev/ota0\n");
      g_ota_ctx.active_slot = "/dev/ota0";
    }

  g_ota_ctx.target_slot = ota_get_target_slot();
  g_ota_ctx.initialized = true;

  printf("[OTA] Init: active=%s target=%s ver=%d.%d.%d\n",
         g_ota_ctx.active_slot, g_ota_ctx.target_slot,
         OTA_FW_VERSION_MAJOR, OTA_FW_VERSION_MINOR,
         OTA_FW_VERSION_PATCH);

  return 0;
}

/* 服务器地址：优先用 SD 卡 plant.cfg 里的 server_host/server_port
 * （与心跳/拍照/语音共用同一个地址），读不到再退回编译期默认值。
 * 这样板卡换环境只要改 plant.cfg，不用重新编译固件。
 */

static char     g_ota_host[128];
static uint16_t g_ota_port = OTA_SERVER_PORT;

static void ota_resolve_server(void)
{
  g_ota_host[0] = '\0';
  g_ota_port    = OTA_SERVER_PORT;

#ifdef CONFIG_PLANT_SD_CARD
  {
    char     ssid[33];
    char     pwd[65];
    char     host[128];
    uint16_t port = OTA_SERVER_PORT;

    ssid[0] = '\0';
    pwd[0]  = '\0';
    host[0] = '\0';

    if (device_cfg_load(ssid, sizeof(ssid), pwd, sizeof(pwd),
                        host, sizeof(host), &port) == 0 &&
        host[0] != '\0')
      {
        strncpy(g_ota_host, host, sizeof(g_ota_host) - 1);
        g_ota_host[sizeof(g_ota_host) - 1] = '\0';
        g_ota_port = port;
        return;
      }
  }
#endif

  strncpy(g_ota_host, OTA_SERVER_HOST, sizeof(g_ota_host) - 1);
  g_ota_host[sizeof(g_ota_host) - 1] = '\0';
}

static const char *ota_server_host(void)
{
  ota_resolve_server();
  return g_ota_host;
}

static int ota_server_port(void)
{
  ota_resolve_server();
  return (int)g_ota_port;
}


/* 阶段 A 新增：查询服务器版本并校验（不含下载）。
 * 供 ota_check_version（UI 检查）与 ota_update_common（确认升级）共用：
 *   返回 OTA_OK            有新版本，info 已填（版本/大小/哈希）
 *   返回 OTA_ERR_NO_UPDATE 已是最新
 *   返回负 ota_error_e     网络/解析/HW/大小错误
 */

static int ota_query_version(FAR struct ota_version_info_s *info)
{
  char buf[1024];
  int  ret;

  if (!g_ota_ctx.initialized)
    {
      ret = ota_init();
      if (ret < 0)
        {
          return ret;
        }
    }

  printf("[OTA] Checking for updates...\n");

  /* Step 1: Query server */

  ret = ota_http_get_json(ota_server_host(), ota_server_port(),
                          OTA_VERSION_PATH, buf, sizeof(buf));
  if (ret < 0)
    {
      printf("[OTA] Query server failed: %d\n", ret);
      g_ota_ctx.status.last_error = OTA_ERR_NETWORK;
      return OTA_ERR_NETWORK;
    }

  /* Step 2: Parse version */

  ret = ota_parse_version_json(buf, info);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[OTA] Parse version failed\n");
      g_ota_ctx.status.last_error = OTA_ERR_NETWORK;
      return OTA_ERR_NETWORK;
    }

  printf("[OTA] Server: %d.%d.%d size=%u hw=%u\n",
         info->version_major, info->version_minor,
         info->version_patch, info->size,
         info->hardware_id);

  /* Step 3: Check version */

  if (!ota_is_newer_version(info->version_major,
                            info->version_minor,
                            info->version_patch))
    {
      printf("[OTA] Already up to date\n");
      return OTA_ERR_NO_UPDATE;
    }

  /* Step 4: Check hardware ID */

  if (info->hardware_id != OTA_CURRENT_HARDWARE_ID)
    {
      syslog(LOG_ERR, "[OTA] HW ID mismatch: %u != %u\n",
             info->hardware_id, OTA_CURRENT_HARDWARE_ID);
      g_ota_ctx.status.last_error = OTA_ERR_HARDWARE_ID;
      return OTA_ERR_HARDWARE_ID;
    }

  /* Step 5: Check size (the MTD slot check happens again during the
   * download, but fail early here to avoid useless network traffic)
   */

  if (info->size == 0 || info->size > OTA_SLOT_SIZE)
    {
      syslog(LOG_ERR, "[OTA] Invalid firmware size: %u\n",
             info->size);
      g_ota_ctx.status.last_error = OTA_ERR_SIZE_EXCEEDED;
      return OTA_ERR_SIZE_EXCEEDED;
    }

  return OTA_OK;
}

static int ota_update_common(bool do_switch)
{
  struct ota_version_info_s version_info;
  int  ret;

  if (!g_ota_ctx.initialized)
    {
      ret = ota_init();
      if (ret < 0)
        {
          return ret;
        }
    }

  g_ota_ctx.status.state = 0;
  g_ota_ctx.status.last_error = OTA_OK;

  /* Steps 1-5: query + parse + version/HW/size checks */

  ret = ota_query_version(&version_info);
  if (ret != OTA_OK)
    {
      return ret;
    }

  /* Step 6: Download to target slot */

  printf("[OTA] Downloading %d.%d.%d to %s\n",
         version_info.version_major, version_info.version_minor,
         version_info.version_patch, g_ota_ctx.target_slot);

  g_ota_ctx.status.state = 1;  /* DOWNLOADING */
  g_ota_ctx.status.total = version_info.size;

  ret = ota_http_download(ota_server_host(), ota_server_port(),
                          version_info.url, g_ota_ctx.target_slot,
                          version_info.size);
  if (ret < 0)
    {
      printf("[OTA] Download failed: %d\n", ret);
      g_ota_ctx.status.last_error = ret;
      return ret;
    }

  g_ota_ctx.status.downloaded = ret;

  /* Step 7: Verify SHA256 */

  if (!ota_verify_sha256(g_ota_ctx.target_slot,
                         version_info.size, version_info.sha256))
    {
      syslog(LOG_ERR, "[OTA] SHA256 mismatch\n");
      g_ota_ctx.status.last_error = OTA_ERR_SHA256;
      return OTA_ERR_SHA256;
    }

  /* Step 7b: In stage mode (plant ota stage) stop here: the image is
   * staged and verified in the safe slot, but the boot is NOT switched
   * and nothing outside the target slot is touched.  otadata is left
   * untouched, so the active-slot derivation keeps returning the safe
   * fallback on the next run.
   */

  if (!do_switch)
    {
      printf("[OTA] STAGED: new firmware verified in %s\n"
             "[OTA] Boot NOT switched (stage mode, no otadata write)\n",
             g_ota_ctx.target_slot);
      return OTA_OK;
    }

  /* Step 8: Switch the boot to the new slot by writing the otadata
   * partition (read by the second-stage bootloader).  The entry is
   * written as NEW: the bootloader turns it into PENDING_VERIFY, boots
   * the new slot, and rolls back to the previous slot automatically if
   * the new firmware is not confirmed with 'plant ota confirm'.
   */

  printf("[OTA] Applying update...\n");

  /* ⚠️ 阶段 A（OTA UI）：切槽前把当前版本落 SD（/mnt/sd/ota_prev.txt），
   * 供重启后「升级成功」页显示"上一版本"（槽位里是裸固件镜像，无版本头，
   * 无法从另一槽读出版本；SD 不在则静默跳过，页面隐藏该行）。 */

  ota_save_prev_version();

  ret = ota_write_otadata(g_ota_ctx.target_slot);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[OTA] otadata switch failed: %d\n", ret);
      g_ota_ctx.status.last_error = ret;
      return ret;
    }

  printf("[OTA] Update applied, rebooting in 3s...\n");
  printf("[OTA] NOTE: the bootloader will boot %s on next reset\n"
         "      as PENDING_VERIFY.  Verify the version after reboot,\n"
         "      then run 'plant ota confirm' to mark it VALID;\n"
         "      otherwise the next reset rolls back automatically.\n",
         g_ota_ctx.target_slot);

  /* Step 9: Reboot */

  sleep(3);
  up_systemreset();

  return OTA_OK;  /* unreachable */
}

int ota_check_update(void)
{
  return ota_update_common(true);
}

int ota_stage_update(void)
{
  return ota_update_common(false);
}

int ota_get_status(FAR struct ota_status_s *status)
{
  if (status == NULL)
    {
      return -EINVAL;
    }

  *status = g_ota_ctx.status;
  return 0;
}

void ota_get_current_version(FAR int *major, FAR int *minor,
                             FAR int *patch)
{
  if (major != NULL)
    {
      *major = OTA_FW_VERSION_MAJOR;
    }

  if (minor != NULL)
    {
      *minor = OTA_FW_VERSION_MINOR;
    }

  if (patch != NULL)
    {
      *patch = OTA_FW_VERSION_PATCH;
    }
}

/****************************************************************************
 * Name: ota_check_version
 *
 * Description:
 *   阶段 A（OTA UI「检查更新」）：只查询服务器版本并校验，不下载、
 *   不写 otadata、不重启。有新版本时 info 填好版本信息。
 *
 * Returned Value:
 *   OTA_OK 有新版本（info 已填）；OTA_ERR_NO_UPDATE 已最新；
 *   其它负 ota_error_e 错误。
 ****************************************************************************/

int ota_check_version(FAR struct ota_version_info_s *info)
{
  if (info == NULL)
    {
      return -EINVAL;
    }

  return ota_query_version(info);
}

/****************************************************************************
 * Name: ota_is_pending_verify
 *
 * Description:
 *   阶段 A（升级后状态页）：当前启动槽是否处于 PENDING_VERIFY
 *   （bootloader 刚切换过来、等待 confirm 标 VALID；未确认则下次
 *   复位自动回退旧槽）。
 *
 * Returned Value:
 *   true 当前槽 PENDING_VERIFY（刚 OTA 升级完）；false 其它。
 ****************************************************************************/

bool ota_is_pending_verify(void)
{
  struct ota_otadata_entry_s e0;
  struct ota_otadata_entry_s e1;
  FAR struct ota_otadata_entry_s *sel;
  int ret;

  if (!g_ota_ctx.initialized)
    {
      ret = ota_init();
      if (ret < 0)
        {
          return false;
        }
    }

  sel = ota_otadata_active(&e0, &e1, NULL);
  if (sel == NULL)
    {
      return false;
    }

  return sel->ota_state == OTA_OTA_STATE_PENDING_VERIFY;
}

/****************************************************************************
 * Name: ota_save_prev_version
 *
 * Description:
 *   阶段 A：切槽前把当前运行版本写 /mnt/sd/ota_prev.txt（"x.y.z"），
 *   供升级后状态页显示"上一版本"。SD 未挂载时静默失败。
 ****************************************************************************/

static void ota_save_prev_version(void)
{
  FILE *fp;
  char buf[32];

  snprintf(buf, sizeof(buf), "%d.%d.%d\n",
           OTA_FW_VERSION_MAJOR, OTA_FW_VERSION_MINOR,
           OTA_FW_VERSION_PATCH);

  fp = fopen("/mnt/sd/ota_prev.txt", "w");
  if (fp == NULL)
    {
      return;   /* SD 不在，静默跳过 */
    }

  fputs(buf, fp);
  fclose(fp);
}

/****************************************************************************
 * Name: ota_read_prev_version
 *
 * Description:
 *   阶段 A：读 /mnt/sd/ota_prev.txt 的上一版本。文件不存在/损坏
 *   返回负 errno（调用方隐藏"上一版本"行）。
 ****************************************************************************/

int ota_read_prev_version(FAR int *major, FAR int *minor,
                          FAR int *patch)
{
  FILE *fp;
  char buf[32];
  int mj;
  int mn;
  int pt;

  if (major == NULL || minor == NULL || patch == NULL)
    {
      return -EINVAL;
    }

  fp = fopen("/mnt/sd/ota_prev.txt", "r");
  if (fp == NULL)
    {
      return -ENOENT;
    }

  if (fgets(buf, sizeof(buf), fp) == NULL)
    {
      fclose(fp);
      return -EIO;
    }

  fclose(fp);

  if (sscanf(buf, "%d.%d.%d", &mj, &mn, &pt) != 3)
    {
      return -EINVAL;
    }

  *major = mj;
  *minor = mn;
  *patch = pt;
  return 0;
}

/****************************************************************************
 * Name: ota_print_boot_status
 *
 * Description:
 *   Print the otadata state: both copies (seq + state) and the derived
 *   active/target slots.  otadata is the single source of truth for the
 *   boot decision; there is no KVDB/bootctl layer anymore.
 ****************************************************************************/

void ota_print_boot_status(void)
{
  struct ota_otadata_entry_s e0;
  struct ota_otadata_entry_s e1;
  FAR const char *active;
  FAR const char *target;
  int ret;

  ret = ota_otadata_read(&e0, &e1);
  if (ret < 0)
    {
      printf("[OTA] otadata: read failed (%d)\n", ret);
      return;
    }

  active = ota_get_active_slot();
  target = ota_get_target_slot();
  printf("[OTA] otadata: seq0=0x%08x st0=%u | seq1=0x%08x st1=%u\n",
         (unsigned int)e0.ota_seq, (unsigned int)e0.ota_state,
         (unsigned int)e1.ota_seq, (unsigned int)e1.ota_state);
  printf("[OTA] active=%s target=%s\n",
         active != NULL ? active : "(uninitialized)",
         target != NULL ? target : "(uninitialized)");
  printf("[OTA] state: 0=NEW 1=PENDING_VERIFY 2=VALID 3=INVALID 4=ABORTED\n");
}

int ota_confirm(void)
{
  int ret;

  if (!g_ota_ctx.initialized)
    {
      ret = ota_init();
      if (ret < 0)
        {
          return ret;
        }
    }

  /* Mark the currently booted slot as VALID so the bootloader keeps
   * booting it instead of rolling back to the previous slot.
   */

  ret = ota_mark_current_valid();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[OTA] mark valid failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "[OTA] Slot confirmed (VALID)\n");
  return 0;
}
