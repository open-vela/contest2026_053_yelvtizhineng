/****************************************************************************
 * apps/plant-companion/main/app_main.c
 *
 * Plant Companion — AI Smart Plant Care System
 *
 * Usage:
 *   nsh> plant wifi <ssid> <password>
 *   nsh> plant ai test
 *   nsh> plant ai advise <temp> <moisture> <ec> <salt> <n> <p> <k> <ph>
 *   nsh> plant soil read
 *   nsh> plant sd test
 ****************************************************************************/

#include <nuttx/config.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <nuttx/clock.h>
#include <nuttx/lcd/lcd_dev.h>

#ifdef CONFIG_PLANT_UI
#  include <lvgl/lvgl.h>
#endif

#ifdef CONFIG_PLANT_UI_PANEL
#  include "../components/ui_panel/lcd_st7796.h"
#endif

#ifdef CONFIG_PLANT_UI
#  include "../ui/ui_app.h"
#  include "../ui/assets/fonts/zh_font.h"
#endif

#ifdef CONFIG_PLANT_WIFI_MANAGER
#  include "../communication/wifi_manager/wifi_manager.h"
#endif

#ifdef CONFIG_PLANT_AI_MODULE
#  include "../ai_module/ai_common.h"
#  include "../ai_module/ai_engine/ai_engine.h"
#  include "../services/server_bridge.h"
#  include "../services/sd_write.h"
#endif

#ifdef CONFIG_PLANT_AI_VOICE
#  include "../ai_module/ai_voice/ai_voice.h"
#  include "../services/voice_service.h"
#  include "../components/voice_agent/voice_agent.h"
#  include "../hal/hal_i2s.h"
#  include "../hal/hal_i2c.h"

/* I2C 总线初始化（内核符号，链接时解析；寄存器快照用） */

extern FAR struct i2c_master_s *esp32s3_i2cbus_initialize(int bus);
#endif

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
#  include "../components/camera_capture/camera_capture.h"
#endif

#ifdef CONFIG_PLANT_GESTURE_SENSOR
#  include "../components/gesture_sensor/gesture_sensor.h"
#endif

#ifdef CONFIG_PLANT_SYSTEM_MONITOR
#  include "../components/system_monitor/battery_monitor.h"
#endif

#ifdef CONFIG_PLANT_SOIL_SENSOR
#  include "../components/sensor_driver/soil_sensor.h"
#  include "../services/sensor_service.h"
#endif

#ifdef CONFIG_PLANT_SD_CARD
#  include "../components/system_monitor/sd_card.h"
#  include "../components/system_monitor/device_cfg.h"
#endif

#ifdef CONFIG_ESP32S3_WIFI
#  include "../ota/ota.h"
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/



/****************************************************************************
 * Private Data — 本会话“最近一次成功”的运行配置记录
 * （自动恢复/成功联网后写回 SD 卡 plant.cfg，见 device_cfg.h）
 ****************************************************************************/

#ifdef CONFIG_PLANT_WIFI_MANAGER
static char s_wifi_ssid[33];
static char s_wifi_password[65];
static bool s_wifi_ok;                 /* 本会话内 WiFi 连接成功过 */
#endif

#ifdef CONFIG_PLANT_AI_MODULE
static char s_srv_host[128];
static uint16_t s_srv_port;
static bool s_srv_ok;                  /* 本会话内服务器地址配置成功过 */
#endif

/****************************************************************************
 * Private Functions — 开机配置持久化/自动恢复
 ****************************************************************************/

#ifdef CONFIG_PLANT_SD_CARD
static void cfg_save_runtime(void)
{
  char o_ssid[33] = { 0 };
  char o_pwd[65] = { 0 };
  char o_host[128] = { 0 };
  uint16_t o_port = 0;
  const char *ssid = NULL;
  const char *password = NULL;
  const char *host = NULL;
  uint16_t port = 0;
  int ret;

  /* ⚠ 2026-09-16：这里原来会**整段**覆盖 SD 配置。
   * 只改了 WiFi 就调本函数时，服务器地址因为 s_srv_ok 还是 false 而被写成空，
   * SD 上原来存好的服务器地址就一起丢了 —— 实测：NSH 里
   *     plant wifi H3C_1209 <密码>
   * 之后 plant cfg show 变成"服务器="，重启后直接报"还没有配置服务器"，
   * 管理台也再也看不到设备在线。
   * 先把 SD 上已有的配置读回来，只有本次真的改过的才覆盖。 */

  (void)device_cfg_load(o_ssid, sizeof(o_ssid), o_pwd, sizeof(o_pwd),
                        o_host, sizeof(o_host), &o_port);

#ifdef CONFIG_PLANT_WIFI_MANAGER
  if (s_wifi_ok)
    {
      ssid = s_wifi_ssid;
      password = s_wifi_password;
    }
  else if (o_ssid[0] != '\0')
    {
      ssid = o_ssid;
      password = o_pwd;
    }
#endif
#ifdef CONFIG_PLANT_AI_MODULE
  if (s_srv_ok)
    {
      host = s_srv_host;
      port = s_srv_port;
    }
  else if (o_host[0] != '\0')
    {
      host = o_host;
      port = o_port;
    }
#endif

  if (ssid == NULL && host == NULL)
    {
      return;
    }

  ret = device_cfg_save(ssid, password, host, port);
  if (ret == 0)
    {
      printf("[Cfg] 已保存开机配置到 /mnt/sd/plant.cfg\n");
    }
  else
    {
      printf("[Cfg] 开机配置保存失败: %d\n", ret);
    }
}
#endif /* CONFIG_PLANT_SD_CARD */

#if defined(CONFIG_PLANT_SD_CARD) && defined(CONFIG_PLANT_WIFI_MANAGER)
/* 读 SD plant.cfg → 自动连 WiFi → 配置服务器（verbose 供 CLI 使用）。
 * WiFi 已连时跳过重连；服务器注册失败不影响返回值（联网结果更重要）。 */

static int cfg_apply_from_file(bool verbose)
{
  char ssid[33] = { 0 };
  char password[65] = { 0 };
  char host[128] = { 0 };
  uint16_t port = 0;
  int ret;

  ret = device_cfg_load(ssid, sizeof(ssid), password, sizeof(password),
                        host, sizeof(host), &port);
  if (ret < 0 && ret != -ENOENT)
    {
      if (verbose)
        {
          printf("[Cfg] 读取配置失败: %d\n", ret);
        }

      return ret;
    }

  if (ssid[0] == '\0' && host[0] == '\0')
    {
      if (verbose)
        {
          printf("[Cfg] SD 上没有保存的开机配置\n");
        }

      return -ENOENT;
    }

  if (ssid[0] != '\0')
    {
      if (!wifi_manager_is_connected())
        {
          wifi_manager_init();
          ret = wifi_manager_connect(ssid, password);
          if (verbose || ret != 0)
            {
              printf("[Cfg] 自动连 WiFi %s → %d\n", ssid, ret);
            }
        }
      else
        {
          ret = 0;
        }

      if (ret == 0)
        {
          strlcpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid));
          strlcpy(s_wifi_password, password, sizeof(s_wifi_password));
          s_wifi_ok = true;
        }
    }

#ifdef CONFIG_PLANT_AI_MODULE
  if (host[0] != '\0')
    {
      uint16_t use_port = (port != 0) ? port : 8000;

      ret = server_bridge_set_server(host, use_port);
      if (verbose || ret != 0)
        {
          printf("[Cfg] 配置服务器 %s:%u → %d\n", host,
                 (unsigned)use_port, ret);
        }

      if (ret == 0)
        {
          strlcpy(s_srv_host, host, sizeof(s_srv_host));
          s_srv_port = use_port;
          s_srv_ok = true;
        }
    }
#endif

  return 0;
}
#endif /* SD && WIFI_MANAGER */

#if defined(CONFIG_PLANT_SD_CARD) && defined(CONFIG_PLANT_WIFI_MANAGER)
/* 开机自动恢复线程：不阻塞 UI 启动；SD 上电预热最长约 40s 内完成重连。 */

static void *boot_cfg_thread(void *arg)
{
  int waited = 0;

  (void)arg;

  while (!sd_card_status() && waited < 40)
    {
      usleep(1000 * 1000);
      waited++;
    }

  cfg_apply_from_file(false);
  return NULL;
}

static void boot_cfg_start(void)
{
  pthread_t tid;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  if (pthread_create(&tid, &attr, boot_cfg_thread, NULL) == 0)
    {
      pthread_detach(tid);
    }
  else
    {
      printf("[Cfg] 开机恢复线程创建失败\n");
    }

  pthread_attr_destroy(&attr);
}
#endif /* SD && WIFI_MANAGER */

#ifdef CONFIG_PLANT_WIFI_MANAGER
static int cmd_wifi(int argc, char *argv[])
{
  if (argc < 3)
    {
      printf("Usage: plant wifi <ssid> [password]\n");
      return -1;
    }

  const char *ssid = argv[2];
  const char *password = (argc >= 4) ? argv[3] : "";

  printf("[WIFI] Connecting to: %s\n", ssid);

  wifi_manager_init();
  int ret = wifi_manager_connect(ssid, password);

  if (ret == 0)
    {
      strlcpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid));
      strlcpy(s_wifi_password, password, sizeof(s_wifi_password));
      s_wifi_ok = true;
      printf("[WIFI] Connected!\n");
#ifdef CONFIG_PLANT_SD_CARD
      cfg_save_runtime();
#endif
    }
  else
    {
      printf("[WIFI] Failed: %d\n", ret);
    }

  return ret;
}
#endif

#ifdef CONFIG_PLANT_AI_VOICE
/* ⚠ 2026-09-01 rec 命令流式化（用户定案：按 LVGL 说话功能的方式做——
 * ai_voice_stream_record 静态缓冲零大 malloc，回调块写 SD 文件） */

static FILE *s_rec_fp;
static uint32_t s_rec_data;
static int32_t s_rec_peak;      /* 信号峰值（|s| 最大） */
static uint64_t s_rec_sq;       /* 均方和（RMS 判据） */
static uint32_t s_rec_nonzero;  /* 非零采样数 */

static void rec_write_cb(FAR const int16_t *pcm, size_t samples, void *arg)
{
  size_t i;
  (void)arg;

  /* ⚠️ 2026-09-16 采样计数移出写盘分支：nowrite 模式（s_rec_fp==NULL）
   * 下也要统计采集量，用于隔离"写 SD 是否拖累采集"。 */

  s_rec_data += (uint32_t)samples * 2;

  if (s_rec_fp != NULL)
    {
      sd_write_aligned(s_rec_fp, pcm, samples * 2);

      /* 信号统计：峰值 / 均方 / 非零率（定位采集弱/削顶/全零） */

      for (i = 0; i < samples; i++)
        {
          int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];

          if (a > s_rec_peak)
            {
              s_rec_peak = a;
            }

          s_rec_sq += (uint64_t)a * (uint64_t)a;
          if (a != 0)
            {
              s_rec_nonzero++;
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────
 * plant voice diag：双段判别（前一半安静 / 后一半说话）
 * 目的：一次录音量化对比两段统计，锁定"录音噪声"根因在哪一层，
 *       而不是猜参数反复烧录。判据：
 *   1) 说话段峰值≥30000 或非零≈100% → 削顶（MIC PGA 增益过高）
 *   2) 安静段峰值已≥10000        → 底噪过大（增益/模拟配置/脉冲）
 *   3) 说话段峰值<3000           → 未拾到语音（模拟配置/硬件）
 *   4) 说话段明显强于安静段       → 设备端拾音正常 → 问题在服务器侧
 * 原始 PCM 同时落 /mnt/sd/rec.wav，可拔卡用 Audacity 看波形/频谱复核。
 * ───────────────────────────────────────────────────────────── */

static uint32_t s_diag_seg;       /* 已累计采样数（段归属判据） */
static uint32_t s_diag_split;     /* 段边界（采样数） */
static int32_t  s_diag_peak[2];   /* 每段 |s| 峰值 */
static uint64_t s_diag_sq[2];     /* 每段均方和 */
static uint32_t s_diag_nonzero[2];
static uint32_t s_diag_spike[2];  /* |s| > 2000（削顶尖峰/脉冲） */
static uint32_t s_diag_hist[2][256];  /* 128 步进直方图（分位数） */
static bool     s_diag_announced; /* 是否已提示"开始说话" */

/* ⚠ 2026-09-02 "听不到人声"判别补充：
 *  - s_diag_zc / s_diag_zc_last：说话段过零率（语音 ~0.05-0.15 低频为主；
 *    宽带噪声 ~0.3-0.5 高频为主）——数据有能量但过零率高 = 录到的是
 *    噪声/环境声而非人声；
 *  - 实际录音耗时（diag 分支记录 ticks）：若实际耗时 ≈ 标称/2 → RX 实际
 *    帧率 48kHz（非 24k）→ 3:2 降采样拿错样本 → 语音变噪声（历史 §14）。 */

static uint32_t s_diag_zc;        /* 说话段过零计数 */
static int32_t  s_diag_zc_last;   /* 上一个采样（跨块） */
static bool     s_diag_zc_init;   /* 过零统计是否已初始化 */

