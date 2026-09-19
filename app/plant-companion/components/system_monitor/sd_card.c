/****************************************************************************
 * sd_card.c — SD card driver implementation
 *
 * SDIO 1-bit interface for ESP32-S3 (per board schematic):
 *   CMD = IO0, CLK = IO43, D0 = IO44
 * Block device /dev/mmcsd1 is registered by the board bringup
 * (esp32s3_bringup.c -> board_sdmmc_initialize -> mmcsd_slotinitialize(1)).
 * No card-detect pin is wired on the board: the driver hard-codes
 * "card present", so the card must be inserted before power-on.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include "sd_card.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* ════════════════════════════════════════════════════════════════════
 * SD 挂载预热重试（2026-09-02 实测结论）
 *
 * 现象（2026-09-02 实机）：同一 sd_card_mount()，"第一次不行、第三次
 * 行"——plant mem → errno=19(ENODEV 设备不可用) → sd status →
 * errno=22(EINVAL 能读卡但数据非 FAT) → sd mount → 成功。三次之间
 * 仅用户敲命令的时间（数秒~数十秒），无代码在跑 → 是卡/控制器上电
 * 后渐进预热（mmcsd 首次探测失败后需时间/后续访问恢复），不是内存
 * 不足、不是卡坏、更不是"自动 vs 手动"路径差异。
 *
 * 旧 boot 自动挂载只重试 3×100ms=0.3s 就放弃 → 开机没挂上就一直
 * 没挂上，用户以为"自动不行手动可以"。修复：
 *   ① boot 失败 → 后台线程每 2s 重试，最长 ~30s（卡热了自动挂上）；
 *   ② 手动 mount 放宽到 5×300ms。
 * ════════════════════════════════════════════════════════════════════ */

#define SD_WARMUP_RETRIES      15   /* 后台预热重试次数（×2s ≈ 30s） */
#define SD_WARMUP_INTERVAL_MS  2000
#define SD_MOUNT_RETRIES       5    /* 单次 mount 内重试（×300ms） */
#define SD_MOUNT_RETRY_MS      300
#define SD_RETRY_STACK         6144 /* 重试线程栈（FAT mount 栈占用较大） */

/* Create the mountpoint directory (/mnt and /mnt/sd). The FAT filesystem
 * is mounted on top of it, so it must exist on the rootfs first. */