static void diag_write_cb(FAR const int16_t *pcm, size_t samples, void *arg)
{
  size_t i;
  (void)arg;

  if (s_rec_fp != NULL)
    {
      sd_write_aligned(s_rec_fp, pcm, samples * 2);
      s_rec_data += (uint32_t)samples * 2;
    }

  for (i = 0; i < samples; i++)
    {
      int seg = (s_diag_seg + i < s_diag_split) ? 0 : 1;
      int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];

      if (a > s_diag_peak[seg])
        {
          s_diag_peak[seg] = a;
        }

      s_diag_sq[seg] += (uint64_t)a * (uint64_t)a;
      if (a != 0)
        {
          s_diag_nonzero[seg]++;
        }

      if (a > 2000)
        {
          s_diag_spike[seg]++;
        }

      s_diag_hist[seg][(uint32_t)a >> 7]++;

      /* 说话段过零统计（跨块保持状态） */

      if (seg == 1)
        {
          if (s_diag_zc_init)
            {
              if ((pcm[i] >= 0 && s_diag_zc_last < 0) ||
                  (pcm[i] < 0 && s_diag_zc_last >= 0))
                {
                  s_diag_zc++;
                }
            }
          else
            {
              s_diag_zc_init = true;
            }

          s_diag_zc_last = pcm[i];
        }
    }

  /* 跨过半段边界 → 提示开始说话（一次 printf，输出量小不阻塞） */

  if (!s_diag_announced && s_diag_seg + samples >= s_diag_split)
    {
      s_diag_announced = true;
      printf("▶ 开始说话（直到录音结束）\n");
    }

  s_diag_seg += (uint32_t)samples;
}

static void diag_print_seg(int seg, uint32_t nsamp)
{
  uint32_t target[4];
  uint32_t pval[4] = {0, 0, 0, 0};
  uint32_t cum = 0;
  uint32_t k;
  int p;

  if (nsamp == 0)
    {
      return;
    }

  target[0] = nsamp / 2;
  target[1] = nsamp * 9 / 10;
  target[2] = nsamp * 99 / 100;
  target[3] = nsamp * 999 / 1000;
  p = 0;
  for (k = 0; k < 256 && p < 4; k++)
    {
      cum += s_diag_hist[seg][k];
      while (p < 4 && cum > target[p])
        {
          pval[p] = k * 128 + 63;
          p++;
        }
    }

  printf("[Diag] %s: 峰值=%d 均方MS=%u 非零=%u/%u(%.0f%%) 尖峰>2000=%u"
         " | P50=%u P90=%u P99=%u P999=%u\n",
         seg == 0 ? "安静段" : "说话段",
         (int)s_diag_peak[seg],
         (unsigned)(s_diag_sq[seg] / nsamp),
         (unsigned)s_diag_nonzero[seg], (unsigned)nsamp,
         (double)s_diag_nonzero[seg] * 100.0 / (double)nsamp,
         (unsigned)s_diag_spike[seg],
         (unsigned)pval[0], (unsigned)pval[1],
         (unsigned)pval[2], (unsigned)pval[3]);
}

static void diag_verdict(uint32_t ns)
{
  int32_t qpeak = s_diag_peak[0];
  int32_t speak = s_diag_peak[1];
  uint32_t snz  = s_diag_nonzero[1];
  uint32_t snz_pct = (ns > 0) ? snz * 100 / ns : 0;

  printf("[Diag] 判别: ");
  if (speak >= 30000)
    {
      /* 削顶唯一判据 = 峰值接近满幅 32767；非零率高是数据正常的标志 */
      printf("说话段满幅削顶（峰值 %d ≈ 满幅）→ 增益仍过高 → "
             "es7210.c gain 值 9→更低 重测\n", (int)speak);
    }
  else if (snz_pct < 90 && speak > 5000)
    {
      /* 信号强但非零率低 = 数据稀疏/丢失（修复前的 69% 特征） */
      printf("数据稀疏：非零仅 %u%% 但峰值 %d → 丢数据/帧错位"
             "（预读未生效？）\n", (unsigned)snz_pct, (int)speak);
    }
  else if (qpeak >= 10000)
    {
      printf("安静底噪过大（峰值 %d）→ 增益过高 / 模拟配置(REG40 VMID/MICBIAS)"
             " / 68.7Hz 周期脉冲\n", (int)qpeak);
    }
  else if (speak < 3000)
    {
      printf("说话段几乎无信号（峰值 %d）→ ES7210 未拾音"
             "（restore_analog 后 REG40 仍异常 / MICBIAS≠2.87V / 焊点）\n",
             (int)speak);
    }
  else
    {
      printf("设备端拾音正常（安静峰值 %d / 说话峰值 %d，非零 %u%%）"
             "→ 链路干净，下一步验证服务器侧（[DENOISE] 日志 + MiMo 转写）\n",
             (int)qpeak, (int)speak, (unsigned)snz_pct);
    }
}

static int cmd_voice(int argc, char *argv[])
{
  int ret;

  if (argc < 3)
    {
      goto usage;
    }

  /* plant voice i2ctest                    - I2C link to codecs */

  if (strcmp(argv[2], "i2ctest") == 0)
    {
      return ai_voice_i2c_test();
    }

  /* plant voice tone <freq>                  - play a test tone */

  if (strcmp(argv[2], "tone") == 0)
    {
      uint32_t freq = (argc > 3) ? strtoul(argv[3], NULL, 10) : 1000;
      uint32_t t0 = clock_systime_ticks();
      int ret = ai_voice_play_tone(freq, 500);
      uint32_t elapsed = clock_systime_ticks() - t0;

      /* ⚠ 2026-09-02 无声诊断：tone 是纯 TX 链路（正弦→hal_i2s_write→
       * ES8311→PA），不经过录音/RX。无声时打印 TX 外设状态 + ES8311
       * DAC 关键寄存器，一次定位断在哪一段。 */

      /* ⚠ 2026-09-02 耗时判读：500ms@24k = 50 ticks（10ms/tick）。
       * ≈50 → 数据按 24k 实时流出（正常）；明显更大 → 播放被拖慢/
       * 块间有等待（听感断续/音调错）；明显更小 → DMA 假完成丢数据。 */

      printf("[Voice] tone ret=%d 耗时=%u ticks (500ms@24k 应≈50), "
             "无声则看下面寄存器:\n", ret, elapsed);
      hal_i2s_dump_tx();
      {
        extern FAR struct i2c_master_s *esp32s3_i2cbus_initialize(int bus);
        FAR struct i2c_master_s *i2c = esp32s3_i2cbus_initialize(0);

        if (i2c != NULL)
          {
            es8311_dump_dac(i2c);
          }
      }

      return ret;
    }

  /* plant voice vol [0-255]                - 设置/查看 DAC 输出音量
   * ES8311 REG32：0x00=-95.5dB .. 0xFF=+32dB，0xBF≈0dB。缺参数=只看。
   * 初始化后调用立即写寄存器（调音不用重烧）。 */

  if (strcmp(argv[2], "vol") == 0)
    {
      if (argc > 3)
        {
          long v = strtol(argv[3], NULL, 0);

          if (v < 0 || v > 255)
            {
              printf("用法: plant voice vol [0-255]\n");
              return 0;
            }

          voice_agent_set_dac_volume((uint8_t)v);
        }

      printf("[Voice] DAC 音量 REG32=0x%02X (%u)"
             "（0xBF≈0dB，0xFF=+32dB）\n",
             (unsigned)voice_agent_get_dac_volume(),
             (unsigned)voice_agent_get_dac_volume());
      return 0;
    }

  /* plant voice regs                        - I2S TX + ES8311 寄存器全览
   * （无声排查：对比 小智 esp_codec_dev / OpenVela NuttX 参考值） */

  if (strcmp(argv[2], "regs") == 0)
    {
      extern FAR struct i2c_master_s *esp32s3_i2cbus_initialize(int bus);
      FAR struct i2c_master_s *i2c = esp32s3_i2cbus_initialize(0);

      hal_i2s_dump_tx();
      if (i2c != NULL)
        {
          es8311_dump_regs(i2c);
        }
      else
        {
          printf("[Voice] I2C 总线初始化失败，跳过 ES8311 dump\n");
        }

      return 0;
    }

  /* plant voice rec [seconds]                - record mic -> WAV */

  if (strcmp(argv[2], "rec") == 0)
    {
      int sec = (argc > 3) ? atoi(argv[3]) : 3;
      int ms;
      /* ⚠️ 2026-09-16 nowrite：只采集不写 SD（隔离"写盘拖累采集"用） */
      bool nowrite = (argc > 4) && (strcmp(argv[4], "nowrite") == 0);

      /* ⚠ 2026-09-01 流式化（按 LVGL 说话功能方式）：旧实现 malloc
       * (44+16k×2×sec) 整段 WAV，WiFi 后堆 ~16KB，3s=94KB 必 OOM。
       * 改用 ai_voice_stream_record（静态缓冲零大 malloc）+ 回调写
       * /mnt/sd/rec.wav——与说话功能同源、同滤波链。 */

      if (sec <= 0 || sec > AI_VOICE_MAX_SECONDS)
        {
          sec = AI_VOICE_MAX_SECONDS;
        }

      /* ⚠️ 2026-09-16 nowrite 模式：只采集不写 SD。
       * 隔离"写盘是否拖累录音采集"——有写盘时实测输入率仅 16.4kHz
       * （24kHz 的 68%，等于丢 1/3 样本=录音被压缩）；无写盘应 ≈24kHz。 */

      if (!nowrite)
        {
          s_rec_fp = fopen("/mnt/sd/rec.wav", "wb");
          if (s_rec_fp == NULL)
            {
              printf("[Voice] 打开 /mnt/sd/rec.wav 失败（SD 未挂？）\n");
              return -1;
            }

          /* WAV 头占位（44B，数据长度最后回填） */

          {
            uint8_t hdr[44];

            memset(hdr, 0, sizeof(hdr));
            sd_write_aligned(s_rec_fp, hdr, sizeof(hdr));
          }
        }

      s_rec_data = 0;
      s_rec_peak = 0;
      s_rec_sq = 0;
      s_rec_nonzero = 0;
      ms = ai_voice_stream_record(rec_write_cb, NULL, sec, 0);
      if (s_rec_fp != NULL)
        {
          fclose(s_rec_fp);
          s_rec_fp = NULL;
        }

      if (ms < 0)
        {
          printf("[Voice] 流式录音失败: %d\n", ms);
          return ms;
        }

      /* 回填 WAV 头（RIFF/fmt/data，16kHz mono 16bit）；nowrite 无文件跳过 */

      if (nowrite)
        {
          printf("[Voice] nowrite：未写 SD（隔离测试）采集 %u B / %u 采样\n",
                 (unsigned)s_rec_data, (unsigned)(s_rec_data / 2));
        }
      else
      {
        uint8_t hdr[44];
        uint32_t rate = AI_VOICE_AI_RATE;
        uint32_t n = s_rec_data;

        memcpy(hdr, "RIFF", 4);
        hdr[4] = (n + 36) & 0xff;  hdr[5] = ((n + 36) >> 8) & 0xff;
        hdr[6] = ((n + 36) >> 16) & 0xff;  hdr[7] = ((n + 36) >> 24) & 0xff;
        memcpy(hdr + 8, "WAVEfmt ", 8);
        hdr[16] = 16;  hdr[20] = 1;  hdr[21] = 0;
        hdr[22] = 1;  hdr[23] = 0;              /* mono */
        hdr[24] = rate & 0xff;  hdr[25] = (rate >> 8) & 0xff;
        hdr[26] = (rate >> 16) & 0xff;  hdr[27] = (rate >> 24) & 0xff;
        hdr[28] = (rate * 2) & 0xff;  hdr[29] = ((rate * 2) >> 8) & 0xff;
        hdr[30] = ((rate * 2) >> 16) & 0xff;  hdr[31] = ((rate * 2) >> 24) & 0xff;
        hdr[32] = 2;  hdr[33] = 0;              /* block align */
        hdr[34] = 16;  hdr[35] = 0;             /* bits */
        memcpy(hdr + 36, "data", 4);
        hdr[40] = n & 0xff;  hdr[41] = (n >> 8) & 0xff;
        hdr[42] = (n >> 16) & 0xff;  hdr[43] = (n >> 24) & 0xff;

        s_rec_fp = fopen("/mnt/sd/rec.wav", "r+b");
        if (s_rec_fp != NULL)
          {
            sd_write_aligned(s_rec_fp, hdr, sizeof(hdr));
            fclose(s_rec_fp);
          }

        s_rec_fp = NULL;
      }

      if (!nowrite)
        {
          printf("[Voice] 录音 %d ms → /mnt/sd/rec.wav（%u B，16kHz mono）\n",
                 ms, (unsigned)s_rec_data);
        }
      printf("[Voice] 信号统计: 峰值=%d 均方RMS=%u 非零=%u/%u (%.0f%%)\n",
             (int)s_rec_peak,
             (unsigned)(s_rec_data ?
                 (uint32_t)(s_rec_sq / (s_rec_data / 2)) : 0),
             (unsigned)s_rec_nonzero, (unsigned)(s_rec_data / 2),
             s_rec_data ?
                 (double)s_rec_nonzero * 100.0 / (s_rec_data / 2) : 0.0);
      return 0;
    }

  /* plant voice diag [seconds]               - 双段判别：前一半安静、后一半说话
   * （根因锁定工具：一次录音输出两段统计 + 判别结论，不猜参数） */

  if (strcmp(argv[2], "diag") == 0)
    {
      int sec = (argc > 3) ? atoi(argv[3]) : 4;
      uint32_t want;
      uint32_t nseg[2];
      int ms;

      if (sec <= 0 || sec > AI_VOICE_MAX_SECONDS)
        {
          sec = AI_VOICE_MAX_SECONDS;
        }

      s_rec_fp = fopen("/mnt/sd/rec.wav", "wb");
      if (s_rec_fp == NULL)
        {
          printf("[Voice] 打开 /mnt/sd/rec.wav 失败（SD 未挂？）\n");
          return -1;
        }

      {
        uint8_t hdr[44];

        memset(hdr, 0, sizeof(hdr));
        sd_write_aligned(s_rec_fp, hdr, sizeof(hdr));
      }

      want = (uint32_t)sec * AI_VOICE_AI_RATE;   /* 16k 总采样 */
      s_rec_data = 0;
      s_diag_seg = 0;
      s_diag_split = want / 2;
      s_diag_announced = false;
      s_diag_zc = 0;
      s_diag_zc_last = 0;
      s_diag_zc_init = false;
      memset(s_diag_peak, 0, sizeof(s_diag_peak));
      memset(s_diag_sq, 0, sizeof(s_diag_sq));
      memset(s_diag_nonzero, 0, sizeof(s_diag_nonzero));
      memset(s_diag_spike, 0, sizeof(s_diag_spike));
      memset(s_diag_hist, 0, sizeof(s_diag_hist));

      /* ⚠ ES7210 寄存器快照（automute/帧结构根因判别）：
       * REG01=时钟开关 REG08=slave+LRCK_RATE_MODE REG12=TDM开关
       * REG13=ADC automute（官方从不写，复位默认待验证）
       * REG14/15=mute range（bit[1:0]=0=范围0） REG40=模拟电源 REG43/44=MIC1/2 增益 */

      {
        FAR struct i2c_master_s *i2c;
        static const uint8_t  regs[10] = {0x01, 0x08, 0x12, 0x13, 0x14,
                                          0x15, 0x40, 0x43, 0x44, 0x02};
        static const char *names[10] = {"REG01", "REG08", "REG12",
                                        "REG13", "REG14", "REG15",
                                        "REG40", "REG43", "REG44",
                                        "REG02"};
        uint8_t v;
        int ri;

        i2c = esp32s3_i2cbus_initialize(0);
        if (i2c != NULL)
          {
            printf("[Diag] ES7210 寄存器快照:");
            for (ri = 0; ri < 10; ri++)
              {
                v = 0;
                if (hal_i2c_read_reg(i2c, 0x40, regs[ri], &v, 100000) == 0)
                  {
                    printf(" %s=0x%02x", names[ri], v);
                  }
              }

            printf("\n");
          }
      }

      printf("[Diag] 录音 %d s：前 %d s 保持安静，听到提示后开始说话\n",
             sec, sec / 2);
      {
        clock_t t0 = clock_systime_ticks();

        ms = ai_voice_stream_record(diag_write_cb, NULL, sec, 0);
        printf("[Diag] 实际录音耗时 ≈ %lu ms（标称 %d ms）%s\n",
               (unsigned long)(clock_systime_ticks() - t0) * 1000 /
               TICK_PER_SEC, sec * 1000,
               (clock_systime_ticks() - t0) * 1000 / TICK_PER_SEC <
                   (uint32_t)sec * 1000 / 2 ?
                   " ← ⚠ 仅为标称一半：RX 实际帧率 48kHz（非 24k）→ 降采样错位！" :
                   "");
      }

      fclose(s_rec_fp);
      s_rec_fp = NULL;

      if (ms < 0)
        {
          printf("[Voice] 流式录音失败: %d\n", ms);
          return ms;
        }

      /* 回填 WAV 头（16kHz mono 16bit，与 rec 相同） */

      {
        uint8_t hdr[44];
        uint32_t rate = AI_VOICE_AI_RATE;
        uint32_t n = s_rec_data;

        memcpy(hdr, "RIFF", 4);
        hdr[4] = (n + 36) & 0xff;  hdr[5] = ((n + 36) >> 8) & 0xff;
        hdr[6] = ((n + 36) >> 16) & 0xff;  hdr[7] = ((n + 36) >> 24) & 0xff;
        memcpy(hdr + 8, "WAVEfmt ", 8);
        hdr[16] = 16;  hdr[20] = 1;  hdr[21] = 0;
        hdr[22] = 1;  hdr[23] = 0;
        hdr[24] = rate & 0xff;  hdr[25] = (rate >> 8) & 0xff;
        hdr[26] = (rate >> 16) & 0xff;  hdr[27] = (rate >> 24) & 0xff;
        hdr[28] = (rate * 2) & 0xff;  hdr[29] = ((rate * 2) >> 8) & 0xff;
        hdr[30] = ((rate * 2) >> 16) & 0xff;  hdr[31] = ((rate * 2) >> 24) & 0xff;
        hdr[32] = 2;  hdr[33] = 0;
        hdr[34] = 16;  hdr[35] = 0;
        memcpy(hdr + 36, "data", 4);
        hdr[40] = n & 0xff;  hdr[41] = (n >> 8) & 0xff;
        hdr[42] = (n >> 16) & 0xff;  hdr[43] = (n >> 24) & 0xff;

        s_rec_fp = fopen("/mnt/sd/rec.wav", "r+b");
        if (s_rec_fp != NULL)
          {
            sd_write_aligned(s_rec_fp, hdr, sizeof(hdr));
            fclose(s_rec_fp);
          }

        s_rec_fp = NULL;
      }

      nseg[0] = s_diag_split;
      nseg[1] = want - s_diag_split;
      printf("[Diag] 录音 %d ms（%u 采样）\n", ms, (unsigned)want);
      diag_print_seg(0, nseg[0]);
      diag_print_seg(1, nseg[1]);

      /* 说话段过零率（语音 ~0.05-0.2 低频；宽带噪声 ~0.3-0.5） */

      if (nseg[1] > 0)
        {
          uint32_t zc_rate = s_diag_zc * 1000 / nseg[1];

          printf("[Diag] 说话段过零率=%u.%u%% %s\n",
                 (unsigned)(zc_rate / 10), (unsigned)(zc_rate % 10),
                 zc_rate > 300 ?
                     "← 偏高（噪声/环境声特征）" :
                     (zc_rate < 200 ? "← 正常（语音/低频特征）" : ""));
        }

      diag_verdict(nseg[1]);
      printf("[Diag] 原始 PCM 已存 /mnt/sd/rec.wav（可拔卡 Audacity 复核）\n");
      return 0;
    }

  /* plant voice raw [seconds] - 原始 24k RX 单声道流诊断（根因锁定）
   * 直接 hal_i2s_read_slot 收原始数据（无滤波/降采样），输出：
   *   1) 首 64 采样 hex：位偏移（0x1000 倍数）/ 削顶（0x7fff）/ 零分布
   *   2) 收到总数 vs 期望：实际 RX 数据率（≠24k 说明帧结构错位）
   *   3) 峰值/RMS/非零率/尖峰>2000
   *   4) 零值相位 idx%8：结构性零（说话段 31% 零值 → 帧/槽错位）
   * 用法：安静跑一次、说话跑一次对比（1s 足够判结构）。 */

  if (strcmp(argv[2], "raw") == 0)
    {
      static int16_t s_raw[1024];
      static int16_t s_first[64];
      uint32_t want_samp;
      uint32_t got = 0;
      uint32_t ph_zero[8] = {0};
      uint32_t ph_cnt[8] = {0};
      uint32_t nonzero = 0;
      uint32_t spike = 0;
      uint32_t i;
      uint64_t sq = 0;
      int32_t peak = 0;
      int sec;
      int r;

      sec = (argc > 3) ? atoi(argv[3]) : 1;
      if (sec <= 0)
        {
          sec = 1;
        }

      if (sec > 3)
        {
          sec = 3;
        }

      /* ⚠ 2026-09-02 槽位 A/B：第 4 参 = 槽号（0=MIC1 左，1=MIC2 右）。
       * 录音"有数据但无人声"时分别跑 slot 0 / slot 1 对比峰值——
       * 若说话时 1 号槽峰值远大于 0 号 → 实际麦克风在右声道，读错槽。 */

      {
        int slot = (argc > 4) ? atoi(argv[4]) : 0;

        printf("[Raw] 槽位=%d（0=MIC1 左 / 1=MIC2 右）\n", slot);

        want_samp = (uint32_t)sec * 24000;

        ret = ai_voice_init();
        if (ret < 0)
          {
            return ret;
          }

        memset(s_first, 0, sizeof(s_first));

        {
          clock_t t0 = clock_systime_ticks();

          while (got < want_samp)
            {
              uint32_t chunk = want_samp - got;

              if (chunk > 1024)
                {
                  chunk = 1024;
                }

              r = hal_i2s_read_slot(s_raw, chunk * 2, slot);
              if (r <= 0)
                {
                  printf("[Raw] RX error: %d (got=%u/%u)\n", r,
                         (unsigned)got, (unsigned)want_samp);
                  break;
                }

              for (i = 0; i < (uint32_t)r / 2; i++)
                {
                  uint32_t idx = got + i;
                  int32_t a = s_raw[i] < 0 ? -s_raw[i] : s_raw[i];

                  if (idx < 64)
                    {
                      s_first[idx] = s_raw[i];
                    }

                  if (a > peak)
                    {
                      peak = a;
                    }

                  sq += (uint64_t)a * (uint64_t)a;
                  if (a != 0)
                    {
                      nonzero++;
                    }

                  if (a > 2000)
                    {
                      spike++;
                    }

                  ph_zero[idx & 7] += (a == 0) ? 1 : 0;
                  ph_cnt[idx & 7]++;
                }

              got += (uint32_t)r / 2;
            }

          /* ⚠ 帧率判别：1s 请求 24000 采样 @24k 应耗时 ~1000ms；
           * 若 ~500ms → RX 实际 48kHz → 24k 名义降采样错位 → 语音变噪声 */

          printf("[Raw] 实际采集耗时 ≈ %lu ms（标称 %d ms）%s\n",
                 (unsigned long)(clock_systime_ticks() - t0) * 1000 /
                 TICK_PER_SEC, sec * 1000,
                 (clock_systime_ticks() - t0) * 1000 / TICK_PER_SEC <
                     (uint32_t)sec * 1000 / 2 ?
                     " ← ⚠ 仅为标称一半：RX 实际 48kHz，非 24k！" : "");
        }
      }

      printf("[Raw] 首64采样(hex):\n");
      for (i = 0; i < 64; i += 8)
        {
          printf("[Raw]   %04x %04x %04x %04x %04x %04x %04x %04x\n",
                 (uint16_t)s_first[i], (uint16_t)s_first[i + 1],
                 (uint16_t)s_first[i + 2], (uint16_t)s_first[i + 3],
                 (uint16_t)s_first[i + 4], (uint16_t)s_first[i + 5],
                 (uint16_t)s_first[i + 6], (uint16_t)s_first[i + 7]);
        }

      printf("[Raw] 收到 %u/%u 采样 峰值=%d RMS=%u 非零=%u%% 尖峰>2000=%u\n",
             (unsigned)got, (unsigned)want_samp, (int)peak,
             (unsigned)(got ? (uint32_t)(sq / got) : 0),
             (unsigned)(got ? nonzero * 100 / got : 0),
             (unsigned)spike);
      printf("[Raw] 零值相位 idx%%8: ");
      for (i = 0; i < 8; i++)
        {
          printf("p%u=%u%% ", (unsigned)i,
                 (unsigned)(ph_cnt[i] ? ph_zero[i] * 100 / ph_cnt[i] : 0));
        }

      printf("\n");
      return 0;
    }

  /* plant voice loop [seconds]               - record then play back
   * (validates the full ADC -> I2S -> DAC path in one call) */

  if (strcmp(argv[2], "loop") == 0)
    {
      int sec = (argc > 3) ? atoi(argv[3]) : 2;
      int ms;

      /* ⚠ 2026-09-01 流式化（同 rec）：录到 /mnt/sd/rec.wav（静态缓冲
       * 零大 malloc）→ ai_voice_play_file 流式回放（喇叭听，验证
       * ADC→I2S→DAC 全链路，器件测试标准流程）。 */

      if (sec <= 0 || sec > AI_VOICE_MAX_SECONDS)
        {
          sec = AI_VOICE_MAX_SECONDS;
        }

      s_rec_fp = fopen("/mnt/sd/rec.wav", "wb");
      if (s_rec_fp == NULL)
        {
          printf("[Voice] 打开 /mnt/sd/rec.wav 失败（SD 未挂？）\n");
          return -1;
        }

      {
        uint8_t hdr[44];

        memset(hdr, 0, sizeof(hdr));
        sd_write_aligned(s_rec_fp, hdr, sizeof(hdr));
      }

      s_rec_data = 0;
      s_rec_peak = 0;
      s_rec_sq = 0;
      s_rec_nonzero = 0;
      ms = ai_voice_stream_record(rec_write_cb, NULL, sec, 0);
      fclose(s_rec_fp);
      s_rec_fp = NULL;

      if (ms < 0)
        {
          printf("[Voice] 流式录音失败: %d\n", ms);
          return ms;
        }

      /* 回填 WAV 头（16kHz mono 16bit） */

      {
        uint8_t hdr[44];
        uint32_t rate = AI_VOICE_AI_RATE;
        uint32_t n = s_rec_data;

        memcpy(hdr, "RIFF", 4);
        hdr[4] = (n + 36) & 0xff;  hdr[5] = ((n + 36) >> 8) & 0xff;
        hdr[6] = ((n + 36) >> 16) & 0xff;  hdr[7] = ((n + 36) >> 24) & 0xff;
        memcpy(hdr + 8, "WAVEfmt ", 8);
        hdr[16] = 16;  hdr[20] = 1;  hdr[21] = 0;
        hdr[22] = 1;  hdr[23] = 0;
        hdr[24] = rate & 0xff;  hdr[25] = (rate >> 8) & 0xff;
        hdr[26] = (rate >> 16) & 0xff;  hdr[27] = (rate >> 24) & 0xff;
        hdr[28] = (rate * 2) & 0xff;  hdr[29] = ((rate * 2) >> 8) & 0xff;
        hdr[30] = ((rate * 2) >> 16) & 0xff;  hdr[31] = ((rate * 2) >> 24) & 0xff;
        hdr[32] = 2;  hdr[33] = 0;
        hdr[34] = 16;  hdr[35] = 0;
        memcpy(hdr + 36, "data", 4);
        hdr[40] = n & 0xff;  hdr[41] = (n >> 8) & 0xff;
        hdr[42] = (n >> 16) & 0xff;  hdr[43] = (n >> 24) & 0xff;

        s_rec_fp = fopen("/mnt/sd/rec.wav", "r+b");
        if (s_rec_fp != NULL)
          {
            sd_write_aligned(s_rec_fp, hdr, sizeof(hdr));
            fclose(s_rec_fp);
          }

        s_rec_fp = NULL;
      }

      printf("[Voice] Loopback: 播放 %u B 录音...\n", (unsigned)s_rec_data);
      ret = ai_voice_play_file("/mnt/sd/rec.wav");

      /* ⚠ 2026-09-02 无声诊断：播放"完成"却无声时，打印 TX 外设状态
       * + ES8311 DAC 关键寄存器：
       *  - I2S TX_START=1 / TX_IDLE（播放后应为 1=已串行化完）/ STOP_EN=0
       *  - ES8311 REG09 bit6=0(未静音) bit7=0(I2S输入) REG31/32(音量)
       *    REG0D/0E/12(电源) */

      printf("[Voice] 无声则看下面寄存器:\n");
      hal_i2s_dump_tx();
      {
        extern FAR struct i2c_master_s *esp32s3_i2cbus_initialize(int bus);
        FAR struct i2c_master_s *i2c = esp32s3_i2cbus_initialize(0);

        if (i2c != NULL)
          {
            es8311_dump_dac(i2c);
          }
      }

      return ret;
    }

  /* plant voice get <server_path> <file>   - 按服务器路径下载到 SD 文件
   *   （验证"下载→落盘→播放"链路，例如把上一轮 TTS 音频抓下来回放） */

  if (strcmp(argv[2], "get") == 0)
    {
      int n;

      if (argc < 5)
        {
          printf("Usage: plant voice get <server_path> <local_file>\n");
          printf("  e.g. plant voice get /api/v1/media/md_xxx/file "
                 "/mnt/sd/tts_test.wav\n");
          return -1;
        }

      n = server_bridge_fetch_path(argv[3], argv[4]);
      printf("[Voice] GET %s -> %s : %d B\n", argv[3], argv[4], n);

      if (n > 0)
        {
          printf("[Voice] 现在可以: plant voice play %s\n", argv[4]);
          return 0;
        }

      return -1;
    }

  /* plant voice play <wav路径>              - 播放任意 SD WAV（排查用：
   *   PC 生成 3s 440Hz 16k mono WAV 放 SD → 若纯音也卡 = 播放路径问题；
   *   配合播放耗时打印（实时连续/有停顿）判读） */

  if (strcmp(argv[2], "play") == 0)
    {
      if (argc < 4)
        {
          printf("Usage: plant voice play <wav path>\n");
          return -1;
        }

      return ai_voice_play_file(argv[3]);
    }

  /* plant voice talk [seconds]             - 跑一轮完整语音会话回归
   *   （录音 → 服务器理解 → 边下边播），打印各阶段耗时。聊天功能不走
   *   控制台，这条命令是为了在板卡上做端到端计时验证。 */

  if (strcmp(argv[2], "talk") == 0)
    {
      int sec = (argc > 3) ? atoi(argv[3]) : 5;
      uint32_t t0;
      int ret;

      if (sec < 1 || sec > 5)
        {
          sec = 5;
        }

      t0 = clock_systime_ticks();
      ret = voice_service_talk(sec);
      if (ret < 0)
        {
          printf("[Voice] talk 启动失败: %d\n", ret);
          return ret;
        }

      /* 会话在 worker 线程异步跑：先给它 300ms 起跑，再等回 IDLE。
       * 超时 120s 兜底（finalize 最多 90s + 播放几十秒）。 */

      usleep(300 * 1000);
      while (voice_service_busy())
        {
          if (clock_systime_ticks() - t0 > 12000)
            {
              printf("[Voice] talk 超时（>120s），放弃等待\n");
              return -1;
            }

          usleep(100 * 1000);
        }

      printf("[Voice] talk 完成，端到端 %u ticks (10ms/tick)\n",
             (unsigned)(clock_systime_ticks() - t0));
      return 0;
    }

  if (strcmp(argv[2], "server") == 0)
    {
      /* 配置服务器辅助手地址（语音链路：分段上传 + finalize） */

      extern int server_bridge_set_server(const char *host, uint16_t port);
      uint16_t port = (argc > 4) ? (uint16_t)atoi(argv[4]) : 8000;

      if (argc < 4)
        {
          printf("Usage: plant voice server <ip> [port]\n");
          return -1;
        }

      {
        int cfg_ret = server_bridge_set_server(argv[3], port);

#if defined(CONFIG_PLANT_AI_MODULE) && defined(CONFIG_PLANT_SD_CARD)
        if (cfg_ret == 0)
          {
            strlcpy(s_srv_host, argv[3], sizeof(s_srv_host));
            s_srv_port = port;
            s_srv_ok = true;
            cfg_save_runtime();
          }
#endif
        return cfg_ret;
      }
    }

usage:
  printf("Usage:\n");
  printf("  plant voice tone <freq>      Play a %d ms test tone\n", 500);
  printf("  plant voice rec [seconds]    Record mic -> WAV (16 kHz)\n");
  printf("  plant voice diag [seconds]   A/B: quiet vs speech stats (root-cause)\n");
  printf("  plant voice raw [seconds] [slot]  Raw 24k RX dump (0=MIC1/1=MIC2)\n");
  printf("  plant voice loop [seconds]   Record then play back\n");
  printf("  plant voice i2ctest          I2C link test (codecs)\n");
  printf("  plant voice regs             I2S TX + ES8311 register dump\n");
  printf("  plant voice server <ip>      Set AI server bridge address\n");
  printf("  plant voice get <path> <file>  GET server path -> SD file\n");
  printf("  plant voice play <wav path>    Play a SD WAV file\n");
  return -1;
}
#endif