static int sd_make_mountpoint(void)
{
  /* /mnt may not exist on the rootfs; create it (ignore "already there") */

  if (mkdir("/mnt", 0777) < 0 && errno != EEXIST)
    {
      printf("[SD] Cannot create /mnt: %d\n", errno);
      return -1;
    }

  if (mkdir(SD_MOUNTPOINT, 0777) < 0 && errno != EEXIST)
    {
      printf("[SD] Cannot create %s: %d\n", SD_MOUNTPOINT, errno);
      return -1;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sd_card_status(void)
{
  struct statfs buf;

  /* statfs on a mounted vfat returns MSDOS_SUPER_MAGIC; on a plain rootfs
   * directory it returns the rootfs type. That distinguishes "mounted"
   * from "not mounted". */

  if (statfs(SD_MOUNTPOINT, &buf) < 0)
    {
      return 0;
    }

  return (buf.f_type == MSDOS_SUPER_MAGIC) ? 1 : 0;
}

int sd_card_mount(void)
{
  int ret;
  int attempt;

  if (sd_card_status())
    {
      printf("[SD] Already mounted at %s\n", SD_MOUNTPOINT);
      return 0;
    }

  if (sd_make_mountpoint() < 0)
    {
      return -1;
    }

  /* ⚠️ 2026-09-02 WROOM 内存修复：系统堆见底（实测 free=4KB/largest=2.6KB）
   * 时，mmcsd/FAT/DMA 的挂载期堆分配会偶发失败 → errno=5(EIO)/19(ENODEV)
   * 假"无卡"。重试容忍瞬时堆不足；卡本身没变。
   * ⚠️ 2026-09-02 v2（预热实测）：即使堆健康，卡上电后首几次访问也可能
   * ENODEV/EINVAL（渐进预热），窗口放宽到 5×300ms=1.5s；仍不行则由
   * sd_card_init 的后台线程继续每 2s 重试最长 30s。 */

  for (attempt = 0; attempt < SD_MOUNT_RETRIES; attempt++)
    {
      if (attempt > 0)
        {
          printf("[SD] Mount retry %d/%d...\n", attempt + 1, SD_MOUNT_RETRIES);
          usleep(SD_MOUNT_RETRY_MS * 1000);
        }

      ret = mount(SD_DEVICE, SD_MOUNTPOINT, "vfat", 0, NULL);
      if (ret == 0)
        {
          break;
        }
    }

  if (ret < 0)
    {
      /* Distinguish the two common failure causes:
       *  - ENODEV: /dev/mmcsd1 missing -> no card at power-on (no hot-plug)
       *  - EINVAL: card present but no valid FAT filesystem
       */

      if (errno == ENODEV || errno == ENOENT || errno == ENOTBLK)
        {
          printf("[SD] Mount failed: %s unavailable (errno=%d)\n",
                 SD_DEVICE, errno);
          printf("[SD]   Card must be inserted before power-on "
                 "(board has no card-detect pin)\n");
        }
      else if (errno == EINVAL)
        {
          printf("[SD] Mount failed: %s has no FAT filesystem (errno=%d)\n",
                 SD_DEVICE, errno);
          printf("[SD]   Format first:  mkfatfs %s   (or mkfs.vfat on a PC)\n",
                 SD_DEVICE);
        }
      else
        {
          printf("[SD] Mount failed: errno=%d\n", errno);
        }

      return -1;
    }

  printf("[SD] Mounted %s at %s\n", SD_DEVICE, SD_MOUNTPOINT);
  return 0;
}

static void *sd_card_warmup_worker(FAR void *arg)
{
  int i;

  (void)arg;

  /* 每 2s 试一次；卡预热完成会自动挂上（sd_card_mount 幂等） */

  for (i = 0; i < SD_WARMUP_RETRIES; i++)
    {
      usleep(SD_WARMUP_INTERVAL_MS * 1000);

      if (sd_card_status())
        {
          printf("[SD] 预热重试: 已挂载（他人挂载，退出）\n");
          return NULL;
        }

      if (sd_card_mount() == 0)
        {
          printf("[SD] 预热重试: 第 %d 次挂载成功（卡预热完成）\n", i + 1);
          return NULL;
        }
    }

  printf("[SD] 预热重试 %d 次仍未挂载（卡未插/未就绪，系统继续）\n",
         SD_WARMUP_RETRIES);
  return NULL;
}

int sd_card_init(void)
{
  pthread_t tid;
  pthread_attr_t attr;
  int ret;

  printf("[SD] init: probing %s...\n", SD_DEVICE);

  if (sd_card_mount() == 0)
    {
      return 0;
    }

  /* ⚠️ 2026-09-02 预热实测：首几次挂载失败不代表没卡——卡/控制器
   * 上电后渐进预热（ENODEV→EINVAL→OK 需数秒~数十秒）。起后台线程
   * 每 2s 重试最长 ~30s，卡热了自动挂上，不等用户手动补。 */

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, SD_RETRY_STACK);
  ret = pthread_create(&tid, &attr, sd_card_warmup_worker, NULL);
  if (ret == 0)
    {
      pthread_detach(tid);
    }
  else
    {
      printf("[SD] init: 预热重试线程创建失败: %d\n", ret);
    }

  printf("[SD] init: SD 未就绪，后台预热重试中（系统继续）\n");
  return -1;
}

int sd_card_umount(void)
{
  int ret;

  if (!sd_card_status())
    {
      printf("[SD] Not mounted\n");
      return 0;
    }

  ret = umount(SD_MOUNTPOINT);
  if (ret < 0)
    {
      printf("[SD] Unmount failed: %d\n", errno);
      return -1;
    }

  printf("[SD] Unmounted %s\n", SD_MOUNTPOINT);
  return 0;
}

int sd_card_test(void)
{
  int fd;
  int ret;
  int was_mounted;
  char buf[64];
  const char *test_data = "Plant Companion SD Card Test\n";

  was_mounted = sd_card_status();

  /* Mount (tolerates already-mounted) */

  ret = sd_card_mount();
  if (ret < 0)
    {
      printf("[SD] Test aborted: cannot mount\n");
      return -1;
    }

  /* Write test file */

  fd = open(SD_MOUNTPOINT "/test.txt",
            O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      printf("[SD] Open for write failed: %d\n", errno);
      goto errout_with_mount;
    }

  ret = write(fd, test_data, strlen(test_data));
  close(fd);
  printf("[SD] Written %d bytes\n", ret);

  if (ret != (int)strlen(test_data))
    {
      goto errout_with_mount;
    }

  /* Read back */

  fd = open(SD_MOUNTPOINT "/test.txt", O_RDONLY);
  if (fd < 0)
    {
      printf("[SD] Open for read failed: %d\n", errno);
      goto errout_with_mount;
    }

  memset(buf, 0, sizeof(buf));
  ret = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  printf("[SD] Read back %d bytes: %s", ret, buf);

  /* Verify */

  if (ret != (int)strlen(test_data) || strcmp(buf, test_data) != 0)
    {
      printf("[SD] Verify FAILED\n");
      goto errout_with_mount;
    }

  printf("[SD] Verify OK!\n");

  /* Unmount only if we mounted it ourselves */

  if (!was_mounted)
    {
      sd_card_umount();
    }

  return 0;

errout_with_mount:
  if (!was_mounted)
    {
      sd_card_umount();
    }

  return -1;
}