#ifdef CONFIG_PLANT_AI_MODULE
static int cmd_ai(int argc, char *argv[])
{
  if (argc < 3)
    {
      goto usage;
    }
  if (strcmp(argv[2], "test") == 0)
    {
      if (ai_common_init(CONFIG_PLANT_AI_API_KEY) < 0 ||
          ai_engine_init() < 0)
        {
          return -1;
        }

      printf("[AI] OK, key=%s\n", ai_common_get_api_key());
      printf("[AI] Host: %s\n", AI_MIMO_HOST);
      printf("[AI] Model: %s\n", AI_MIMO_MODEL);

      ai_engine_deinit();
      ai_common_deinit();
      return 0;
    }

  if (strcmp(argv[2], "advise") == 0)
    {
      if (argc < 11)
        {
          printf("Usage: plant ai advise <temp> <moisture> <ec> "
                 "<salt> <n> <p> <k> <ph>\n");
          return -1;
        }

      if (ai_common_init(CONFIG_PLANT_AI_API_KEY) < 0 ||
          ai_engine_init() < 0)
        {
          return -1;
        }

      struct soil_data_s data;
      data.temp       = atof(argv[3]);
      data.moisture   = atof(argv[4]);
      data.ec         = atof(argv[5]);
      data.salt       = atof(argv[6]);
      data.nitrogen   = atof(argv[7]);
      data.phosphorus = atof(argv[8]);
      data.potassium  = atof(argv[9]);
      data.ph         = atof(argv[10]);

      struct ai_engine_result_s result;
      if (ai_engine_run(&data, NULL, 0, &result) < 0)
        {
          printf("[AI] Engine failed\n");
          ai_engine_deinit();
          ai_common_deinit();
          return -1;
        }

      ai_engine_print_result(&result);

      ai_engine_deinit();
      ai_common_deinit();
      return 0;
    }

usage:
  printf("Usage:\n");
  printf("  plant ai test\n");
  printf("  plant ai advise <temp> <moisture> <ec> <salt> <n> <p> <k> <ph>\n");
  return -1;
}

/* plant srv — 服务器大脑冒烟/运维（协议 v2，对应 services/server_bridge）：
 *   plant srv status           显示服务器地址/设备凭证状态
 *   plant srv login            重新注册取 token（一般 set server 时已自动）
 *   plant srv time             拉取服务器北京时间
 *   plant srv report <m> <t> <ec> [ph] [salt] [n] [p] [k]  传感器单条上报
 *   plant srv diag            单帧拍照 → 服务器结构化诊断（冒烟）
 */

static int cmd_srv(int argc, char *argv[])
{
  if (argc < 3)
    {
      goto usage;
    }

  if (strcmp(argv[2], "status") == 0)
    {
      const char *host = server_bridge_host();
      const char *tok = server_bridge_device_token();

      printf("[Srv] host=%s\n", (host != NULL && host[0]) ? host : "(未配置)");
      printf("[Srv] device_token=%s\n",
             (tok != NULL && tok[0]) ? tok : "(未注册)");
      printf("[Srv] config_epoch=%d\n", server_bridge_config_epoch());
      return 0;
    }

  if (strcmp(argv[2], "login") == 0)
    {
      return server_bridge_device_login();
    }

  if (strcmp(argv[2], "time") == 0)
    {
      int64_t unix_bj = 0;
      char date[16];
      char tm_buf[8];
      int ret = server_bridge_get_time(&unix_bj, date, sizeof(date),
                                       tm_buf, sizeof(tm_buf));

      if (ret < 0)
        {
          printf("[Srv] time 获取失败: %d\n", ret);
          return ret;
        }

      printf("[Srv] time unix=%lld date=%s time=%s\n",
             (long long)unix_bj, date, tm_buf);
      return 0;
    }

  if (strcmp(argv[2], "report") == 0)
    {
      float m;
      float t;
      float ec;
      float ph = 0.0f;
      float salt = 0.0f;
      float n = 0.0f;
      float p = 0.0f;
      float k = 0.0f;

      if (argc < 6)
        {
          goto usage;
        }

      m = (float)atof(argv[3]);
      t = (float)atof(argv[4]);
      ec = (float)atof(argv[5]);
      if (argc > 6)
        {
          ph = (float)atof(argv[6]);
        }

      if (argc > 7)
        {
          salt = (float)atof(argv[7]);
        }

      if (argc > 8)
        {
          n = (float)atof(argv[8]);
        }

      if (argc > 9)
        {
          p = (float)atof(argv[9]);
        }

      if (argc > 10)
        {
          k = (float)atof(argv[10]);
        }

      return server_bridge_report_telemetry(m, t, ec, ph, salt, n, p, k);
    }

  if (strcmp(argv[2], "diag") == 0)
    {
#if defined(CONFIG_PLANT_AI_MODULE) && defined(CONFIG_PLANT_CAMERA_CAPTURE)
      extern void esp32s3_cam_dvp_set_swap(bool on);
      struct sb_diag_result_s diag;
      const uint8_t *fb = NULL;
      size_t flen = 0;
      clock_t t0;
      int ms;
      int ret;

      esp32s3_cam_dvp_set_swap(true);
      ret = camera_init();
      if (ret < 0)
        {
          printf("[Srv] camera_init failed: %d\n", ret);
          return ret;
        }

      /* 2026-09-10：上传识别的图要够清楚 —— 预览 160×120 只有 38400B，
       * 服务器交给大模型后只能猜。与 UI「拍一拍」共用同一条拍照路径：
       * 切 320×240（camera_mode_photo）→ 抓一帧（camera_photo_capture，
       * 帧留在驱动 rxbuf，零拷贝；驱动内部负责 init/start/stop，所以
       * 不会出现 2026-09-09 那个"get_frame 还是 NULL"的时序坑）。 */

      ret = camera_mode_photo();
      if (ret < 0)
        {
          printf("[Srv] photo mode failed: %d\n", ret);
          return ret;
        }

      ret = camera_photo_capture(&fb, &flen);
      if (ret < 0 || fb == NULL || flen == 0)
        {
          printf("[Srv] capture failed: %d\n", (ret < 0) ? ret : -ENODATA);
          return (ret < 0) ? ret : -ENODATA;
        }

      printf("[Srv] 已拍照高清帧（%u 字节），开始服务器诊断...\n",
             (unsigned)flen);
      t0 = clock_systime_ticks();
      ret = server_bridge_image_diagnose(fb, flen, &diag);
      ms = (int)((clock_systime_ticks() - t0) * 1000 / TICK_PER_SEC);

      if (ret < 0)
        {
          printf("[Srv] 诊断失败: %d（%.1f s）\n", ret, ms / 1000.0);
          return ret;
        }

      printf("[Srv] 诊断完成 %.1f s：\n", ms / 1000.0);
      printf("  植物: %s / %s\n", diag.name, diag.latin);
      printf("  匹配: %d%%  健康分: %d\n", diag.match, diag.health_score);
      printf("  问题:\n%s", diag.issue);
      printf("  建议:\n%s", diag.advice);
      printf("  总结: %s\n", diag.summary);
      return 0;
#else
      printf("[Srv] diag 需要 AI_MODULE + CAMERA_CAPTURE\n");
      return -1;
#endif
    }

usage:
  printf("Usage:\n");
  printf("  plant srv status\n");
  printf("  plant srv login\n");
  printf("  plant srv time\n");
  printf("  plant srv report <moisture%%> <temp> <ec> [light]\n");
  printf("  plant srv diag\n");
  return -1;
}
#endif

#ifdef CONFIG_PLANT_SOIL_SENSOR
static int cmd_soil(int argc, char *argv[])
{
  if (argc >= 3 && strcmp(argv[2], "scan") == 0)
    {
      /* 2026-09-11 排查用：扫【常见波特率 × 从站地址 1..8】，
       * 看总线上到底有没有设备应答。
       *   全是 hits=0 → 接线/供电/模块问题（或传感器坏）；
       *   某个 baud+addr 应答 → 地址或波特率与默认值(0x02@9600)不符。 */

      static const int scan_baud[] = {9600, 4800, 19200, 38400};
      uint8_t resp[32];
      int hits = 0;
      int b;
      int a;

      soil_sensor_init(NULL, 0);
      printf("[SOIL] scan start: baud x addr 1..8\n");

      for (b = 0; b < (int)(sizeof(scan_baud) / sizeof(scan_baud[0])); b++)
        {
          for (a = 1; a <= 8; a++)
            {
              int n = soil_sensor_probe(a, scan_baud[b], resp, sizeof(resp));

              if (n > 0)
                {
                  int i;
                  int ok = soil_sensor_frame_valid(resp, n, (uint8_t)a);

                  /* HIT = 整帧校验过的真设备应答；noise = 收到字节但校验不过
                   * （RX 悬空/串扰），两种都打印，便于现场看总线状态。 */

                  printf("[SOIL] %s baud=%d addr=%d rx %d B:",
                         ok ? "HIT" : "noise", scan_baud[b], a, n);
                  for (i = 0; i < n && i < (int)sizeof(resp); i++)
                    {
                      printf(" %02X", resp[i]);
                    }

                  printf("\n");
                  hits += ok;
                }
            }
        }

      printf("[SOIL] scan done, hits=%d\n", hits);
      return hits > 0 ? 0 : -1;
    }

  if (argc < 3 || strcmp(argv[2], "read") != 0)
    {
      printf("Usage: plant soil read | plant soil scan\n");
      return -1;
    }

  if (soil_sensor_init(NULL, 0) < 0)
    {
      return -1;
    }

  struct soil_data_s data;
  if (soil_sensor_read(&data, 1) < 0)   /* 手动命令：带诊断打印 */
    {
      printf("[SOIL] Read failed\n");
      soil_sensor_deinit();
      return -1;
    }

  soil_sensor_print_data(&data);

  soil_sensor_deinit();
  return 0;
}
#endif

#ifdef CONFIG_PLANT_SD_CARD
static int cmd_sd(int argc, char *argv[])
{
  if (argc < 3)
    {
      goto usage;
    }

  if (strcmp(argv[2], "test") == 0)
    {
      return sd_card_test();
    }

  if (strcmp(argv[2], "mount") == 0)
    {
      return sd_card_mount();
    }

  if (strcmp(argv[2], "umount") == 0)
    {
      return sd_card_umount();
    }

  if (strcmp(argv[2], "status") == 0)
    {
      printf("[SD] %s at %s\n",
             sd_card_status() ? "Mounted" : "Not mounted", SD_MOUNTPOINT);
      return 0;
    }

usage:
  printf("Usage:\n");
  printf("  plant sd status   Show mount status\n");
  printf("  plant sd mount    Mount SD card to %s\n", SD_MOUNTPOINT);
  printf("  plant sd test     Mount + write + read + verify\n");
  printf("  plant sd umount   Unmount SD card\n");
  return -1;
}

/* plant cfg：开机配置（SD plant.cfg）查看/保存/清空/立即恢复 */

static int cmd_cfg(int argc, char *argv[])
{
  if (argc < 3)
    {
      goto usage;
    }

  if (strcmp(argv[2], "save") == 0)
    {
      cfg_save_runtime();
      return 0;
    }

  if (strcmp(argv[2], "clear") == 0)
    {
      int cfg_ret = device_cfg_clear();

      printf("[Cfg] plant.cfg %s\n",
             cfg_ret == 0 ? "已删除（下次重启不再自动联网）" : "删除失败");
      return (cfg_ret == 0) ? 0 : -1;
    }

#ifdef CONFIG_PLANT_WIFI_MANAGER
  if (strcmp(argv[2], "connect") == 0)
    {
      int cfg_ret = cfg_apply_from_file(true);

      return (cfg_ret == 0) ? 0 : 1;
    }
#endif

  if (strcmp(argv[2], "show") == 0)
    {
      char ssid[33] = { 0 };
      char password[65] = { 0 };
      char host[128] = { 0 };
      uint16_t port = 0;
      int cfg_ret;

      printf("[Cfg] 会话内记录:\n");
#ifdef CONFIG_PLANT_WIFI_MANAGER
      printf("  WiFi: %s%s\n", s_wifi_ok ? "已连接 " : "无记录",
             s_wifi_ok ? s_wifi_ssid : "");
#else
      printf("  WiFi: 未编译\n");
#endif
#ifdef CONFIG_PLANT_AI_MODULE
      printf("  服务器: %s%s:%u\n", s_srv_ok ? "" : "无记录",
             s_srv_ok ? s_srv_host : "", (unsigned)s_srv_port);
#else
      printf("  服务器: 未编译\n");
#endif

      cfg_ret = device_cfg_load(ssid, sizeof(ssid), password,
                                sizeof(password), host, sizeof(host), &port);
      if (cfg_ret == 0)
        {
          printf("[Cfg] plant.cfg: WiFi=%s 服务器=%s:%u\n",
                 ssid[0] != '\0' ? ssid : "(无)", host,
                 (unsigned)port);
        }
      else
        {
          printf("[Cfg] plant.cfg 不存在或读取失败: %d\n", cfg_ret);
        }

      return 0;
    }

usage:
  printf("Usage:\n");
  printf("  plant cfg save      把最近成功的 WiFi/服务器配置存入 SD\n");
  printf("  plant cfg show      查看会话内记录与 SD 配置\n");
#ifdef CONFIG_PLANT_WIFI_MANAGER
  printf("  plant cfg connect   按 SD 配置立即恢复联网+服务器\n");
#endif
  printf("  plant cfg clear     删除开机配置\n");
  return -1;
}
#endif

#ifdef CONFIG_ESP32S3_WIFI
static int cmd_ota(int argc, char *argv[])
{
  int ret;

  if (argc < 3)
    {
      goto usage;
    }

  if (strcmp(argv[2], "check") == 0)
    {
      ret = ota_init();
      if (ret < 0)
        {
          printf("[OTA] Init failed: %d\n", ret);
          return ret;
        }

      return ota_check_update();
    }

  if (strcmp(argv[2], "stage") == 0)
    {
      ret = ota_init();
      if (ret < 0)
        {
          printf("[OTA] Init failed: %d\n", ret);
          return ret;
        }

      /* Stage-only: download + SHA256 verify, no otadata write/reboot */

      return ota_stage_update();
    }

  if (strcmp(argv[2], "status") == 0)
    {
      struct ota_status_s status;
      int major;
      int minor;
      int patch;

      ota_get_current_version(&major, &minor, &patch);
      printf("[OTA] Current version: %d.%d.%d\n", major, minor, patch);

      ret = ota_get_status(&status);
      if (ret == 0)
        {
          printf("[OTA] State: %d\n", status.state);
          printf("[OTA] Downloaded: %u / %u\n",
                 status.downloaded, status.total);
          printf("[OTA] Error: %d\n", status.last_error);
        }

      /* 两套状态对照：otadata（实际启动） vs KVDB（记账） */

      ota_print_boot_status();

      return 0;
    }

  if (strcmp(argv[2], "confirm") == 0)
    {
      return ota_confirm();
    }

usage:
  printf("Usage:\n");
  printf("  plant ota check      Check for firmware update (full A/B switch)\n");
  printf("  plant ota stage      Download + verify only, no switch (safe test)\n");
  printf("  plant ota status     Show OTA status (otadata)\n");
  printf("  plant ota confirm    Mark current slot VALID (cancel rollback)\n");
  return -1;
}
#endif

/****************************************************************************
 * cmd_cam — 摄像头调试：探测 + OV3660 JPEG 配置 + 拍照落盘
 ****************************************************************************/

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
static int cmd_cam(int argc, char *argv[])
{
  /* 传感器是否已配置为 RGB565（capture 不再每次软复位重配，
   * 避免 AEC 每次重启导致画面反复偏暗/偏色；切 JPEG(cfg) 后置回 false） */

  static bool s_cam_rgb565_ready;

  if (argc < 3)
    {
      printf("Usage: plant cam probe | cfg | rgb565 | reg | capture | "
             "swap [on|off] | diag | off\n");
      return -1;
    }

  if (strcmp(argv[2], "diag") == 0)
    {
#ifdef CONFIG_ESP32S3_CAM_DVP
      extern void esp32s3_cam_dvp_diag(void);
      esp32s3_cam_dvp_diag();
      return 0;
#else
      printf("[Cam] diag not compiled (CONFIG_ESP32S3_CAM_DVP)\n");
      return -1;
#endif
    }

  if (strcmp(argv[2], "off") == 0)
    {
      /* 2026-09-09 M2（土壤↔拍照引脚互斥）：等效"离开拍照页"的关停序列——
       *   停 CAM/DMA + SCCB 软复位 OV3660（传感器不再驱动 DVP 输出）
       *   → IO42/40 从 DVP 输入态切回 UART0
       *   → 解除土壤挂起 → 恢复 3s 轮询（含 30s 上云节流）。
       * 顺序与 screen_camera 的 cam_on_delete 完全一致；
       * 串口可直接验证，也可作为 UI 流程失效时的兜底恢复手段。 */

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
      int rc = camera_shutdown();
      printf("[Cam] shutdown rc=%d\n", rc);
#endif

#ifdef CONFIG_PLANT_SOIL_SENSOR
      extern void esp32s3_uart0_reclaim_pins(void);

      esp32s3_uart0_reclaim_pins();
      soil_sensor_set_paused(false);
      sensor_service_start(3000, NULL, NULL);
      printf("[Cam] UART0 引脚已收回，土壤轮询恢复\n");
#endif
      return 0;
    }

  if (strcmp(argv[2], "bar") == 0)
    {
      /* 传感器彩条测试图案：判定字节序 / R/B 分量序（见 2.48）。
       * 与场景无关、一拍定案，打印 8 条标准色 × 4 种解码。 */

      int ret = camera_init();

      if (ret < 0)
        {
          return ret;
        }

      if (!s_cam_rgb565_ready)
        {
          ret = camera_config_rgb565_qvga();
          if (ret < 0)
            {
              printf("[Cam] rgb565 config failed: %d\n", ret);
              return ret;
            }

          s_cam_rgb565_ready = true;
        }

      return camera_colorbar_test();
    }

  if (strcmp(argv[2], "probe") == 0)
    {
      return camera_init();
    }

  if (strcmp(argv[2], "swap") == 0)
    {
#ifdef CONFIG_ESP32S3_CAM_DVP
      extern void esp32s3_cam_dvp_set_swap(bool on);
      extern bool esp32s3_cam_dvp_get_swap(void);

      if (argc >= 4)
        {
          if (strcmp(argv[3], "on") == 0)
            {
              esp32s3_cam_dvp_set_swap(true);
            }
          else if (strcmp(argv[3], "off") == 0)
            {
              esp32s3_cam_dvp_set_swap(false);
            }
          else
            {
              printf("Usage: plant cam swap [on|off]\n");
              return -1;
            }
        }

      printf("[Cam] rgb565 byte swap = %s\n",
             esp32s3_cam_dvp_get_swap() ? "on" : "off");
      return 0;
#else
      printf("[Cam] swap not compiled (CONFIG_ESP32S3_CAM_DVP)\n");
      return -1;
#endif
    }

  if (strcmp(argv[2], "cfg") == 0)
    {
      int ret = camera_init();
      if (ret < 0)
        {
          return ret;
        }

      ret = camera_config_jpeg_qvga();
      if (ret == 0)
        {
          s_cam_rgb565_ready = false;
        }

      return ret;
    }

  if (strcmp(argv[2], "rgb565") == 0)
    {
      int ret = camera_init();
      if (ret < 0)
        {
          return ret;
        }

      ret = camera_config_rgb565_qvga();
      if (ret == 0)
        {
          s_cam_rgb565_ready = true;
        }

      return ret;
    }

  if (strcmp(argv[2], "reg") == 0)
    {
      return camera_dump_regs();
    }

  if (strcmp(argv[2], "capture") == 0)
    {
      size_t jpeg_len = 0;
      int ret;

      /* OV3660 RGB565 是大端输出（2.49 彩条实锤）：强制字节交换（防残留/误设） */

      {
        extern void esp32s3_cam_dvp_set_swap(bool on);

        esp32s3_cam_dvp_set_swap(true);
      }

      ret = camera_init();
      if (ret < 0)
        {
          printf("[Cam] camera_init failed: %d\n", ret);
          return ret;
        }

      /* 统一走模式管理：这条命令要的是 160×120 单帧（预览几何）。若上次
       * 拍照把传感器留在 320×240，必须先切回来 —— 否则会按 160 宽解析
       * 320 宽的帧（花屏），还会把 153600B 写进 SD。只配一次：重复
       * capture 不再软复位传感器（AEC/曝光保持收敛）。 */

      ret = camera_mode_preview();
      if (ret < 0)
        {
          printf("[Cam] preview mode failed: %d\n", ret);
          return ret;
        }

      s_cam_rgb565_ready = true;

      /* RGB565 frame: use the DMA driver's static rxbuf as sink target,
       * frame appears in LCD image (LVGL zero-copy via get_frame). */

      {
        extern uint8_t *esp32s3_cam_dvp_get_frame(void);
        uint8_t *fb = esp32s3_cam_dvp_get_frame();

        printf("[Cam] capturing -> LCD preview ...\n");
        ret = camera_capture_frame(fb, CAM_PREVIEW_SIZE, &jpeg_len);

        if (ret == 0)
          {
            printf("[Cam] OK: %u bytes frame\n", jpeg_len);

            /* 单帧"capture→直写 LCD"验证（与 preview 同链路）：
             * 出画面 = capture+写屏链路通；黑屏 = 链路断（看上面诊断） */

            if (lcd_put_rgb565(fb, CAM_PREVIEW_W, CAM_PREVIEW_H,
                               33, 63, 414, 184) < 0)
              {
                printf("[Cam] LCD write FAILED\n");
                ret = -1;
              }
            else
              {
                printf("[Cam] frame drawn to viewport (33,63,414,184)\n");
              }
          }
        else
          {
            printf("[Cam] FAILED: %d\n", ret);
          }
      }

      return ret;
    }

  if (strcmp(argv[2], "preview") == 0)
    {
      /* 动态视频预览（参考官方 dvp_spi_lcd 每帧 draw_bitmap 模式）：
       * 连续 capture → 软件缩放 → LCDDEVIO_PUTAREA 直接写取景框区域。
       * 不经过 LVGL lv_image 渲染（本版本有 bug）。
       * 取景框区域：screen_camera viewport(30,60,420,190) 内部。 */

      int n = (argc >= 4) ? atoi(argv[3]) : 30;
      int i;
      int ret;
      volatile bool eoi = false;
      extern void esp32s3_cam_dvp_set_swap(bool on);
      extern void esp32s3_cam_dvp_set_stream(bool on);
      extern int esp32s3_cam_dvp_init(void);
      extern int esp32s3_cam_dvp_start(void);
      extern int esp32s3_cam_dvp_stop(void);
      extern int esp32s3_cam_dvp_capture(void (*sink)(const uint8_t *,
                                                      size_t, void *),
                                         void *, volatile bool *);
      extern uint8_t *esp32s3_cam_dvp_get_frame(void);

      /* OV3660 RGB565 是大端输出（2.49 彩条实锤）：强制字节交换（防残留/误设） */

      esp32s3_cam_dvp_set_swap(true);

      ret = camera_init();
      if (ret < 0)
        {
          return ret;
        }

      if (!s_cam_rgb565_ready)
        {
          ret = camera_config_rgb565_qvga();
          if (ret < 0)
            {
              printf("[Cam] rgb565 config failed: %d\n", ret);
              return ret;
            }

          s_cam_rgb565_ready = true;
        }

      esp32s3_cam_dvp_init();
      esp32s3_cam_dvp_start();
      esp32s3_cam_dvp_set_stream(true);   /* 滚动采集：每帧只等一个帧周期 */
      lcd_put_rgb565_set_sharpen(false);   /* 预览快路径（无逐像素锐化） */

      /* 首帧预期 ~3s：配置 1s AEC 收敛 + capture 首次 40 帧预热
       * （~1.4s@28FPS）。不是黑屏故障，等首帧出现再判断。 */

      printf("[Cam] preview %d frames: first frame ~3s (warmup), "
             "then per-frame ms shown\n", n);

      for (i = 0; i < n; i++)
        {
          clock_t t0 = clock_systime_ticks();

          ret = esp32s3_cam_dvp_capture(NULL, NULL, &eoi);
          if (ret < 0)
            {
              printf("[Cam] preview frame %d failed: %d\n", i, ret);
              break;
            }

          clock_t t_cap = clock_systime_ticks();

          /* sink=NULL 时 capture 不打印帧头诊断，这里每 5 帧补一次：
           * 全 00 = DMA 无数据；结构乱 = 帧对齐/时序问题 */

          if ((i % 5) == 0)
            {
              const uint8_t *fp = esp32s3_cam_dvp_get_frame();

              printf("[Cam] preview frame %d head: %02x %02x %02x %02x "
                     "%02x %02x %02x %02x\n",
                     i, fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6],
                     fp[7]);
            }

          if (lcd_put_rgb565(esp32s3_cam_dvp_get_frame(), 160, 120,
                             33, 63, 414, 184) < 0)
            {
              printf("[Cam] preview frame %d: LCD write FAILED\n", i);
              break;
            }

          clock_t t_end = clock_systime_ticks();

          printf("[Cam] preview frame %d: total %lu ms (cap %lu + lcd %lu)\n",
                 i,
                 (unsigned long)(t_end - t0) * 1000 / TICK_PER_SEC,
                 (unsigned long)(t_cap - t0) * 1000 / TICK_PER_SEC,
                 (unsigned long)(t_end - t_cap) * 1000 / TICK_PER_SEC);
        }

      esp32s3_cam_dvp_stop();
      esp32s3_cam_dvp_set_stream(false);
      lcd_put_rgb565_set_sharpen(true);
      printf("[Cam] preview done: %d frames\n", i);
      return 0;
    }

  printf("Usage: plant cam probe | cfg | rgb565 | reg | capture | "
         "preview [N] | bar | swap [on|off] | diag\n");
  return -1;
}
#endif

/****************************************************************************
 * cmd_mem — 内存/存储诊断（Phase 0.4，BUG-4 排查工具）
 *
 * 输出：heap（malloc 池）+ LVGL 池 + /mnt/sd 用量。
 * 用途：泄漏复现实验（切 Tab 前后对比）、黑屏时第一手证据。
 ****************************************************************************/

static int cmd_mem(int argc, char *argv[])
{
  struct mallinfo mi;

  (void)argc;
  (void)argv;

  printf("=== MEM DIAG ===\n");

  /* 1. 系统堆（malloc 池）——WROOM 512KB SRAM 的资源健康核心。
   *    判据（工业级阈值，见 docs 资源调度策略）：
   *      free ≥ 16KB 且 largest ≥ 8KB → OK（SD 挂载/音频 apb 瞬态安全）
   *      free 8~16KB                 → 警戒（大 malloc 可能失败）
   *      free < 8KB 或 largest < 4KB → 危险（SD/音频/FAT 会偶发失败） */

  mi = mallinfo();
  printf("heap: free=%u largest=%u total=%u",
         (unsigned)mi.fordblks, (unsigned)mi.mxordblk,
         (unsigned)mi.arena);
  if (mi.fordblks >= 16384 && mi.mxordblk >= 8192)
    {
      printf(" [OK]\n");
    }
  else if (mi.fordblks >= 8192)
    {
      printf(" [警戒]\n");
    }
  else
    {
      printf(" [危险: SD/音频可能偶发失败]\n");
    }

  /* 2. LVGL 池（仅 UI 已初始化时安全） */

#ifdef CONFIG_PLANT_UI
  if (lv_is_initialized())
    {
      lv_mem_monitor_t mon;

      lv_mem_monitor(&mon);
      if (mon.total_size == 0)
        {
          /* 本工程 LVGL 走系统 malloc（无固定池），lv_mem_monitor
           * 输出无意义；真实占用看上方 heap 行，切页细粒度用
           * plant memlog on（2026-09-08 修正误导文案）。 */

          printf("lvgl: heap-backed（无固定池，走系统堆）→ 占用见 heap 行\n");
        }
      else
        {
          printf("lvgl: total=%u free=%u used_pct=%d%% frag=%d%%",
                 (unsigned)mon.total_size, (unsigned)mon.free_size,
                 (int)mon.used_pct, (int)mon.frag_pct);
          if (mon.free_size < mon.total_size / 4)
            {
              printf(" [警戒: UI 池余量 <25%%]\n");
            }
          else
            {
              printf(" [OK]\n");
            }
        }
    }
  else
    {
      printf("lvgl: not initialized\n");
    }
#endif

  /* 3. 静态保留（链接期定死，不占堆）。
   * ⚠ 2026-09-08：LVGL 本版无固定池（走系统 malloc），不再打印
   * "lvgl_pool=80KB" 旧文案（那曾与 CONFIG_LV_MEM_SIZE_KILOBYTES 混淆）。 */

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  printf("static: cam_dvp_rxbuf=37.5KB lcd_wrbuf=9KB（内部 SRAM 常驻）\n");
#else
  printf("static: lcd_wrbuf=9KB（内部 SRAM 常驻）\n");
#endif

  /* 4. 存储（SD 卡用量，NuttX 用 statvfs） */

#ifdef CONFIG_PLANT_SD_CARD
  {
    struct statvfs st;

    if (statvfs("/mnt/sd", &st) == 0)
      {
        uint64_t total = (uint64_t)st.f_blocks * st.f_frsize;
        uint64_t free  = (uint64_t)st.f_bfree * st.f_frsize;

        printf("sd: total=%lluKB free=%lluKB\n",
               (unsigned long long)(total / 1024),
               (unsigned long long)(free / 1024));
      }
    else
      {
        printf("sd: not mounted\n");
      }
  }
#endif

  printf("=== MEM DIAG END ===\n");
  return 0;
}

/****************************************************************************
 * cmd_memlog — 切页内存打点开关（plant memlog on|off，默认关）
 *
 * 设置共享标志，由 ui_task 在每次切页真正完成（延迟删除已冲刷）后
 * 打一行系统堆余量 + 相对首页基线增量：观测各页对象开销、回首页
 * 增量是否归零（泄漏检查）。UI 不运行时设置也安全（下次 UI 启动生效）。
 ****************************************************************************/

#ifdef CONFIG_PLANT_UI
static int cmd_memlog(int argc, char *argv[])
{
  if (argc >= 3 && strcmp(argv[2], "on") == 0)
    {
      ui_app_memlog_ctl(1);
      return 0;
    }

  if (argc >= 3 && strcmp(argv[2], "off") == 0)
    {
      ui_app_memlog_ctl(0);
      return 0;
    }

  printf("Usage: plant memlog on|off\n");
  return -1;
}
#endif

/****************************************************************************
 * cmd_demo — 完整外设演示（明天给老板看）
 ****************************************************************************/

static int cmd_demo(int argc, char *argv[])
{
  int ret;

  printf("\n");
  printf("========================================\n");
  printf("  植小伴 — 全外设演示\n");
  printf("========================================\n\n");

  /* 1. LCD 显示 */

#ifdef CONFIG_PLANT_UI_PANEL
  printf("[1] LCD: 已初始化（main 中）\n\n");
#else
  printf("[1] LCD: 未配置\n\n");
#endif

  /* 2. IMU (ICM-42607) */

#ifdef CONFIG_PLANT_GESTURE_SENSOR
  printf("[2] IMU (ICM-42607)...\n");
  ret = imu_init();
  if (ret == 0)
    {
      int16_t ax, ay, az;
      float temp;
      ret = imu_read_accel(&ax, &ay, &az);
      printf("[2] 加速度: X=%d Y=%d Z=%d\n", ax, ay, az);
      ret = imu_read_temp(&temp);
      printf("[2] 温度: %.1f°C\n", temp);
      printf("[2] IMU OK\n\n");
    }
  else
    {
      printf("[2] IMU 失败: %d\n\n", ret);
    }
#else
  printf("[2] IMU: 未配置\n\n");
#endif

  /* 3. 土壤传感器 */

#ifdef CONFIG_PLANT_SOIL_SENSOR
  printf("[3] 土壤传感器 (RS485)...\n");
  ret = soil_sensor_init(SOIL_SENSOR_DEV_DEFAULT, SOIL_SENSOR_BAUD_DEFAULT);
  if (ret == 0)
    {
      struct soil_data_s data;
      ret = soil_sensor_read(&data, 0);
      printf("[3] 湿度=%.1f%% 温度=%.1f°C EC=%.1f pH=%.1f\n",
             data.moisture, data.temp, data.ec, data.ph);
      printf("[3] 土壤传感器 OK\n\n");
    }
  else
    {
      printf("[3] 土壤传感器失败: %d\n\n", ret);
    }
#else
  printf("[3] 土壤传感器: 未配置\n\n");
#endif

  /* 4. 电池 ADC */

#ifdef CONFIG_PLANT_SYSTEM_MONITOR
  printf("[4] 电池 ADC...\n");
  int mv, pct;
  ret = battery_read_mv(&mv);
  battery_read_pct(&pct);
  printf("[4] 电压=%dmV 电量=%d%%\n", mv, pct);
  printf("[4] 电池 ADC OK\n\n");
#else
  printf("[4] 电池 ADC: 未配置\n\n");
#endif

  /* 5. 摄像头 */

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  printf("[5] 摄像头 (OV3660)...\n");
  ret = camera_init();
  printf("[5] 摄像头 %s\n\n", ret == 0 ? "OK" : "失败");
#else
  printf("[5] 摄像头: 未配置\n\n");
#endif

  /* 6. 语音（I2S + ES8311 + ES7210） */

#ifdef CONFIG_PLANT_AI_VOICE
  printf("[6] 语音 I2S...\n");
  ret = ai_voice_init();
  if (ret == 0)
    {
      printf("[6] 播放测试音 1000Hz...\n");
      ai_voice_play_tone(1000, 500);
      printf("[6] 语音 OK\n\n");
    }
  else
    {
      printf("[6] 语音失败: %d\n\n", ret);
    }
#else
  printf("[6] 语音: 未配置\n\n");
#endif

  /* 7. WiFi */

#ifdef CONFIG_PLANT_WIFI_MANAGER
  printf("[7] WiFi...\n");
  if (wifi_manager_is_connected())
    {
      wifi_status_t status;
      wifi_manager_get_status(&status);
      printf("[7] 已连接 RSSI=%d dBm\n", status.rssi);
    }
  else
    {
      printf("[7] 未连接\n");
    }
  printf("[7] WiFi OK\n\n");
#else
  printf("[7] WiFi: 未配置\n\n");
#endif

  /* 8. SD 卡 */

#ifdef CONFIG_PLANT_SD_CARD
  printf("[8] SD 卡...\n");
  printf("[8] SD 卡: 已配置\n\n");
#else
  printf("[8] SD 卡: 未配置\n\n");
#endif

  printf("========================================\n");
  printf("  演示完成！\n");
  printf("========================================\n\n");

  return 0;
}

/****************************************************************************
 * cmd_font — 中文字体诊断（阶段 A：缺字量化，见 CLAUDE_SYSTEM.md §14）
 *
 * plant font status [文本]：
 *   无文本 → 数据源 + 示例缺字统计
 *   有文本 → 测该文本在当前 20px 字体下缺多少字（方框数）
 * 注：lv_font_get_glyph_dsc 是纯查表（const 数据），无需启动 UI 即可用。
 ****************************************************************************/

#ifdef CONFIG_PLANT_UI
static int cmd_font(int argc, char *argv[])
{
  static const char demo[] =
    "小绿你好，今天浇水了吗？这盆植物看起来需要多晒太阳";
  int miss;

  /* ⚠ 2026-09-10 实测：SD 字库的 FILE* 句柄按任务组隔离（NuttX fd 表按组划分），
   * 在 ui_task 里 open 的句柄只能在 ui_task 里读。本组命令跑在 shell
   * 上下文，所以查 SD 兜底必然 EBADF → 汉字会被误报成"缺"。
   * 要看屏幕上真实的缺字/字宽，看 plant objs（miss）的输出
   * （那条跑在 UI 上下文，与绘制同一个句柄）。 */

  if (argc >= 4 && strcmp(argv[2], "trace") == 0)
    {
      zh_font_init();
      printf("[Font] ⚠ 本命令在 shell 上下文：SD 兜底句柄归 ui_task，"
             "此处查 SD 会误报缺字；\n"
             "       屏幕真实缺字/字宽请看 plant objs 的输出\n");
      zh_font_trace_text(20, argv[3]);
      return 0;
    }

  if (argc >= 4 && strcmp(argv[2], "clean") == 0)
    {
      char out[256];

      zh_font_init();
      zh_text_sanitize(out, sizeof(out), argv[3]);
      printf("[Font] 净化演示: \"%s\" -> \"%s\"\n", argv[3], out);
      printf("[Font] (这里是 shell 上下文，查不到 SD 兜底 → 汉字会被误判成"
             "无字形，本演示主要看 emoji/变体符过滤)\n");
      return 0;
    }

  if (argc >= 4 && strcmp(argv[2], "sd") == 0)
    {
      zh_font_init();
      printf("[Font] ⚠ 同上：shell 上下文读 SD 句柄必 EBADF，"
             "本输出仅看查找流程\n");
      zh_font_diag_sd(zh_font_utf8_first(argv[3]));
      return 0;
    }

  if (argc < 3 || strcmp(argv[2], "status") != 0)
    {
      printf("Usage: plant font status [文本]\n");
      printf("       plant font trace <文本>   逐字打印兜底层级\n");
      printf("       plant font sd <字>       打印 SD 查找过程\n");
      printf("       plant font clean <文本>  演示文本净化（剔无字形字符）\n");
      printf("  ⚠ 上两条跑在 shell 上下文，查 SD 兜底会误报缺字；"
             "屏幕真实缺字/字宽用 plant objs（miss）\n");
      return -1;
    }

  zh_font_init();
  printf("[Font] 字体策略: %s（20px 覆盖 %d 字）\n",
         zh_font_src() == ZH_SRC_FLASH_SUBSET ?
         "仅 flash 子集（SD 兜底未就绪）" :
         "flash 子集主字体 + SD 全量兜底",
         zh_font_count(20));

  if (argc >= 4)
    {
      miss = zh_font_check_text(20, argv[3]);
      printf("[Font] 20px 检查 \"%s\" → 缺 %d 字（显示为方框）\n",
             argv[3], miss);
    }
  else
    {
      miss = zh_font_check_text(20, demo);
      printf("[Font] 示例文本缺字: %d\n", miss);
      printf("[Font] 用法: plant font status <任意中文文本> 可测缺字\n");
    }

  return 0;
}
#endif


/****************************************************************************
 * cmd_nav — 调试：跨线程切换 4-Tab 页面（plant nav <home|data|tasks|diary>）
 *
 * 直接调用 ui_app_nav_to 只是置标志；实际 LVGL 切页由 ui_task 的
 * 500ms 刷新定时器执行（铁律：LVGL 对象只在 ui_task 操作）。这里睡
 * 1.6s 保证切换 + 首帧渲染完成，供后续截屏。
 ****************************************************************************/

static int cmd_nav(int argc, char *argv[])
{
  static const char *const names[4] =
    { "home", "data", "tasks", "diary" };
  int tab = -1;
  int i;

  if (argc < 3)
    {
      printf("Usage: plant nav <home|data|tasks|diary>\n");
      return -1;
    }

  for (i = 0; i < 4; i++)
    {
      if (strcmp(argv[2], names[i]) == 0)
        {
          tab = i;
          break;
        }
    }

  if (tab < 0)
    {
      printf("Usage: plant nav <home|data|tasks|diary>\n");
      return -1;
    }

  printf("[Nav] switch to %s (%d)...\n", names[tab], tab);

  if (ui_app_nav_to(tab) != 0)
    {
      printf("[Nav] rejected (UI not ready)\n");
      return -1;
    }

  /* 等 ui_task 定时器完成切换 + 至少一帧渲染（500ms 粒度 → 1.6s 足够） */

  usleep(1600 * 1000);
  printf("[Nav] %s ready\n", names[tab]);
  return 0;
}

/****************************************************************************
 * cmd_tsim — 调试：模拟真实点按做 Tab 切换压力（plant tsim <cycles>）
 *
 * 在 ui_task 内用额外 LVGL pointer indev 按下/抬起底部 Tab 按钮，完整走
 * LVGL 输入事件管线（含"点击回调里删除当前页面"路径），专用于复现
 * "跑一阵后界面切换卡住（疑似触摸失灵）"。
 ****************************************************************************/

static int cmd_tsim(int argc, char *argv[])
{
  int cycles;
  int timeout_ms;
  int waited;

  if (argc < 3)
    {
      printf("Usage: plant tsim <cycles> | stop | status\n");
      return -1;
    }

  if (strcmp(argv[2], "stop") == 0)
    {
      ui_app_tsim_stop();
      printf("[SynTap] stopped\n");
      return 0;
    }

  if (strcmp(argv[2], "status") == 0)
    {
      printf("[SynTap] left=%d\n", ui_app_tsim_left());
      return 0;
    }

  cycles = atoi(argv[2]);
  if (cycles <= 0)
    {
      printf("Usage: plant tsim <cycles>\n");
      return -1;
    }

  if (ui_app_tsim_start(cycles) != 0)
    {
      printf("[SynTap] start failed (busy?)\n");
      return -1;
    }

  timeout_ms = cycles * 700 + 20000;

  for (waited = 0; waited < timeout_ms; waited += 100)
    {
      if (ui_app_tsim_left() < 0)
        {
          printf("[SynTap] completed cycles=%d\n", cycles);
          return 0;
        }

      usleep(100 * 1000);
    }

  printf("[SynTap] STALL: not finished after %d ms (left=%d)\n",
         timeout_ms, ui_app_tsim_left());
  return -1;
}

/****************************************************************************
 * cmd_tap — 调试：在屏幕坐标注入一次真实点按（plant tap <x> <y>）
 *
 * 走 tsim 那路合成 indev，完整经过 LVGL 输入管线，用来远程进二级页
 * （拍照页等）验证布局，不必手点屏幕。
 ****************************************************************************/

static int cmd_tap(int argc, char *argv[])
{
  int x;
  int y;
  int ret;

  if (argc < 4)
    {
      printf("Usage: plant tap <x> <y>   (480x320)\n");
      return -1;
    }

  x = atoi(argv[2]);
  y = atoi(argv[3]);

  ret = ui_app_tap_at(x, y);
  if (ret != 0)
    {
      printf("[SynTap] tap failed: %d\n", ret);
      return -1;
    }

  /* 等一下让 ui_task 采到按下->抬起（每次 33ms，再留点余量） */

  usleep(300 * 1000);
  printf("[SynTap] tap sent %d,%d\n", x, y);
  return 0;
}

/****************************************************************************
 * cmd_tdiag — 调试：真实触摸双端诊断（plant tdiag [on|off]）
 *
 * GT911 驱动侧计数（polls/down/up/i2c err）在板级 gt911_diag_snapshot()，
 * LVGL 侧 25ms 定时器采样真实 indev 按下次数。on 后每 ~2s 自动打一行，
 * 用于复现"跑一会后点触摸没反应"时区分：驱动/芯片没上报、还是上报了
 * 但 LVGL/应用没消费、还是 ui_task 整体卡死。
 ****************************************************************************/

static int cmd_tdiag(int argc, char *argv[])
{
  if (argc >= 3 && strcmp(argv[2], "on") == 0)
    {
      ui_app_tdiag_ctl(1);
      return 0;
    }

  if (argc >= 3 && strcmp(argv[2], "off") == 0)
    {
      ui_app_tdiag_ctl(0);
      return 0;
    }

  ui_app_tdiag_ctl(2);   /* default: print once */
  return 0;
}

/****************************************************************************
 * cmd_cap — 调试：把当前屏幕整屏截成 BMP（plant cap <名字>）
 *        （输出 /mnt/sd/<名字>.bmp）
 *
 * 实现：从系统堆（PSRAM 8MB 已并入）malloc ~307KB 快照缓冲，请求
 * ui_task 用 LVGL snapshot（lv_snapshot_take_to_draw_buf）渲染当前
 * lv_screen_active 到该缓冲并写 BMP。避免直接读回 LCD GRAM（读回路径
 * 在此板会静默终止任务）。文件为 16bpp RGB565、行序自上而下。
 ****************************************************************************/

#ifdef CONFIG_PLANT_SD_CARD
static int cmd_cap(int argc, char *argv[])
{
  static const uint32_t cap_bufsize = 480 * 320 * 2u + 512u;
  char path[96];
  const char *name;
  uint8_t *buf;
  int ret;
  int i;

  name = (argc >= 3) ? argv[2] : "screen";

  /* 文件名白名单：字母/数字/_（纯调试，防路径注入） */

  for (i = 0; name[i] != '\0' && i < 48; i++)
    {
      char c = name[i];

      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_'))
        {
          printf("[Cap] 非法文件名: %s\n", name);
          return -1;
        }
    }

  if (i == 0)
    {
      printf("[Cap] 非法文件名\n");
      return -1;
    }

  snprintf(path, sizeof(path), "/mnt/sd/%s.bmp", name);

  buf = (uint8_t *)malloc(cap_bufsize);
  if (buf == NULL)
    {
      printf("[Cap] malloc %luB 失败\n", (unsigned long)cap_bufsize);
      return -1;
    }

  printf("[Cap] snapshot → %s ...\n", path);
  fflush(stdout);

  ret = ui_app_capture_to_file(path, buf, cap_bufsize);

  free(buf);

  if (ret != 0)
    {
      printf("[Cap] 失败\n");
      return -1;
    }

  printf("[Cap] OK\n");
  return 0;
}
#endif /* CONFIG_PLANT_SD_CARD */


/****************************************************************************
 * cmd_capa — 调试：把当前屏幕快照转成 ASCII 点阵直接打印到串口
 *        （plant capa [x y w h]，可选窗口，缺省整屏 480x320）
 *
 * 与 cmd_cap 同一 snapshot 机制（LVGL snapshot → 系统堆缓冲），但映射为
 * 字符后从串口打印，不写 SD（大批量写 BMP 在此板会卡死）。输出约
 * 120x160 字符，可直接目检汉字是正常笔画还是"方框/豆腐块"。
 ****************************************************************************/

#ifdef CONFIG_PLANT_UI
static int cmd_capa(int argc, char *argv[])
{
  static const uint32_t cap_bufsize = 480 * 320 * 2u + 512u;
  int x = 0;
  int y = 0;
  int w = 480;
  int h = 320;
  uint8_t *buf;
  int ret;

  if (argc >= 6)
    {
      x = atoi(argv[2]);
      y = atoi(argv[3]);
      w = atoi(argv[4]);
      h = atoi(argv[5]);
    }

  buf = (uint8_t *)malloc(cap_bufsize);
  if (buf == NULL)
    {
      printf("[Capa] malloc %luB 失败\n", (unsigned long)cap_bufsize);
      return -1;
    }

  printf("[Capa] snapshot->ascii region %d,%d %dx%d\n", x, y, w, h);
  fflush(stdout);

  ret = ui_app_capture_ascii(buf, cap_bufsize, x, y, w, h);

  free(buf);

  if (ret != 0)
    {
      printf("[Capa] 失败\n");
      return -1;
    }

  printf("[Capa] OK\n");
  return 0;
}
#endif /* CONFIG_PLANT_UI */


/****************************************************************************
 * cmd_objs — 调试：转储当前屏对象树（label 文本/坐标/字体/码点）
 *        （plant objs）—— 把屏幕上的方框定位到具体 label 的字符。
 ****************************************************************************/

#ifdef CONFIG_PLANT_UI
static int cmd_objs(int argc, char *argv[])
{
  int ret;
  int miss_only = (argc >= 3 && strcmp(argv[2], "miss") == 0);

  printf("[Obj] request object dump%s...\n", miss_only ? " (miss only)" : "");
  fflush(stdout);

  ret = miss_only ? ui_app_dump_missing() : ui_app_dump_objects();

  if (ret != 0)
    {
      printf("[Obj] 失败\n");
      return -1;
    }

  printf("[Obj] OK\n");
  return 0;
}
#endif /* CONFIG_PLANT_UI */


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  /* Version marker */

  printf("\n=== Firmware V%d.%d.%d ===\n\n",
         OTA_FW_VERSION_MAJOR, OTA_FW_VERSION_MINOR, OTA_FW_VERSION_PATCH);

#ifdef CONFIG_PLANT_VOICE_AUTOTEST
  /* 调试专用：开机自跑语音链路（配合 OpenOCD+GDB，无需串口交互）。
   * 跑：1) 播放已知音调（隔离 TX/DAC 链路——音调干净=播放链路 OK，
   *        嘶声在录音数据；音调也嘶=播放链路坏）
   *    2) rate 测试（流结构 dump）
   *    3) 录音 2s → 回放
   * 跑完返回 → nsh 提示符回来，可手动跑 plant voice 命令。 */

  printf("[Autotest] 语音链路自测启动\n");

  printf("[Autotest] 阶段1：播放 440Hz 音调 1s（听：干净=播放链路OK）\n");
  ai_voice_play_tone(440, 1000);

  printf("[Autotest] 阶段2：RX 速率/结构测试\n");
  ai_voice_rx_rate_test(2);
  printf("[Autotest] rate 测试完成，tick=%lu\n",
         (unsigned long)clock_systime_ticks());
  {
    FAR uint8_t *wav = (FAR uint8_t *)malloc(44 + 16000 * 2 * 2);
    int ret;

    if (wav != NULL)
      {
        printf("[Autotest] 阶段3：开始录音，tick=%lu\n",
               (unsigned long)clock_systime_ticks());
        ret = ai_voice_record(wav, 44 + 16000 * 2 * 2, 2);
        printf("[Autotest] 录音返回 ret=%d，tick=%lu\n",
               ret, (unsigned long)clock_systime_ticks());
        if (ret > 0)
          {
            printf("[Autotest] 录音 OK (%d B)，开始回放，tick=%lu\n",
                   ret, (unsigned long)clock_systime_ticks());
            ai_voice_play(wav, (size_t)ret);
            printf("[Autotest] 回放结束，tick=%lu\n",
                   (unsigned long)clock_systime_ticks());
          }

        free(wav);
      }
  }

  printf("[Autotest] 自测结束，回到 nsh（可跑 plant voice tone / loop）\n");
  return 0;
#endif

#ifdef CONFIG_PLANT_UI_PANEL
  lcd_init();
#endif

#ifdef CONFIG_PLANT_SD_CARD
  /* Boot-time auto-mount (non-fatal when no card is present) */

  sd_card_init();
#endif

#ifdef CONFIG_PLANT_WIFI_MANAGER
  if (argc >= 2 && strcmp(argv[1], "wifi") == 0)
    {
      return cmd_wifi(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_AI_MODULE
  if (argc >= 2 && strcmp(argv[1], "ai") == 0)
    {
      return cmd_ai(argc, argv);
    }

  if (argc >= 2 && strcmp(argv[1], "srv") == 0)
    {
      return cmd_srv(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_AI_VOICE
  if (argc >= 2 && strcmp(argv[1], "voice") == 0)
    {
      return cmd_voice(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_SOIL_SENSOR
  if (argc >= 2 && strcmp(argv[1], "soil") == 0)
    {
      return cmd_soil(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  if (argc >= 2 && strcmp(argv[1], "cam") == 0)
    {
      return cmd_cam(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_SD_CARD
  if (argc >= 2 && strcmp(argv[1], "sd") == 0)
    {
      return cmd_sd(argc, argv);
    }

  if (argc >= 2 && strcmp(argv[1], "cfg") == 0)
    {
      return cmd_cfg(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_UI
  if (argc >= 2 && strcmp(argv[1], "img") == 0)
    {
      extern void ui_app_show_image(const uint8_t *rgb565, int w, int h);
      extern void ui_app_hide_image(void);

      if (argc >= 3 && strcmp(argv[2], "hide") == 0)
        {
          ui_app_hide_image();
          printf("[Img] test image hidden\n");
          return 0;
        }

      /* 目标缓冲：复用摄像头驱动静态 rxbuf（160×120×2=38400），
       * 摄像头不在运行，缓冲区空闲且生命周期稳定 */

      extern uint8_t *esp32s3_cam_dvp_get_frame(void);
      uint8_t *fb = esp32s3_cam_dvp_get_frame();
      int x;
      int y;

      if (argc >= 3 && strcmp(argv[2], "test") == 0)
        {
          /* 四象限色块：红/绿/蓝/白 —— 一眼验证 LCD 颜色通道。
           * 直接写 LCD 全屏（软件缩放，绕过 LVGL lv_image 渲染 bug） */

          for (y = 0; y < 120; y++)
            {
              for (x = 0; x < 160; x++)
                {
                  uint16_t v;

                  if (x < 80 && y < 60)
                    {
                      v = 0xf800;       /* 红 */
                    }
                  else if (x >= 80 && y < 60)
                    {
                      v = 0x07e0;       /* 绿 */
                    }
                  else if (x < 80 && y >= 60)
                    {
                      v = 0x001f;       /* 蓝 */
                    }
                  else
                    {
                      v = 0xffff;       /* 白 */
                    }

                  fb[y * 320 + x * 2] = v & 0xff;
                  fb[y * 320 + x * 2 + 1] = v >> 8;
                }
            }

          lcd_put_rgb565(fb, 160, 120, 0, 0, 480, 320);
          printf("[Img] test pattern -> LCD direct (R/G/B/W quadrants)\n");
          return 0;
        }

      if (argc >= 4)
        {
          /* plant img show <file.raw>：从 SD 读 160×120 RGB565 直接写 LCD */

          FILE *fp = fopen(argv[3], "rb");
          size_t n;

          if (fp == NULL)
            {
              printf("[Img] open %s failed\n", argv[3]);
              return -1;
            }

          n = fread(fb, 1, 38400, fp);
          fclose(fp);

          if (n != 38400)
            {
              printf("[Img] read %zu bytes (want 38400) — file must be "
                     "160x120 RGB565 raw\n", n);
              return -1;
            }

          lcd_put_rgb565(fb, 160, 120, 0, 0, 480, 320);
          printf("[Img] showing %s -> LCD direct (160x120 RGB565)\n", argv[3]);
          return 0;
        }

      printf("Usage: plant img test | show <file.raw> | hide\n");
      return -1;
    }
#endif


#ifdef CONFIG_PLANT_UI
  if (argc >= 2 && strcmp(argv[1], "tsim") == 0)
    {
      return cmd_tsim(argc, argv);
    }
#endif
#ifdef CONFIG_PLANT_UI
  if (argc >= 2 && strcmp(argv[1], "nav") == 0)
    {
      return cmd_nav(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_UI
  if (argc >= 2 && strcmp(argv[1], "tap") == 0)
    {
      return cmd_tap(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_UI
  if (argc >= 2 && strcmp(argv[1], "tdiag") == 0)
    {
      return cmd_tdiag(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_UI
  if (argc >= 2 && strcmp(argv[1], "memlog") == 0)
    {
      return cmd_memlog(argc, argv);
    }
#endif

#if defined(CONFIG_PLANT_UI) && defined(CONFIG_PLANT_SD_CARD)
  if (argc >= 2 && strcmp(argv[1], "cap") == 0)
    {
      return cmd_cap(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_UI
  if (argc >= 2 && strcmp(argv[1], "capa") == 0)
    {
      return cmd_capa(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_UI
  if (argc >= 2 && strcmp(argv[1], "objs") == 0)
    {
      return cmd_objs(argc, argv);
    }
#endif

#ifdef CONFIG_ESP32S3_WIFI
  if (argc >= 2 && strcmp(argv[1], "ota") == 0)
    {
      return cmd_ota(argc, argv);
    }
#endif

#ifdef CONFIG_PLANT_UI
  if (argc >= 2 && strcmp(argv[1], "font") == 0)
    {
      return cmd_font(argc, argv);
    }
#endif

  if (argc >= 2 && strcmp(argv[1], "demo") == 0)
    {
      return cmd_demo(argc, argv);
    }

  if (argc >= 2 && strcmp(argv[1], "mem") == 0)
    {
      return cmd_mem(argc, argv);
    }

#ifdef CONFIG_PLANT_UI
  /* 产品模式：无参数（或 plant ui）→ 开机自启 UI 首屏。
   * 所有子命令（wifi/ai/voice/soil/sd/ota/demo）优先于 UI，
   * 供 NSH 串口调试使用。
   */

  if (argc < 2 || strcmp(argv[1], "ui") == 0)
    {
#if defined(CONFIG_PLANT_SD_CARD) && defined(CONFIG_PLANT_WIFI_MANAGER)
      /* 开机后台自动恢复：读 SD plant.cfg → 连 WiFi → 配服务器 */
      boot_cfg_start();
#endif
      return ui_app_start();
    }
#endif

  printf("Plant Companion — AI Smart Plant Care\n\n");
  printf("Usage: plant [ui|command]\n\n");
  printf("  (no args / ui)            Start product-mode UI (boot screen)\n");
  printf("Commands:\n");
#ifdef CONFIG_PLANT_WIFI_MANAGER
  printf("  wifi  <ssid> [password]     Connect to WiFi\n");
#endif
#ifdef CONFIG_PLANT_AI_MODULE
  printf("  ai    test                  Test AI connectivity\n");
  printf("  ai    advise <8 params>     Get AI plant advice\n");
#endif
#ifdef CONFIG_PLANT_AI_VOICE
  printf("  voice tone <freq>           Play a test tone\n");
  printf("  voice rec [seconds]         Record mic -> WAV (16 kHz)\n");
  printf("  voice diag [seconds]        A/B quiet-vs-speech stats (root-cause)\n");
  printf("  voice raw [seconds]         Raw 24k RX stream dump (frame check)\n");
  printf("  voice loop [seconds]        Record then play back\n");
#endif
#ifdef CONFIG_PLANT_SOIL_SENSOR
  printf("  soil  read                  Read soil sensor data\n");
#endif
#ifdef CONFIG_PLANT_SD_CARD
  printf("  sd    status                 Show SD mount status\n");
  printf("  sd    test                   SD card read/write test\n");
  printf("  sd    mount                  Mount SD card to /mnt/sd\n");
  printf("  sd    umount                 Unmount SD card\n");
  printf("  cfg   save|show|connect|clear  开机配置存 SD/恢复（防重启丢配置）\n");
#endif
  printf("  demo                        Run full peripheral demo\n");
  printf("  mem                         Memory/storage diagnostics\n");
#ifdef CONFIG_ESP32S3_WIFI
  printf("  ota   check                 Check for firmware update (A/B switch)\n");
  printf("  ota   stage                 Download + verify only, no switch\n");
  printf("  ota   status                Show OTA status\n");
  printf("  ota   confirm               Mark current slot VALID (cancel rollback)\n");
#endif
#ifdef CONFIG_PLANT_UI
  printf("  font  status [文本]         中文字体缺字诊断（CLAUDE_SYSTEM §14）\n");
  printf("  font  trace <文本>        逐字打印字形来源（主字体/SD/符号/ 缺）\n");
  printf("  font  sd <字>            打印 SD 兜底查找过程（fseek/fread/errno）\n");
  printf("  objs                        转储当前屏对象树（方框定位）\n");
  printf("  tap   <x> <y>           注入一次点按（远程进二级页）\n");
  printf("  memlog on|off               切页完成时打印堆余量（默认关）\n");
#endif

  return 0;
}
