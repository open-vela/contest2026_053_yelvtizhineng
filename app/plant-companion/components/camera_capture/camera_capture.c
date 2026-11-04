/****************************************************************************
 * apps/plant-companion/components/camera_capture/camera_capture.c
 *
 * OV3660 DVP camera — SCCB probe + JPEG 配置接口
 *
 * 参考：ESP-IDF esp32-camera 官方 ov3660.c（寄存器序列照抄自官方，
 * 保证 JPEG 320×240 输出时序正确）。
 *
 * 阶段一目标：拍一帧 JPEG → 喂 ai_image_analyze()（真实 MiMo 视觉）。
 * 帧采集（LCD_CAM DMA）后续实现；当前先完成传感器配置部分。
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>
#include "../../hal/hal_camera.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>   /* malloc/free */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>   /* usleep */
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/clock.h>

#include "camera_capture.h"
#ifdef CONFIG_PLANT_UI
#include "../../ui/ui_app.h"
#endif
#include "../system_monitor/user_preferences.h"
#include "tjpgd.h"
#include "../../services/sd_write.h"

/* board 层导出：SCCB 读写 + XMCLK + 探测 */

int board_camera_sccb_probe(uint16_t *pid);
int board_camera_sccb_write(uint16_t reg, uint8_t val);
int board_camera_sccb_read(uint16_t reg, uint8_t *val);

/* 驱动层导出：帧几何运行时切换（预览 160×120 / 拍照 320×240） */

#ifdef CONFIG_ESP32S3_CAM_DVP
int esp32s3_cam_dvp_set_framesize(uint32_t w, uint32_t h);
void esp32s3_cam_dvp_get_framesize(uint32_t *w, uint32_t *h);
void esp32s3_cam_dvp_set_jpeg(bool on);
void esp32s3_cam_dvp_set_stream_direct(bool on);
uint8_t *esp32s3_cam_dvp_get_shadow(void);
size_t esp32s3_cam_dvp_get_shadow_size(void);
void esp32s3_cam_dvp_invalidate(const void *data, size_t len);
#endif

/* PSRAM 静态区分配：与摄像头 DMA 缓冲同一套保留区，不能进普通堆。 */

void *esp32s3_psram_static_alloc(size_t size);

/****************************************************************************
 * 寄存器地址（官方 ov3660_regs.h）
 ****************************************************************************/

#define OV3660_SYSTEM_CTROL0   0x3008   /* 软复位 */
#define OV3660_SC_PLLS_CTRL0   0x303a
#define OV3660_SC_PLLS_CTRL1   0x303b
#define OV3660_SC_PLLS_CTRL2   0x303c
#define OV3660_SC_PLLS_CTRL3   0x303d
#define OV3660_PCLK_RATIO      0x3824
#define OV3660_VFIFO_CTRL0C    0x460c
#define OV3660_JPEG_QS         0x04
#define OV3660_FORMAT_CTRL     0x501f
#define OV3660_FORMAT_CTRL00   0x4300
#define OV3660_TIMING_TC_REG20 0x3820
#define OV3660_TIMING_TC_REG21 0x3821
#define OV3660_X_ADDR_ST_H     0x3800
#define OV3660_X_ADDR_END_H    0x3804
#define OV3660_X_OUTPUT_SIZE_H 0x3808
#define OV3660_X_TOTAL_SIZE_H  0x380c
#define OV3660_X_OFFSET_H      0x3810
#define OV3660_X_INCREMENT     0x3814
#define OV3660_Y_INCREMENT     0x3815
#define OV3660_ISP_CONTROL_01  0x5001
#define OV3660_IMAGE_OPTION    0x4514

/* Keep exposure compensation across preview/JPEG mode changes. */
static int g_camera_ae_level = 5;
static bool g_camera_flipped;

#define REGLIST_TAIL 0x0000
#define REG_DLY      0xffff   /* 官方 ov3660_settings.h：延时标记 */

/****************************************************************************
 * 官方默认寄存器序列（ov3660_settings.h sensor_default_regs，215 条，逐条照抄）
 *
 * 2026-08-28 修正（花屏"暗+绿"根因，对照官方逐条 diff 发现）：
 *   1. 结尾补 {0x5001, 0x83} —— 开启 AWB/色彩矩阵/SDE（此前完全没写 0x5001，
 *      AWB 默认关 → 白墙偏绿）
 *   2. 补全 gamma 表缺失的 13 个点（0x5800/0x5802/0x5803/0x5804/0x580b/0x5811/
 *      0x5817/0x581c/0x581d/0x581f/0x5820/0x5823/0x583d）——gamma 点默认 0x00
 *      会把图像压暗
 *   3. 移除误加的 JPEG 专用尾 {0x3002,0x00} {0x3006,0xff} {0x471c,0x50}
 *      （这些只在 fmt_jpeg 序列里；默认序列应为 0x471c=0xd0）
 * 开头软复位时序（0x82 → REG_DLY 10ms → 0x42）与 0x4740 PCLK 极性保留。
 ****************************************************************************/

static const uint16_t ov3660_default_regs[][2] = {
  {0x3008, 0x82},
  {REG_DLY, 10},    /* delay 10ms after soft reset - CRITICAL */
  {0x3103, 0x13},
  {0x3008, 0x42},
  {0x3017, 0xff},
  {0x3018, 0xff},
  {0x302c, 0xc3},
  {0x4740, 0x21},
  {0x3611, 0x01},
  {0x3612, 0x2d},
  {0x3032, 0x00},
  {0x3614, 0x80},
  {0x3618, 0x00},
  {0x3619, 0x75},
  {0x3622, 0x80},
  {0x3623, 0x00},
  {0x3624, 0x03},
  {0x3630, 0x52},
  {0x3632, 0x07},
  {0x3633, 0xd2},
  {0x3704, 0x80},
  {0x3708, 0x66},
  {0x3709, 0x12},
  {0x370b, 0x12},
  {0x3717, 0x00},
  {0x371b, 0x60},
  {0x371c, 0x00},
  {0x3901, 0x13},
  {0x3600, 0x08},
  {0x3620, 0x43},
  {0x3702, 0x20},
  {0x3739, 0x48},
  {0x3730, 0x20},
  {0x370c, 0x0c},
  {0x3a18, 0x00},
  {0x3a19, 0xf8},
  {0x3000, 0x10},
  /* v132：0x3004 bit2 = 0 —— 关闭 50/60Hz 自动检测（否则传感器会
   * 持续检测灯光频率并据此动态改变曝光约束 → 行/帧时序微变 → 采集
   * 画面呈"节拍式"漂移）。灯光频率改为下方固定 50Hz。 */
  {0x3004, 0xeb},
  {0x6700, 0x05},
  {0x6701, 0x19},
  {0x6702, 0xfd},
  {0x6703, 0xd1},
  {0x6704, 0xff},
  {0x6705, 0xff},
  {0x3c01, 0x80},
  {0x3c00, 0x04},
  {0x3a08, 0x00},
  {0x3a09, 0x62},
  {0x3a0e, 0x08},
  {0x3a0a, 0x00},
  {0x3a0b, 0x52},
  {0x3a0d, 0x09},
  {0x3a00, 0x3a},
  {0x3a14, 0x09},
  {0x3a15, 0x30},
  {0x3a02, 0x09},
  {0x3a03, 0x30},
  {0x440e, 0x08},
  {0x4520, 0x0b},
  {0x460b, 0x37},
  {0x4713, 0x02},
  {0x471c, 0xd0},
  {0x5086, 0x00},
  {0x5002, 0x00},
  {0x501f, 0x00},
  {0x3008, 0x02},
  {0x5180, 0xff},
  {0x5181, 0xf2},
  {0x5182, 0x00},
  {0x5183, 0x14},
  {0x5184, 0x25},
  {0x5185, 0x24},
  {0x5186, 0x16},
  {0x5187, 0x16},
  {0x5188, 0x16},
  {0x5189, 0x68},
  {0x518a, 0x60},
  {0x518b, 0xe0},
  {0x518c, 0xb2},
  {0x518d, 0x42},
  {0x518e, 0x35},
  {0x518f, 0x56},
  {0x5190, 0x56},
  {0x5191, 0xf8},
  {0x5192, 0x04},
  {0x5193, 0x70},
  {0x5194, 0xf0},
  {0x5195, 0xf0},
  {0x5196, 0x03},
  {0x5197, 0x01},
  {0x5198, 0x04},
  {0x5199, 0x12},
  {0x519a, 0x04},
  {0x519b, 0x00},
  {0x519c, 0x06},
  {0x519d, 0x82},
  {0x519e, 0x38},
  {0x5381, 0x1d},
  {0x5382, 0x60},
  {0x5383, 0x03},
  {0x5384, 0x0c},
  {0x5385, 0x78},
  {0x5386, 0x84},
  {0x5387, 0x7d},
  {0x5388, 0x6b},
  {0x5389, 0x12},
  {0x538a, 0x01},
  {0x538b, 0x98},
  {0x5480, 0x01},
  {0x5000, 0xa7},
  {0x5800, 0x0c},
  {0x5801, 0x09},
  {0x5802, 0x0c},
  {0x5803, 0x0c},
  {0x5804, 0x0d},
  {0x5805, 0x17},
  {0x5806, 0x06},
  {0x5807, 0x05},
  {0x5808, 0x04},
  {0x5809, 0x06},
  {0x580a, 0x09},
  {0x580b, 0x0e},
  {0x580c, 0x05},
  {0x580d, 0x01},
  {0x580e, 0x01},
  {0x580f, 0x01},
  {0x5810, 0x05},
  {0x5811, 0x0d},
  {0x5812, 0x05},
  {0x5813, 0x01},
  {0x5814, 0x01},
  {0x5815, 0x01},
  {0x5816, 0x05},
  {0x5817, 0x0d},
  {0x5818, 0x08},
  {0x5819, 0x06},
  {0x581a, 0x05},
  {0x581b, 0x07},
  {0x581c, 0x0b},
  {0x581d, 0x0d},
  {0x581e, 0x12},
  {0x581f, 0x0d},
  {0x5820, 0x0e},
  {0x5821, 0x10},
  {0x5822, 0x10},
  {0x5823, 0x1e},
  {0x5824, 0x53},
  {0x5825, 0x15},
  {0x5826, 0x05},
  {0x5827, 0x14},
  {0x5828, 0x54},
  {0x5829, 0x25},
  {0x582a, 0x33},
  {0x582b, 0x33},
  {0x582c, 0x34},
  {0x582d, 0x16},
  {0x582e, 0x24},
  {0x582f, 0x41},
  {0x5830, 0x50},
  {0x5831, 0x42},
  {0x5832, 0x15},
  {0x5833, 0x25},
  {0x5834, 0x34},
  {0x5835, 0x33},
  {0x5836, 0x24},
  {0x5837, 0x26},
  {0x5838, 0x54},
  {0x5839, 0x25},
  {0x583a, 0x15},
  {0x583b, 0x25},
  {0x583c, 0x53},
  {0x583d, 0xcf},
  {0x3a0f, 0x30},
  {0x3a10, 0x28},
  {0x3a1b, 0x30},
  {0x3a1e, 0x28},
  {0x3a11, 0x60},
  {0x3a1f, 0x14},
  {0x5302, 0x28},
  {0x5303, 0x20},
  {0x5306, 0x1c},
  {0x5307, 0x28},
  {0x4002, 0xc5},
  {0x4003, 0x81},
  {0x4005, 0x12},
  {0x5688, 0x11},
  {0x5689, 0x11},
  {0x568a, 0x11},
  {0x568b, 0x11},
  {0x568c, 0x11},
  {0x568d, 0x11},
  {0x568e, 0x11},
  {0x568f, 0x11},
  {0x5580, 0x06},
  {0x5588, 0x00},
  {0x5583, 0x40},
  {0x5584, 0x2c},
  {0x5001, 0x83},
  {REGLIST_TAIL, 0x00},
};

/* 官方 JPEG 格式序列 */

static const uint16_t ov3660_fmt_jpeg[][2] = {
  {OV3660_FORMAT_CTRL, 0x00},      /* YUV422 */
  {OV3660_FORMAT_CTRL00, 0x30},    /* YUYV */
  {0x4407, OV3660_JPEG_QS},        /* finer JPEG quantization scale */
  {0x3002, 0x00},
  {0x3006, 0xff},
  {0x471c, 0x50},
  {REGLIST_TAIL, 0x00},
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ov3660_write_regs(const uint16_t (*regs)[2])
{
  int i;

  for (i = 0; regs[i][0] != REGLIST_TAIL; i++)
    {
      if (regs[i][0] == REG_DLY)
        {
          /* 官方 write_regs：REG_DLY 项 = 延时 regs[i][1] ms */

          usleep(regs[i][1] * 1000);
          continue;
        }

      int ret = board_camera_sccb_write(regs[i][0], regs[i][1]);
      if (ret < 0)
        {
          printf("[Cam] SCCB write 0x%04x=0x%02x failed: %d\n",
                 regs[i][0], regs[i][1], ret);
          return ret;
        }
    }

  return 0;
}

static int ov3660_write_reg16(uint16_t reg, uint16_t value)
{
  int ret;

  ret = board_camera_sccb_write(reg, (uint8_t)(value >> 8));
  if (ret < 0) return ret;
  return board_camera_sccb_write(reg + 1, (uint8_t)(value & 0xff));
}

static int ov3660_write_addr_reg(uint16_t reg, uint16_t x, uint16_t y)
{
  int ret;

  ret = ov3660_write_reg16(reg, x);
  if (ret < 0) return ret;
  return ov3660_write_reg16(reg + 2, y);
}

static int ov3660_write_reg_bits(uint16_t reg, uint8_t mask, bool enable)
{
  uint8_t val;
  int ret;

  ret = board_camera_sccb_read(reg, &val);
  if (ret < 0) return ret;

  if (enable)
    {
      val |= mask;
    }
  else
    {
      val &= ~mask;
    }

  return board_camera_sccb_write(reg, val);
}

/* ==========================================================================
 * 以下全部照抄官方 esp32-camera ov3660.c（sensors/ov3660.c）：
 *   - set_framesize()      第 311-379 行
 *   - set_image_options()  第 242-303 行
 *   - set_pll()            第 150-179 行
 * 仅将 sensor_t / write_reg 适配为 board_camera_sccb_write。
 * ========================================================================== */

/* ratio_table[0] = 4x3（官方 ov3660_settings.h 第 9-20 行）
 *   {max_w, max_h, start_x, start_y, end_x, end_y, off_x, off_y, total_x, total_y}
 *   { 2048, 1536,  0, 0, 2079, 1547, 16, 6, 2300, 1564 } */

struct ov3660_ratio_s
{
  uint16_t max_w, max_h;
  uint16_t start_x, start_y;
  uint16_t end_x, end_y;
  uint16_t off_x, off_y;
  uint16_t total_x, total_y;
};

static const struct ov3660_ratio_s g_ov3660_ratio4x3 =
{
  2048, 1536, 0, 0, 2079, 1547, 16, 6, 2300, 1564
};

/* 最近一次 ov3660_set_framesize 实际选用的参数：写后读回校验必须按当前
 * 模式对值（原来写死 0x303b=0x08 / 0x3824=0x0a，切到 320×240 会误报
 * MISMATCH）。 */

static uint8_t g_pll_mul = 8;
static uint8_t g_pll_div = 10;
static bool    g_pll_binning = true;

/* 传感器当前几何模式（避免每次进出拍照页都做一次 SCCB 切换） */

enum cam_mode_e
{
  CAM_MODE_NONE = 0,
  CAM_MODE_PREVIEW,   /* 160×120：预览优先快 */
  CAM_MODE_PHOTO      /* 320×240：上传优先清 */
};

static int  s_sensor_mode;
static bool s_rgb565_ready;   /* 已跑过完整 RGB565 初始化序列 */
static bool s_rgb_hd_ready;   /* 640x480 RGB 已配置（切模式/shutdown 复位） */
static bool s_rgb_qvga_ready; /* 320x240 RGB 取景已配置（撕裂优化：1:1 直写） */
static bool s_cam_hl_desat;   /* 高光去饱和：仅诊断 A/B，正式链路默认关 */

/* set_image_options（官方第 242-303 行）。
 * JPEG 模式：reg21 bit5=1（compression enable）。
 * binning 由调用者传入（set_framesize 计算）。 */

static int ov3660_set_image_options(bool binning, bool jpeg)
{
  uint8_t reg20 = 0;
  uint8_t reg21 = 0;
  uint8_t reg4514 = 0;
  uint8_t reg4514_test = 0;
  int ret;

  if (jpeg)
    {
      reg21 |= 0x20;
    }

  if (binning)
    {
      reg20 |= 0x01;
      reg21 |= 0x01;
      reg4514_test |= 4;
    }
  else
    {
      reg20 |= 0x40;
    }

  /* Both bits and 0x4514 must follow the official image-option table. */
  if (g_camera_flipped)
    {
      reg20 |= 0x06;
      reg21 |= 0x06;
      reg4514_test |= 3;
    }

  switch (reg4514_test)
    {
      case 0: reg4514 = 0x88; break;   /* no binning, normal */
      case 3: reg4514 = 0xbb; break;   /* no binning, both axes */
      case 4: reg4514 = 0xaa; break;   /* binning, normal */
      case 7: reg4514 = 0xaa; break;   /* binning, both axes */
      default: reg4514 = 0xaa; break;
    }

  ret = board_camera_sccb_write(OV3660_TIMING_TC_REG20, reg20);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(OV3660_TIMING_TC_REG21, reg21);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(OV3660_IMAGE_OPTION, reg4514);
  if (ret < 0) return ret;

  if (binning)
    {
      ret = board_camera_sccb_write(0x4520, 0x0b);   /* binning */
      if (ret < 0) return ret;
      ret = board_camera_sccb_write(OV3660_X_INCREMENT, 0x31); /* odd:3 even:1 */
      if (ret < 0) return ret;
      ret = board_camera_sccb_write(OV3660_Y_INCREMENT, 0x31);
      if (ret < 0) return ret;
    }
  else
    {
      ret = board_camera_sccb_write(0x4520, 0xb0);
      if (ret < 0) return ret;
      ret = board_camera_sccb_write(OV3660_X_INCREMENT, 0x11); /* odd:1 even:1 */
      if (ret < 0) return ret;
      ret = board_camera_sccb_write(OV3660_Y_INCREMENT, 0x11);
      if (ret < 0) return ret;
    }

  return 0;
}

/* set_pll（官方第 150-179 行） */

static int ov3660_set_pll(bool bypass, uint8_t multiplier, uint8_t sys_div,
                          uint8_t pre_div, bool root_2x, uint8_t seld5,
                          bool pclk_manual, uint8_t pclk_div)
{
  int ret;

  if (multiplier > 31 || sys_div > 15 || pre_div > 3 ||
      pclk_div > 31 || seld5 > 3)
    {
      printf("[Cam] set_pll invalid args\n");
      return -EINVAL;
    }

  ret = board_camera_sccb_write(OV3660_SC_PLLS_CTRL0, bypass ? 0x80 : 0x00);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(OV3660_SC_PLLS_CTRL1, multiplier & 0x1f);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(OV3660_SC_PLLS_CTRL2, 0x10 | (sys_div & 0x0f));
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(OV3660_SC_PLLS_CTRL3,
                                ((pre_div & 0x3) << 4) | seld5 |
                                (root_2x ? 0x40 : 0x00));
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(OV3660_PCLK_RATIO, pclk_div & 0x1f);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(OV3660_VFIFO_CTRL0C,
                                pclk_manual ? 0x22 : 0x20);
  if (ret < 0) return ret;

  return 0;
}

/* set_framesize（官方第 311-379 行）：w/h 直接传分辨率，
 * 由 binning 规则判断用哪种 total/offset。 */

static int ov3660_set_framesize(uint16_t w, uint16_t h, bool jpeg)
{
  const struct ov3660_ratio_s *s = &g_ov3660_ratio4x3;
  bool binning;
  bool scale;
  int ret;

  /* binning = w <= max_w/2 && h <= max_h/2（官方第 326-328 行） */

  binning = (w <= (s->max_w / 2) && h <= (s->max_h / 2));


  /* scale = 不是最大尺寸也不是半尺寸（官方第 327-328 行） */

  scale = !((w == s->max_w && h == s->max_h) ||
            (w == (s->max_w / 2) && h == (s->max_h / 2)));

  /* 窗口寄存器（官方第 330-332 行） */

  ret = ov3660_write_addr_reg(OV3660_X_ADDR_ST_H, s->start_x, s->start_y);
  if (ret < 0) return ret;
  ret = ov3660_write_addr_reg(OV3660_X_ADDR_END_H, s->end_x, s->end_y);
  if (ret < 0) return ret;
  ret = ov3660_write_addr_reg(OV3660_X_OUTPUT_SIZE_H, w, h);
  if (ret < 0) return ret;

  /* total/offset 按 binning 分（官方第 334-343 行） */

  if (binning)
    {
      /* 取景器刷新率改造：binning 行有效像素只有 1024，官方 HTS=2300
       * 带来 125% 行消隐，是帧率天花板主因（VTS 已是 768+15 极限）。
       * HTS 收窄到 1600（消隐仍有 56%），16MHz PCLK 下帧长
       * 112ms -> 78ms（8.9 -> 12.8fps 上限）。行周期变短会改变
       * 50Hz _band_滤波的行计数，人工光源下取景可能有轻微条纹
       * （仅取景；2048 快门帧走 QXGA 官方时序不受影响）。
       *
       * ⚠️ 2026-09-27 根因修正：收窄 HTS【仅限 RGB565 取景】！
       * JPEG binning 用 HTS=1600 会杀死传感器的 JPEG 输出链路
       * （VSYNC 死、DMA 间歇性零字节/全 0xFF 垃圾 —— 困扰两天的
       * "冷启动 JPEG 锁死"真凶）；jpegq 活体实测 HTS 回到官方
       * 2300 后首抓即成（50760B 完整帧）。JPEG 分支必须 2300。 */

      /* 2026-09-28 (v69)：RGB 取景 VTS 同步收窄。实测 HTS=1600/VTS=783
       * 时帧周期 136ms（传感器上限 7.3fps），垂直消隐占 75%。
       * VTS = h + 80（640x480 -> 560，帧 97ms，10.3fps）；
       * AEC 曝光上限相应收窄（弱光靠增益补，取景可接受）。
       * JPEG 分支保持官方 total（动它会杀死 JPEG 输出链路）。 */

      /* 2026-09-28 (v70/v71 试错回退)：VTS 收窄（560）与 HTS 收窄
       * （1280）均导致传感器停流（capture -110）——binning+scale 模式
       * 下 VTS=783 已是 768 行内部读出极限，HTS<1600 传感器拒绝。
       * RGB 取景帧周期 136ms（7.3fps）即传感器硬上限，维持 1600。 */

      ret = ov3660_write_addr_reg(OV3660_X_TOTAL_SIZE_H,
                                  jpeg ? s->total_x : 1600,
                                  (s->total_y / 2) + 1);
      if (ret < 0) return ret;
      ret = ov3660_write_addr_reg(OV3660_X_OFFSET_H, 8, 2);
      if (ret < 0) return ret;
    }
  else
    {
      ret = ov3660_write_addr_reg(OV3660_X_TOTAL_SIZE_H, s->total_x,
                                  s->total_y);
      if (ret < 0) return ret;
      ret = ov3660_write_addr_reg(OV3660_X_OFFSET_H, 16, 6);
      if (ret < 0) return ret;
    }

  /* ISP_CONTROL_01 0x20 = scale 使能（官方第 345-346 行） */

  ret = ov3660_write_reg_bits(OV3660_ISP_CONTROL_01, 0x20, scale);
  if (ret < 0) return ret;

  /* set_image_options（官方第 348-350 行） */

  ret = ov3660_set_image_options(binning, jpeg);
  if (ret < 0) return ret;

  /* 记下本次选的参数（供写后读回校验） */

  g_pll_binning = binning;

  /* PLL（官方第 360-378 行）：非 JPEG 分支，官方值 tuned for 16MHz XCLK。
   * 本板 XMCLK = 20MHz（board 层 LEDC），PCLK 会按 1.25× 上偏：
   *   calc_sysclk(20M, M=8, sys=1, pre=0, root2x=0, seld5=0, pclk=8)
   *     → VCO=160M PLLCLK=160M PCLK=10M（应为 8M）→ 帧率偏快 1.25× →
   *     AEC 曝光积分时序错位 → 曝光/白平衡反复振荡（实测帧值每拍不同）
   * 修正：pclk_div 8→10 → PCLK = 160M/2/10 = 8MHz（与官方调优一致）*/

  if (jpeg)
    {
      /* Official esp32-camera JPEG PLL for 20MHz XCLK:
       * 40MHz SYSCLK / 10MHz PCLK（pclk_div=8）。
       * 2048x1536 帧尾 MCU 截断是输出带宽竞态：PCLK 翻倍（pclk_div=4）
       * 后实测尾部缺失从数十 MCU 收敛到 0~10 个（配合 0x4400/0x4401/
       * 0x4722 提速与 HD 环路解码重抓）。pclk_div=2 会出现接收丢字节
       * （信号完整性），勿再降。1024x768 等小帧保持官方 8 不动，
       * 避免动到已调好的曝光时序。 */

      /* 2026-09-28 (v58): 1024x768 的 PCLK 从 10MHz 降到 8MHz。
       * 实测 ~50% 帧中段熵流出现非法字节（如 FFAB，非节点边界，
       * 块无重复/无空洞）——PCLK 越高，PSRAM 带宽竞争 + 排线信号
       * 完整性越临界，AFIFO 瞬间排不干就丢字节，Huffman 流即崩。
       * 8MHz 帧周期 180->225ms（5.5->4.4fps），换取完整率。 */

      uint8_t pclk_div = (w == 2048 && h == 1536) ? 4 : 10;

      ret = ov3660_set_pll(false, 24, 1, 3, false, 0, true, pclk_div);
      g_pll_mul = 24;
      g_pll_div = pclk_div;
    }
  else if (w * h <= 160 * 120)
    {
      /* framesize < QVGA：40M SYSCLK / 8MHz PCLK（20MHz XCLK 下用 pclk=10） */

      ret = ov3660_set_pll(false, 8, 1, 0, false, 0, true, 10);
      g_pll_mul = 8;
      g_pll_div = 10;
    }
  else if (w * h <= 320 * 240)
    {
      /* framesize == QVGA：20M SYSCLK，PCLK 8->16MHz（pclk_div 4->2）。
       * 取景器刷新率改造（目标 ~10Hz）：QVGA binning 帧长
       * 2300x783 PCLK，8MHz 时 225ms（4.4fps）是取景硬上限；
       * 16MHz 后 112ms（8.9fps）。只影响 320x240 RGB565 路径，
       * AEC 闭环自适应（曝光上限减半，弱光靠增益补）。 */

      ret = ov3660_set_pll(false, 16, 5, 0, false, 0, true, 2);
      g_pll_mul = 16;
      g_pll_div = 2;
    }
  else
    {
      /* framesize >= HVGA（640x480 取景）：官方值按 16MHz XCLK 调优，
       * 本板 XCLK 20MHz 会使 PCLK 上偏 1.25 倍 → AEC/AWB 积分时序错位
       * → 曝光/白平衡反复振荡（aligncheck 实测 sad0 145-220、60-93% 行
       * 帧间大差异）。按 1.25 倍校正：pclk_div 2 → 3（≈6.7MHz，官方
       * 等效 6.4MHz），与 QVGA 分支 8→10 的校正思路一致。 */

      ret = ov3660_set_pll(false, 16, 5, 0, false, 2, true, 2);
      g_pll_mul = 16;
      g_pll_div = 2;
    }

  return ret;
}

/* v171：AEC 最大曝光上限（单位 = 行）。
 * 默认软件表里 0x3a02/0x3a14 = 0x0930 = 2352 行（VTS 约 783），
 * 意味着暗光下 AEC 可以把曝光拉到 3 帧长：帧周期
 * 190ms → 560ms，取景直接从 2.3fps 掉到 1.1fps（实测）。
 * 压到 768 行（略小于 1 个 VTS）就把帧周期锁在 ~1 VTS：
 * 暗光靠增益补偿 —— 画面更噪但不变暗，帧率稳定。
 * 只在 RGB565 取景模式生效；JPEG 拍照模式恢复默认，
 * 避免照片因曝光上限变暗。 */

#define CAM_AE_MAX_LINES_DEFAULT  0x0930

static uint16_t g_preview_ae_max_lines = CAM_AE_MAX_LINES_DEFAULT;

static int ov3660_set_ae_max_exposure(uint16_t lines)
{
  int ret;

  ret = board_camera_sccb_write(0x3a02, (uint8_t)(lines >> 8));
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(0x3a03, (uint8_t)(lines & 0xff));
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(0x3a14, (uint8_t)(lines >> 8));
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(0x3a15, (uint8_t)(lines & 0xff));
  return ret;
}

/* set_ae_level(0) —— 官方 reset() 在默认序列后必调（ov3660.c 第 196 行）。
 * 不设曝光目标，画面会过暗/偏色（AEC 未正确初始化）。
 * 照抄官方实现（target=(0+5)*10+5=55）：
 *   level_low  = 55*23/25 = 50 (0x32)
 *   level_high = 55*27/25 = 59 (0x3b)
 *   fast_low   = 50>>1    = 25 (0x19)
 *   fast_high  = 59<<1    = 118 (0x76) */

static int ov3660_set_ae_level0(void)
{
  int ret;

  ret = board_camera_sccb_write(0x3a0f, 59)   /* level_high */
    || board_camera_sccb_write(0x3a10, 50)    /* level_low */
    || board_camera_sccb_write(0x3a1b, 59)    /* level_high */
    || board_camera_sccb_write(0x3a1e, 50)    /* level_low */
    || board_camera_sccb_write(0x3a11, 118)   /* fast_high */
    || board_camera_sccb_write(0x3a1f, 25);   /* fast_low */
  if (ret < 0)
    {
      printf("[Cam] set_ae_level failed: %d\n", ret);
      return ret;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camera_init(void)
{
  static bool s_probed;
  uint16_t pid = 0;
  int ret;

  /* Idempotent: probe (and the XMCLK LEDC init inside it) only ONCE.
   * Repeated probes re-init the sensor clock and disturb the running
   * AEC/AWB -> the image oscillates instead of converging. */

  if (s_probed)
    {
      return 0;
    }

  /* 探测重试（3 次 / 200ms）：XMCLK 刚起来时传感器偶发不应答 SCCB，
   * 单次失败会让整个拍照流程报"camera_init failed: -19"（2026-09-10
   * 实测遇到过一次，重试即可）。 */

  {
    int attempt;

    for (attempt = 0; ; attempt++)
      {
        ret = board_camera_sccb_probe(&pid);
        if (ret == 0)
          {
            break;
          }

        if (attempt >= 2)
          {
            printf("[Cam] probe failed after %d tries: %d\n",
                   attempt + 1, ret);
            return ret;
          }

        printf("[Cam] probe retry %d (ret=%d)\n", attempt + 1, ret);
        usleep(200 * 1000);
      }
  }

  g_camera_ae_level = plant_pref_load_int(PLANT_PREF_AE_PATH, 5, -5, 5);
  g_camera_flipped = plant_pref_load_int(PLANT_PREF_FLIP_PATH, 0, 0, 1) != 0;
  s_probed = true;
  printf("[Cam] preferences: ae=%d flipped=%u\n", g_camera_ae_level, (unsigned)g_camera_flipped);
  printf("[Cam] OV3660 detected (PID=0x%04x)\n", pid);
  return 0;
}

/****************************************************************************
 * Name: camera_config_jpeg_qvga
 *
 * 配置 OV3660 输出 JPEG 320×240（官方寄存器序列）。
 * 帧采集（LCD_CAM DMA）在 camera_capture_frame() 实现。
 ****************************************************************************/

int camera_config_jpeg_qvga(void)
{
  int ret;

  /* 官方默认序列（含软复位 + REG_DLY 10ms） */

  ret = ov3660_write_regs(ov3660_default_regs);
  if (ret < 0)
    {
      printf("[Cam] default regs failed: %d\n", ret);
      return ret;
    }

  /* set_ae_level(0)（官方 reset() 必调，否则曝光不收敛） */

  ret = ov3660_set_ae_level0();
  if (ret < 0)
    {
      return ret;
    }

  /* JPEG 格式 */

  ret = ov3660_write_regs(ov3660_fmt_jpeg);
  if (ret < 0)
    {
      printf("[Cam] jpeg fmt failed: %d\n", ret);
      return ret;
    }

  /* 320×240 + binning + PLL */

  ret = ov3660_set_framesize(320, 240, true);
  if (ret < 0)
    {
      printf("[Cam] framesize failed: %d\n", ret);
      return ret;
    }

  /* 启动输出 */

  ret = board_camera_sccb_write(OV3660_SYSTEM_CTROL0, 0x02);
  if (ret < 0) return ret;

  ov3660_set_ae_max_exposure(CAM_AE_MAX_LINES_DEFAULT);   /* v171: 照片不限曝光 */
  board_camera_sccb_write(0x3a00, 0x3a);                  /* v173: 照片恢复自动帧率 */
  printf("[Cam] OV3660 configured: JPEG 320x240\n");
  return 0;
}

/****************************************************************************
 * Name: camera_config_jpeg_wh
 *
 * 配置 OV3660 输出【指定尺寸】的 JPEG。驱动层进入变长 JPEG 收帧模式，
 * DMA 缓冲按 CAM_JPEG_BUF_SIZE（当前 2MB）准备，capture 返回实际 EOI 长度。
 * 1024x768 走这一条；PLL 使用官方 QXGA 分支
 * （40MHz SYSCLK / 10MHz PCLK @ 20MHz XCLK）。
 ****************************************************************************/

int camera_config_jpeg_wh(uint16_t w, uint16_t h)
{
  int ret;

  /* 2048x1536: 传感器原始字节存在帧尾 MCU 截断竞态（EOI 在、末段熵数据
   * 缺 0~72 个 MCU，逐帧随机）。下方寄存器组合把完整帧概率从 ~0% 提升到
   * ~50%；HD 预览环路对解码失败的帧直接重抓，等效达到稳定可用。
   * （早前"QS=0x04 12288/12288 完整"的样本是服务器二次压缩产物，无效。） */

  if (w > 2048 || h > 1536)
    {
      printf("[Cam] JPEG %ux%u unsupported: max size is 2048x1536\n",
             (unsigned)w, (unsigned)h);
      return -EINVAL;
    }

  ret = ov3660_write_regs(ov3660_default_regs);
  if (ret < 0)
    {
      printf("[Cam] jpeg default regs failed: %d\n", ret);
      return ret;
    }

  ret = ov3660_set_ae_level0();
  if (ret < 0)
    {
      return ret;
    }

  ret = ov3660_write_regs(ov3660_fmt_jpeg);
  if (ret < 0)
    {
      printf("[Cam] jpeg fmt failed: %d\n", ret);
      return ret;
    }

  ret = ov3660_set_framesize(w, h, true);
  if (ret < 0)
    {
      printf("[Cam] jpeg framesize failed: %d\n", ret);
      return ret;
    }

  /* OV3660 compression mode 3 transmits JPEG data with a programmable VFIFO
   * line width.  The reset defaults are 1024x768; leaving them in place while
   * requesting 2048x1536 makes the compression engine segment the stream for
   * the wrong geometry and produces horizontal tearing/seams.  Match the
   * VFIFO geometry to the requested frame, as the Linux OV5640 driver does
   * for the same register block. */

  /* OV3660 compression mode 3 transmits JPEG data with a programmable VFIFO
   * line width.  The reset defaults are 1024x768; leaving them in place while
   * requesting 2048x1536 makes the compression engine segment the stream for
   * the wrong geometry and produces horizontal tearing/seams.  Match the
   * VFIFO geometry to the requested frame, as the Linux OV5640 driver does
   * for the same register block.
   *
   * 2026-09-26 复核：这两句对所有分辨率都是必需的——实测把它们限定到
   * 2048 后，1024x768 冷启动 DMA 零字节（HREF 有突发、VSYNC 死）；
   * 恢复后冷启动首抓即得完整 81~91KB JPEG。 */

  ret = ov3660_write_reg16(0x4602, w);
  if (ret < 0)
    {
      printf("[Cam] jpeg VFIFO HSIZE failed: %d\n", ret);
      return ret;
    }

  ret = ov3660_write_reg16(0x4604, h);
  if (ret < 0)
    {
      printf("[Cam] jpeg VFIFO VSIZE failed: %d\n", ret);
      return ret;
    }

  /* 2048x1536 帧尾截断缓解（2026-09-24 寄存器扫描实测）：
   *   0x4400=0x83  JFIFO 读速度 1->3（bit7 保持 YUV422）
   *   0x4401=0x03  SFIFO 读速度 1->3
   *   0x4722=0x0F  垂直结束偏移 0->15（帧尾多留输出时间窗）
   * 已排除：0x4400>=0x84（中段 Huffman 损坏）、0x4401 bit3/bit2
   * （blanking 读，反而更糟）、0x4600 bit5 固定高度（2048 直接无输出）。 */

  if (w == 2048 && h == 1536)
    {
      board_camera_sccb_write(0x4400, 0x83);
      board_camera_sccb_write(0x4401, 0x03);
      board_camera_sccb_write(0x4722, 0x0f);
    }

#ifdef CONFIG_ESP32S3_CAM_DVP
  esp32s3_cam_dvp_set_jpeg(true);
  esp32s3_cam_dvp_set_framesize(w, h);
#endif

  /* Set input-HREF qualification after programming sensor geometry. */
  ret = board_camera_sccb_write(0x4402, 0x90);
  if (ret < 0) return ret;

  /* Use the same root color/exposure baseline as RGB565. */

  camera_set_sde(false);
  camera_set_gamma_default();
  camera_set_saturation(-2);
  camera_set_ae_level(g_camera_ae_level);

  ret = board_camera_sccb_write(OV3660_SYSTEM_CTROL0, 0x02);
  if (ret < 0)
    {
      return ret;
    }

  usleep(1000 * 1000);

  {
    uint8_t v;
    board_camera_sccb_read(0x3008, &v);
    printf("[Cam] JPEG 0x3008(stream)=0x%02x\n", v);
    board_camera_sccb_read(0x460c, &v);
    printf("[Cam] JPEG 0x460c(PCLK)=0x%02x\n", v);
    board_camera_sccb_read(0x3002, &v);
    printf("[Cam] JPEG 0x3002=0x%02x\n", v);
    board_camera_sccb_read(0x3006, &v);
    printf("[Cam] JPEG 0x3006=0x%02x\n", v);
    board_camera_sccb_read(0x471c, &v);
    printf("[Cam] JPEG 0x471c=0x%02x\n", v);
  }

  /* v180：这里必须把 **四个** 缓存标志全清掉，尤其是
   * s_rgb_qvga_ready！
   * 拍一张照后传感器停在 JPEG 1024x768；回到相机页时
   * camera_preview_start_rgb() 会看 s_rgb_qvga_ready 决定“要不要重配”。
   * 它没被清 → 跳过重配 → 取景线程按 320x240 RGB565 去读
   * JPEG 数据流 → **满屏雪花且永不更新**（用户报：从识别页
   * 点返回出现雪花屏、卡住）。camera_shutdown() 里四个都清了，
   * 所以只有“拍照后不退页面”这条路径会中招。 */

  s_rgb565_ready = false;
  s_rgb_hd_ready = false;
  s_rgb_qvga_ready = false;
  s_sensor_mode = CAM_MODE_NONE;

  ov3660_set_ae_max_exposure(CAM_AE_MAX_LINES_DEFAULT);   /* v171: 照片不限曝光 */
  board_camera_sccb_write(0x3a00, 0x3a);                  /* v173: 照片恢复自动帧率 */
  printf("[Cam] OV3660 configured: JPEG %ux%u\n",
         (unsigned)w, (unsigned)h);
  return 0;
}

/****************************************************************************
 * Name: camera_config_rgb565_wh
 *
 * 配置 OV3660 输出【指定尺寸】的 RGB565（官方 sensor_fmt_rgb565 序列：
 *   FORMAT_CTRL=0x501f -> 0x01 (RGB)
 *   FORMAT_CTRL00=0x4300 -> 0x61 (RGB565 BGR)
 * 帧尺寸/binning/scale/PLL 由 ov3660_set_framesize() 按 w×h 选。
 *
 * 含软复位 + 约 1s 的 AEC/AWB 收敛等待 → 只应在【首次】配置时调用；
 * 之后切分辨率走 camera_mode_preview()/camera_mode_photo()（快得多）。
 ****************************************************************************/

int camera_config_rgb565_wh(uint16_t w, uint16_t h)
{
  static const uint16_t fmt_rgb565[][2] = {
    {OV3660_FORMAT_CTRL, 0x01},   /* RGB */
    {OV3660_FORMAT_CTRL00, 0x61}, /* RGB565 (BGR) */
    {REGLIST_TAIL, 0x00},
  };
  int ret;

  /* 官方默认序列（含软复位 0x82 → REG_DLY 10ms → 0x42 → 驱动能力/PCLK 极性）。
   * 软复位后必须延时，否则后续写入全被复位窗口吞掉（读回=默认值）。 */

  ret = ov3660_write_regs(ov3660_default_regs);
  if (ret < 0)
    {
      printf("[Cam] default regs failed: %d\n", ret);
      return ret;
    }

  /* set_ae_level(0) —— 官方 reset() 在默认序列后必调（ov3660.c 第 196 行）。
   * 不设曝光目标，白色墙壁会显示成暗色/偏色（AEC 未正确初始化）。 */

  ret = ov3660_set_ae_level0();
  if (ret < 0)
    {
      return ret;
    }

  /* RGB565 格式 */

  ret = ov3660_write_regs(fmt_rgb565);
  if (ret < 0)
    {
      printf("[Cam] rgb565 fmt failed: %d\n", ret);
      return ret;
    }

  /* 帧尺寸 + binning + PLL（必须与驱动层 DMA 回读尺寸一致） */

  ret = ov3660_set_framesize(w, h, false);
  if (ret < 0)
    {
      printf("[Cam] framesize failed: %d\n", ret);
      return ret;
    }

  /* ⚠️ 传感器改了输出尺寸，驱动层回读长度必须同步改 —— 否则会按 160×120
   * 去读 640 宽的帧：既拿不到完整画面（只有 38400B），又是错行垃圾。
   * 2026-09-10 实测症状："[Cam] photo captured: 38400 bytes (640x480)"。 */

#ifdef CONFIG_ESP32S3_CAM_DVP
  esp32s3_cam_dvp_set_jpeg(false);
  esp32s3_cam_dvp_set_framesize(w, h);
#endif

  /* Root color/exposure baseline:
   * - SDE off: remove AGC-dependent auto saturation/contrast.
   * - CMX level -2: official saturation table, not the SDE endpoint hack.
   * - AE -2: keep highlight headroom so the red channel does not clip first.
   */

  camera_set_sde(false);
  camera_set_gamma_default();
  camera_set_saturation(-2);
  camera_set_ae_level(g_camera_ae_level);

  /* 启动输出 */

  ret = board_camera_sccb_write(OV3660_SYSTEM_CTROL0, 0x02);
  if (ret < 0) return ret;

  /* 写后读回验证（官方无此步，调试用）：确认寄存器真的写进去了。
   * 之前花屏根因 = 软复位后未延时，后续写入全被吞（读回=默认值）。 */

  usleep(50 * 1000);
  {
    uint8_t v;
    uint16_t check_regs[][2] =
    {
      {0x3808, (uint16_t)(w >> 8)},      /* X_OUT 高字节 */
      {0x3809, (uint16_t)(w & 0xff)},    /* X_OUT 低字节 */
      {0x380a, (uint16_t)(h >> 8)},      /* Y_OUT 高字节 */
      {0x380b, (uint16_t)(h & 0xff)},    /* Y_OUT 低字节 */
      {0x501f, 0x01},   /* RGB */
      {0x4300, 0x61},   /* RGB565 */
      {0x3820, (uint16_t)((g_pll_binning ? 0x01 : 0x40) | (g_camera_flipped ? 6 : 0))},   /* binning/无 */
      {0x3821, (uint16_t)((g_pll_binning ? 0x01 : 0x00) | (g_camera_flipped ? 6 : 0))},   /* binning no-JPEG */
      {0x303b, g_pll_mul},   /* PLL multiplier（当前模式） */
      {0x3824, g_pll_div},   /* PCLK_RATIO（当前模式） */
      {0x5001, 0x23},   /* AWB+CMX+scale, SDE off */
      {0x5800, 0x0c},   /* LENC 表点 */
      {0x583d, 0xcf},   /* LENC 表点 */
      {0x471c, 0xd0},   /* 非 JPEG 状态（默认序列里应为 0xd0 而非 0x50） */
    };
    int i;

    {
      /* 2026-09-09：整页 14 行校验打印并入一次失败高发点，只在异常
       * 时逐行输出（降低"进拍照页即刷屏"带来的串口反压风险）。 */
      int mism = 0;
      int nchk = (int)(sizeof(check_regs) / sizeof(check_regs[0]));

      for (i = 0; i < nchk; i++)
        {
          if (board_camera_sccb_read(check_regs[i][0], &v) == 0)
            {
              if (v != check_regs[i][1])
                {
                  printf("[Cam] verify 0x%04x = 0x%02x (want 0x%02x) <-- MISMATCH\n",
                         check_regs[i][0], v, check_regs[i][1]);
                  mism++;
                }
            }
        }

      printf("[Cam] verify %d regs: %s\n", nchk,
             mism == 0 ? "all OK" : "MISMATCH");
    }
  }

  /* 传感器稳定延时：0x3008=0x02 启动输出后 AEC/AWB 需要约 1s 收敛，
   * 立即 capture 会收到黑/暗/偏色帧（曾实测 300ms 不够，帧在
   * 暗↔亮、绿↔红↔蓝间振荡）。首次配置只等这一次，后续 capture
   * 不再重配/打扰传感器，收敛状态保持。 */

  /* v173：取景帧率的真因：0x3a00 = 0x3a 里 bit5 = 1 是
   * “AEC 自动帧率控制”——暗光下 AEC 把曝光拉到 2~3 帧长，
   * 帧周期跟着从 232ms 拉到 559ms（实测：关掉后采集 559→232ms）。
   * 取景需要稳定帧率 → 关掉（曝光被限在 1 帧内，暗光靠
   * 增益补偿）；拍照模式再恢复，保证暗光照片不变暗。 */

  board_camera_sccb_write(0x3a00, 0x1a);
  ov3660_set_ae_max_exposure(g_preview_ae_max_lines);
  printf("[Cam] preview AE: frame-rate-ctrl OFF, max = %u lines\n",
         (unsigned)g_preview_ae_max_lines);

  usleep(1000 * 1000);

  printf("[Cam] OV3660 configured: RGB565 %ux%u\n", (unsigned)w, (unsigned)h);

  s_rgb565_ready = true;
  if (w != 640 || h != 480)
    {
      s_rgb_hd_ready = false;
    }

  s_rgb_qvga_ready = (w == 320 && h == 240);
  s_sensor_mode = (w == CAM_PREVIEW_W && h == CAM_PREVIEW_H) ?
                  CAM_MODE_PREVIEW : CAM_MODE_PHOTO;
  return 0;
}

/****************************************************************************
 * Name: camera_config_rgb565_qvga
 *
 * 兼容包装（历史调用点）：等价于 camera_config_rgb565_wh(160, 120)。
 ****************************************************************************/

int camera_config_rgb565_qvga(void)
{
  return camera_config_rgb565_wh(CAM_PREVIEW_W, CAM_PREVIEW_H);
}

/****************************************************************************
 * Name: camera_dump_regs
 *
 * 读 OV3660 关键寄存器，验证实际输出尺寸/格式/曝光/增益/AWB/PLL。
 * 新增：AEC 曝光(0x3500-3502)、AGC 增益(0x350a/b)、AEC/AGC 手动位(0x3503)、
 *       AWB 使能(0x5001)、AWB 增益(0x3400/3402/3404)、PLL(0x303a-d/0x3824)、
 *       total(0x380c-f)。用于定位"白墙偏暗/偏绿"。
 ****************************************************************************/

int camera_dump_regs(void)
{
  uint8_t v;
  int ret;

  ret = camera_init();
  if (ret < 0) return ret;

  /* X_ADDR_ST/END (0x3800-0x3807), X_OUTPUT_SIZE (0x3808-0x380B) */

  ret = board_camera_sccb_read(0x3808, &v);
  printf("[Cam] 0x3808(X_OUT_H)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3809, &v);
  printf("[Cam] 0x3809(X_OUT_L)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x380a, &v);
  printf("[Cam] 0x380a(Y_OUT_H)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x380b, &v);
  printf("[Cam] 0x380b(Y_OUT_L)=0x%02x\n", v);

  /* FORMAT_CTRL (0x501f) + FORMAT_CTRL00 (0x4300) */

  ret = board_camera_sccb_read(0x501f, &v);
  printf("[Cam] 0x501f(FMT)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x4300, &v);
  printf("[Cam] 0x4300(FMT00)=0x%02x\n", v);

  /* binning 寄存器 */

  ret = board_camera_sccb_read(0x3820, &v);
  printf("[Cam] 0x3820(TC20)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3821, &v);
  printf("[Cam] 0x3821(TC21)=0x%02x\n", v);

  /* AEC 曝光值（0x3500-3502 = 20bit） */

  ret = board_camera_sccb_read(0x3500, &v);
  printf("[Cam] 0x3500(AEC_H)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3501, &v);
  printf("[Cam] 0x3501(AEC_M)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3502, &v);
  printf("[Cam] 0x3502(AEC_L)=0x%02x\n", v);

  /* AEC/AGC 手动位：bit0=AEC manual, bit1=AGC manual（0=自动） */

  ret = board_camera_sccb_read(0x3503, &v);
  printf("[Cam] 0x3503(AEC_MAN)=0x%02x (AEC%s AGC%s)\n", v,
         (v & 0x01) ? "MANUAL" : "auto ",
         (v & 0x02) ? "MANUAL" : "auto ");

  /* AGC 增益（0x350a/b，6.4 位定点） */

  ret = board_camera_sccb_read(0x350a, &v);
  printf("[Cam] 0x350a(GAIN_H)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x350b, &v);
  printf("[Cam] 0x350b(GAIN_L)=0x%02x\n", v);

  /* ISP / AWB */

  ret = board_camera_sccb_read(0x5000, &v);
  printf("[Cam] 0x5000(ISP)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5001, &v);
  printf("[Cam] 0x5001(ISP1)=0x%02x (AWB=%d scale=%d)\n", v,
         (v & 0x01) ? 1 : 0, (v & 0x20) ? 1 : 0);
  ret = board_camera_sccb_read(0x3406, &v);
  printf("[Cam] 0x3406(AWB_GAIN_EN)=0x%02x\n", v);

  ret = board_camera_sccb_read(0x3400, &v);
  printf("[Cam] 0x3400(AWB_R_H)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3401, &v);
  printf("[Cam] 0x3401(AWB_R_L)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3402, &v);
  printf("[Cam] 0x3402(AWB_G_H)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3403, &v);
  printf("[Cam] 0x3403(AWB_G_L)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3404, &v);
  printf("[Cam] 0x3404(AWB_B_H)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3405, &v);
  printf("[Cam] 0x3405(AWB_B_L)=0x%02x\n", v);

  /* AWB 工作增益（0x5186-0x518e，自动 AWB 实时更新）：
   * 若出现极端值（近 0xFF）→ AWB 溢出，解释"纯彩色/波纹" */

  ret = board_camera_sccb_read(0x5186, &v);
  printf("[Cam] 0x5186(AWB_R)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5187, &v);
  printf("[Cam] 0x5187(AWB_G)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5188, &v);
  printf("[Cam] 0x5188(AWB_B)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5189, &v);
  printf("[Cam] 0x5189(AWB1)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x518a, &v);
  printf("[Cam] 0x518a(AWB2)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x518b, &v);
  printf("[Cam] 0x518b(AWB3)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x518c, &v);
  printf("[Cam] 0x518c(AWB4)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x518d, &v);
  printf("[Cam] 0x518d(AWB5)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x518e, &v);
  printf("[Cam] 0x518e(AWB6)=0x%02x\n", v);

  /* 色彩矩阵（0x5381-0x538b）：系数极端 → ISP 定点溢出 → 波纹 */

  ret = board_camera_sccb_read(0x5381, &v);
  printf("[Cam] 0x5381(CM1)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5382, &v);
  printf("[Cam] 0x5382(CM2)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5383, &v);
  printf("[Cam] 0x5383(CM3)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5384, &v);
  printf("[Cam] 0x5384(CM4)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5385, &v);
  printf("[Cam] 0x5385(CM5)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5386, &v);
  printf("[Cam] 0x5386(CM6)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5387, &v);
  printf("[Cam] 0x5387(CM7)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5388, &v);
  printf("[Cam] 0x5388(CM8)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5389, &v);
  printf("[Cam] 0x5389(CM9)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x538a, &v);
  printf("[Cam] 0x538a(CM10)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x538b, &v);
  printf("[Cam] 0x538b(CM11)=0x%02x\n", v);

  /* gamma control points: 0x5480 enable, 0x5481..0x5490 curve. */

  ret = board_camera_sccb_read(0x5480, &v);
  printf("[Cam] 0x5480(GAMMA_CTRL)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5481, &v);
  printf("[Cam] 0x5481(GAMMA1)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5487, &v);
  printf("[Cam] 0x5487(GAMMA7)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5490, &v);
  printf("[Cam] 0x5490(GAMMA15)=0x%02x\n", v);

  /* SDE saturation/contrast block. */

  ret = board_camera_sccb_read(0x5580, &v);
  printf("[Cam] 0x5580(SDE_CTRL0)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5583, &v);
  printf("[Cam] 0x5583(SDE_U)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5584, &v);
  printf("[Cam] 0x5584(SDE_V)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x5588, &v);
  printf("[Cam] 0x5588(SDE_CTRL8)=0x%02x\n", v);

  /* 测试图案检查：0x503d bit0=1 → 彩条已开（排除"纯彩色=测试图案"） */

  ret = board_camera_sccb_read(0x503d, &v);
  printf("[Cam] 0x503d(TEST_PATTERN)=0x%02x (colorbar=%d)\n", v, v & 0x01);

  /* PLL / PCLK */

  ret = board_camera_sccb_read(0x303a, &v);
  printf("[Cam] 0x303a(PLL0)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x303b, &v);
  printf("[Cam] 0x303b(PLL1)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x303c, &v);
  printf("[Cam] 0x303c(PLL2)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x303d, &v);
  printf("[Cam] 0x303d(PLL3)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x3824, &v);
  printf("[Cam] 0x3824(PCLK_RATIO)=0x%02x\n", v);

  /* total_x / total_y（决定 AEC 最大曝光 = total_y 行） */

  ret = board_camera_sccb_read(0x380c, &v);
  printf("[Cam] 0x380c(TOTAL_X_H)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x380d, &v);
  printf("[Cam] 0x380d(TOTAL_X_L)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x380e, &v);
  printf("[Cam] 0x380e(TOTAL_Y_H)=0x%02x\n", v);
  ret = board_camera_sccb_read(0x380f, &v);
  printf("[Cam] 0x380f(TOTAL_Y_L)=0x%02x\n", v);

  return 0;
}

/* 官方 esp32-camera ov3660_settings.h: sensor_saturation_levels[9][11]。
 * 11 个值依次写 0x5381..0x538B；这是 CMX 色彩矩阵，不是 SDE 的
 * 0x5583/0x5584 自动饱和度端点。 */

static const uint8_t g_ov3660_saturation_levels[9][11] =
{
  {0x1d, 0x60, 0x03, 0x07, 0x48, 0x4f, 0x4b, 0x40, 0x0b, 0x01, 0x98},
  {0x1d, 0x60, 0x03, 0x08, 0x54, 0x5c, 0x58, 0x4b, 0x0d, 0x01, 0x98},
  {0x1d, 0x60, 0x03, 0x0a, 0x60, 0x6a, 0x64, 0x56, 0x0e, 0x01, 0x98},
  {0x1d, 0x60, 0x03, 0x0b, 0x6c, 0x77, 0x70, 0x60, 0x10, 0x01, 0x98},
  {0x1d, 0x60, 0x03, 0x0c, 0x78, 0x84, 0x7d, 0x6b, 0x12, 0x01, 0x98},
  {0x1d, 0x60, 0x03, 0x0d, 0x84, 0x91, 0x8a, 0x76, 0x14, 0x01, 0x98},
  {0x1d, 0x60, 0x03, 0x0e, 0x90, 0x9e, 0x96, 0x80, 0x16, 0x01, 0x98},
  {0x1d, 0x60, 0x03, 0x10, 0x9c, 0xac, 0xa2, 0x8b, 0x17, 0x01, 0x98},
  {0x1d, 0x60, 0x03, 0x11, 0xa8, 0xb9, 0xaf, 0x96, 0x19, 0x01, 0x98},
};

int camera_set_saturation(int level)
{
  int i;
  int ret = 0;
  const uint8_t *regs;

  if (level < -4 || level > 4)
    {
      printf("[Cam] saturation level out of range (-4..4)\n");
      return -EINVAL;
    }

  regs = g_ov3660_saturation_levels[level + 4];

  for (i = 0; i < 11; i++)
    {
      ret = board_camera_sccb_write((uint16_t)(0x5381 + i), regs[i]);
      if (ret < 0)
        {
          return ret;
        }
    }

  printf("[Cam] saturation level -> %d\n", level);
  return 0;
}

/* v171：CLI 调参用（下次进取景页/重新配置时生效） */

int camera_set_preview_ae_max_lines(int lines)
{
  if (lines <= 0)
    {
      g_preview_ae_max_lines = CAM_AE_MAX_LINES_DEFAULT;
    }
  else
    {
      g_preview_ae_max_lines = (uint16_t)lines;
    }

  return (int)g_preview_ae_max_lines;
}

/* v172：单寄存器读写（调参用；可在取景跑着时直接改传感器寄存器） */

int camera_peek_reg(int reg, uint8_t *val)
{
  return board_camera_sccb_read((uint16_t)reg, val);
}

int camera_poke_reg(int reg, int val)
{
  return board_camera_sccb_write((uint16_t)reg, (uint8_t)val);
}

int camera_set_ae_level(int level)
{
  int target;
  int level_low;
  int level_high;
  int fast_low;
  int fast_high;
  int ret;

  if (level < -5 || level > 5)
    {
      printf("[Cam] ae level out of range (-5..5)\n");
      return -EINVAL;
    }

  target = ((level + 5) * 10) + 5;
  level_low = target * 23 / 25;
  level_high = target * 27 / 25;
  fast_low = level_low >> 1;
  fast_high = level_high << 1;
  if (fast_high > 255)
    {
      fast_high = 255;
    }

  ret = board_camera_sccb_write(0x3a0f, (uint8_t)level_high);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(0x3a10, (uint8_t)level_low);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(0x3a1b, (uint8_t)level_high);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(0x3a1e, (uint8_t)level_low);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(0x3a11, (uint8_t)fast_high);
  if (ret < 0) return ret;
  ret = board_camera_sccb_write(0x3a1f, (uint8_t)fast_low);
  if (ret < 0) return ret;

  g_camera_ae_level = level;

  printf("[Cam] ae level -> %d (target=%d high=%d low=%d)\n",
         level, target, level_high, level_low);
  return 0;
}

int camera_store_ae_level(int level)
{
  int ret = camera_set_ae_level(level);
  if (ret < 0) return ret;
  ret = plant_pref_save_int(PLANT_PREF_AE_PATH, level);
  printf("[Cam] exposure preference %s: %d\n", ret < 0 ? "save failed" : "saved", level);
  return ret;
}

int camera_set_flipped(int enabled)
{
  bool old = g_camera_flipped;
  bool running = camera_preview_is_running();
  int ret = 0;
  if (enabled != 0 && enabled != 1) return -EINVAL;
  if (running)
    {
      ret = camera_preview_take();
      if (ret < 0) { camera_preview_resume(); return ret; }
    }
  g_camera_flipped = enabled != 0;
  /* A stopped preview may mean a shutter worker owns the receiver.
   * Store the preference without touching SCCB in that case. The next
   * mode setup applies it through the same image-option function.
   */
  if (running)
    {
      uint8_t current = 0;
      ret = board_camera_sccb_read(OV3660_TIMING_TC_REG21, &current);
      if (ret == 0)
        ret = ov3660_set_image_options(g_pll_binning, (current & 0x20) != 0);
    }
  if (ret < 0) g_camera_flipped = old;
  else ret = plant_pref_save_int(PLANT_PREF_FLIP_PATH, enabled);
  s_rgb_qvga_ready = false;
  s_rgb_hd_ready = false;
  s_rgb565_ready = false;
  if (running) camera_preview_resume();
  printf("[Cam] flipped=%u result=%d\n", (unsigned)g_camera_flipped, ret);
  return ret;
}

int camera_get_ae_level(void) { return g_camera_ae_level; }
int camera_get_flipped(void) { return g_camera_flipped ? 1 : 0; }

/* OV3660 gamma: datasheet defaults vs the extra local override.
 * Both write 0x5481..0x5490; 0x5480 remains the gamma enable register. */

static const uint8_t g_ov3660_gamma_default[16] =
{
  0x26, 0x35, 0x48, 0x63, 0x6e, 0x77, 0x80, 0x88,
  0x8f, 0x96, 0xa3, 0xaf, 0xc5, 0xd7, 0xe8, 0x0f,
};

static const uint8_t g_ov3660_gamma_local[16] =
{
  0x05, 0x09, 0x10, 0x3a, 0x4c, 0x5a, 0x68, 0x74,
  0x80, 0x8e, 0xa4, 0xb4, 0xc8, 0xde, 0xf0, 0x15,
};

static int camera_write_gamma(const uint8_t *gamma, const char *name)
{
  int i;
  int ret;

  for (i = 0; i < 16; i++)
    {
      ret = board_camera_sccb_write((uint16_t)(0x5481 + i), gamma[i]);
      if (ret < 0)
        {
          return ret;
        }
    }

  printf("[Cam] gamma -> %s\n", name);
  return 0;
}

int camera_set_gamma_default(void)
{
  return camera_write_gamma(g_ov3660_gamma_default, "datasheet-default");
}

int camera_set_gamma_local(void)
{
  return camera_write_gamma(g_ov3660_gamma_local, "local-extra");
}

int camera_set_sde(bool on)
{
  int ret;

  ret = board_camera_sccb_write(0x5580, on ? 0x06 : 0x00);
  if (ret < 0)
    {
      return ret;
    }

  ret = board_camera_sccb_write(0x5001, on ? 0xa3 : 0x23);
  if (ret < 0)
    {
      return ret;
    }

  printf("[Cam] SDE -> %s (0x5001=0x%02x, 0x5580=0x%02x)\n",
         on ? "on" : "off", on ? 0xa3 : 0x23, on ? 0x06 : 0x00);
  return 0;
}

void camera_set_highlight_desat(bool on)
{
  s_cam_hl_desat = on;
  printf("[Cam] highlight desat -> %s\n", on ? "on" : "off");
}

/****************************************************************************
 * Name: camera_capture_frame
 *
 * 捕获一帧 RGB565 160×120（DMA 收帧 → 显示 + 落盘 /mnt/sd/photo.rgb）。
 *
 * 架构：照片只留一张（固定文件名覆盖写）。驱动 capture 一次性交付整帧
 *      （38400B），sink 拷入调用方 buf（LVGL 显示）并追加写 SD。
 *
 * buf/max_size 兼容旧签名；out_len 返回帧字节数（38400）。
 *
 * 返回 0 成功；负 errno 失败。
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_CAM_DVP
extern int esp32s3_cam_dvp_init(void);
extern int esp32s3_cam_dvp_reinit_hw(void);
extern int esp32s3_cam_dvp_start(void);
extern int esp32s3_cam_dvp_stop(void);
extern int esp32s3_cam_dvp_capture(void (*sink)(const uint8_t *data,
                                                size_t len, void *arg),
                                   void *sink_arg,
                                   volatile bool *eoi_out);
extern uint8_t *esp32s3_cam_dvp_get_frame(void);
extern void esp32s3_cam_dvp_set_quiet(bool quiet);
extern void esp32s3_cam_dvp_set_stream(bool on);
extern void esp32s3_cam_dvp_set_swap(bool on);

#define CAM_PHOTO_PATH "/mnt/sd/photo.rgb"

/* 帧尺寸单一来源：预览 160×120（camera_capture.h）。驱动层运行时会切到
 * 320×240（拍照），但本文件里 CAM_FRAME_* 只描述【预览】几何 —— 预览的
 * 直写、锐化、彩条解析全部按它算。 */

#define CAM_FRAME_W      CAM_PREVIEW_W
#define CAM_FRAME_H      CAM_PREVIEW_H
#define CAM_FRAME_BPP    CAM_PREVIEW_BPP
#define CAM_FRAME_SIZE   CAM_PREVIEW_SIZE

struct cam_photo_sink_s
{
  FILE *fp;
  uint8_t *buf;
  size_t buf_size;
  size_t buf_off;
  size_t total;
  bool started;
  volatile bool eoi;
};

static void cam_photo_sink(const uint8_t *data, size_t len, void *arg)
{
  struct cam_photo_sink_s *st = (struct cam_photo_sink_s *)arg;

  /* Copy into caller buffer (frame data for LVGL display) */

  if (st->buf != NULL && st->buf_off < st->buf_size)
    {
      size_t n = len;

      if (st->buf_off + n > st->buf_size)
        {
          n = st->buf_size - st->buf_off;
        }

      memcpy(st->buf + st->buf_off, data, n);
      st->buf_off += n;
    }

  /* Persist raw RGB565 frame to SD card (capture delivers the whole frame
   * in one sink call; data already includes the optional byte swap). */

  if (st->fp != NULL)
    {
      (void)sd_write_aligned(st->fp, data, len);
    }

  st->total += len;
  st->started = true;
}

int camera_capture_frame(uint8_t *buf, size_t max_size, size_t *out_len)
{
  struct cam_photo_sink_s st;
  int ret;

  if (max_size != 0 && max_size < 1024)
    {
      printf("[Cam] buffer too small (%u)\n", max_size);
      return -EINVAL;
    }

  memset(&st, 0, sizeof(st));
  st.buf = buf;
  st.buf_size = max_size;

  /* 1. 初始化 DVP 收帧驱动（幂等：可重复调用） */

  ret = esp32s3_cam_dvp_init();
  if (ret < 0)
    {
      printf("[Cam] dvp init failed: %d\n", ret);
      return ret;
    }

  /* 2. 可选落盘（photo.rgb，照片只留一张） */

  st.fp = fopen(CAM_PHOTO_PATH, "wb");
  if (st.fp == NULL)
    {
      printf("[Cam] fopen %s failed (keep in RAM only)\n", CAM_PHOTO_PATH);
    }

  /* 3. 启动收帧 → sink 写入 buf + 落盘 */

  ret = esp32s3_cam_dvp_start();
  if (ret < 0)
    {
      if (st.fp != NULL) fclose(st.fp);
      printf("[Cam] dvp start failed: %d\n", ret);
      return ret;
    }

  ret = esp32s3_cam_dvp_capture(cam_photo_sink, &st, &st.eoi);

  esp32s3_cam_dvp_stop();

  /* 4. 收尾 */

  if (st.fp != NULL) fclose(st.fp);

  if (ret < 0)
    {
      printf("[Cam] capture failed: %d\n", ret);
      return ret;
    }

  if (st.total == 0)
    {
      printf("[Cam] no data received (0 bytes)\n");
      return -ENODATA;
    }

  if (out_len != NULL)
    {
      *out_len = st.total;
    }

  printf("[Cam] frame captured: %u bytes\n", st.total);
  return 0;
}

/****************************************************************************
 * Name: camera_mode_preview / camera_mode_photo
 *
 * 运行时切换传感器输出几何（预览 160×120 / 拍照 320×240）。
 *
 * 2026-09-10 用户需求：预览可以不清楚，但拍照上传必须高清 —— 160×120
 * 只有 38400 字节，服务器把图交给大模型后模型只能猜（实测同一张图连问
 * 4 次给出龟背竹/海芋/未知植物 4 个不同答案）。
 *
 * ⚠️ 这里【不】重跑 camera_config_rgb565_wh()：那条路含软复位 + 1s 收敛
 * 等待，每次拍照都走一遍会让界面卡一秒以上。两种模式用的是同一个传感器
 * 窗口（0,0→2079,1547），只有输出缩放不同，场景平均亮度不变 → AEC/AWB
 * 沿用预览已收敛的状态即可，只改输出窗口 + PLL（约 0.15s）。
 *
 * 调用约定：切换前必须保证没有正在进行的 capture（先
 * camera_preview_stop()），否则会拿到半帧。
 ****************************************************************************/

int camera_mode_preview(void)
{
  int ret;

  if (!s_rgb565_ready)
    {
      /* 首次：走完整配置（含 1s 收敛等待） */
      return camera_config_rgb565_wh(CAM_PREVIEW_W, CAM_PREVIEW_H);
    }

  if (s_sensor_mode == CAM_MODE_PREVIEW)
    {
      return 0;   /* 已在该模式：零开销 */
    }

  ret = ov3660_set_framesize(CAM_PREVIEW_W, CAM_PREVIEW_H, false);
  if (ret < 0)
    {
      printf("[Cam] mode preview: framesize failed %d\n", ret);
      return ret;
    }

  esp32s3_cam_dvp_set_framesize(CAM_PREVIEW_W, CAM_PREVIEW_H);

  /* 新时序稳定（freeze 路径还会自带 2 个帧周期的对齐窗口） */

  usleep(150 * 1000);
  s_sensor_mode = CAM_MODE_PREVIEW;
  printf("[Cam] mode -> preview %dx%d (fast)\n", CAM_PREVIEW_W, CAM_PREVIEW_H);
  return 0;
}

int camera_mode_photo(void)
{
  int ret;

  if (!s_rgb565_ready)
    {
      return camera_config_rgb565_wh(CAM_PHOTO_W, CAM_PHOTO_H);
    }

  if (s_sensor_mode == CAM_MODE_PHOTO)
    {
      return 0;
    }

  ret = ov3660_set_framesize(CAM_PHOTO_W, CAM_PHOTO_H, false);
  if (ret < 0)
    {
      printf("[Cam] mode photo: framesize failed %d\n", ret);
      return ret;
    }

  esp32s3_cam_dvp_set_framesize(CAM_PHOTO_W, CAM_PHOTO_H);
  usleep(150 * 1000);
  s_sensor_mode = CAM_MODE_PHOTO;
  printf("[Cam] mode -> photo %dx%d (upload)\n", CAM_PHOTO_W, CAM_PHOTO_H);
  return 0;
}

/****************************************************************************
 * Name: cam_photo_capture_once  (static)
 *
 * 在【拍照】模式下抓一帧高清 RGB565（零拷贝），帧本体留在驱动 rxbuf。
 *
 * 为什么不 malloc：设备端堆只剩约 17KB，153600B 必失败 —— 直接借用驱动
 * 内部 rxbuf（DMA 已停 → 帧稳定），AI worker 以 borrowed 方式读取上传。
 *
 * 原始帧同时落盘 /mnt/sd/photo.rgb（固定名覆盖写），方便导到电脑上核对
 * 分辨率与清晰度（调试用，失败不影响上传）。
 ****************************************************************************/

static int cam_photo_capture_once(size_t *len_out)
{
  struct cam_photo_sink_s st;
  int ret;

  memset(&st, 0, sizeof(st));
  st.buf = NULL;              /* 不拷副本：帧本体留在 rxbuf，零拷贝 */
  st.buf_size = 0;

  ret = esp32s3_cam_dvp_init();
  if (ret < 0)
    {
      printf("[Cam] photo: dvp init failed %d\n", ret);
      return ret;
    }

  /* 强制走【冻结】路径（抓完即停 DMA，帧稳定）；字节序与预览一致。 */

  esp32s3_cam_dvp_set_stream(false);
  esp32s3_cam_dvp_set_swap(true);
  esp32s3_cam_dvp_set_quiet(true);

  st.fp = fopen(CAM_PHOTO_PATH, "wb");
  if (st.fp == NULL)
    {
      printf("[Cam] photo: fopen %s failed (keep in RAM only)\n",
             CAM_PHOTO_PATH);
    }

  ret = esp32s3_cam_dvp_start();
  if (ret < 0)
    {
      if (st.fp != NULL) fclose(st.fp);
      printf("[Cam] photo: dvp start failed %d\n", ret);
      return ret;
    }

  ret = esp32s3_cam_dvp_capture(cam_photo_sink, &st, &st.eoi);

  esp32s3_cam_dvp_stop();

  if (st.fp != NULL)
    {
      fclose(st.fp);
      st.fp = NULL;
    }

  if (ret < 0)
    {
      printf("[Cam] photo: capture failed %d\n", ret);
      return ret;
    }

  if (st.total == 0)
    {
      printf("[Cam] photo: no data received (0 bytes)\n");
      return -ENODATA;
    }

  if (len_out != NULL)
    {
      *len_out = st.total;
    }

  return 0;
}

/****************************************************************************
 * 高光溢出自适应（2026-09-10 用户报"色彩有粉色的色块失真"）
 *
 * 现象：画面亮部（窗/灯）整片糊成粉/品红色块、一点层次都没有。
 *
 * 板卡实测根因（md_395f676e26a5 原始 565 逐通道拆解）：
 *   1. 粉色区里红通道 54% 的像素已经顶到 31/31，而绿一个都没满、蓝只满
 *      8% —— "红先撞顶、绿还有富余"就是品红，那片区域因此丢光细节。
 *   2. 传感器 AWB 增益 R=1.63x / G=1.00x / B=1.18x（0x3400=0x0688、
 *      0x3402=0x0400、0x3404=0x04bd）：绿色只要亮过满量程的 61%，红色
 *      就已经溢出 —— 这是粉色块的直接来源。
 *   3. 0x3500-02 读出曝光 0x310=784 行 >= 总行数 783（顶格）：房间偏暗，
 *      AEC 把曝光拉满去提亮暗部，亮部必然过曝。
 *
 * 为什么【不】靠降曝光修（2026-09-10 plant srv diag 实测，已试过并放弃）：
 *   把画面亮度从 128/255 压到 70/255（快暗一半），红撞顶只从 13.7% 降到
 *   10.7% —— 那片亮区是灯/窗这类远超量程的光源，压曝光根本救不回来，
 *   只会把整张照片一起压暗，AI 反而更看不清。方向错了。
 *
 * 对策：拍完做一次【高光去饱和】（ISP 处理这类"品红高光"的通用做法）：
 *   对接近满量程的像素按比例把它拉向最亮通道 —— 越接近满量程拉得越狠，
 *   粉色块 → 中性白色高光；中间调与暗部一个像素都不动。
****************************************************************************/

#define CAM_HL_DESAT_T6      55   /* 6bit 域的起拉阈值（55/63≈87% 亮度） */

/* 高光去饱和：把"红通道先撞顶"造成的粉色块还原成中性高光。
 * 全整数、就地改帧；中间调用不起作用（亮度没到阈值直接跳过）。
 *
 * 返回值 = 在 6bit 域里真正消掉的红偏总量 Σ max(0,R6-G6)（恒 >= 0）。
 * excess_out（可为 NULL）= 处理前同一批高光像素的红偏总量 Σ max(0,R6-G6)。
 * 与返回值出自同一个循环、同一批像素，故 excess_out >= 返回值 恒成立。
 * 为什么由它自己算而不是事后拿存回去的像素再量：写回是 5bit 的 R/B，
 * 6→5→6 的位复制取整会把 R6 顶上 +0..2 —— 事后量在"本来就没多少粉块"的
 * 帧上会被这点量化噪声盖过（实测出现过"去饱和后总量反而涨 39%"的假象）。
 * 由算法内部直接累计，才是"这次到底消掉了多少"的干净数字。 */

unsigned cam_highlight_desat(uint8_t *fb, unsigned npx, unsigned *excess_out)
{
  unsigned i;
  unsigned done = 0;
  unsigned excess = 0;

  for (i = 0; i < npx; i++)
    {
      uint16_t px = (uint16_t)(fb[i * 2] | (fb[i * 2 + 1] << 8));
      unsigned r = (px >> 11) & 0x1f;
      unsigned g = (px >> 5) & 0x3f;
      unsigned b = px & 0x1f;
      unsigned r6 = (r << 1) | (r >> 4);   /* 5bit -> 6bit（位复制） */
      unsigned b6 = (b << 1) | (b >> 4);
      unsigned mx = r6 > g ? r6 : g;
      unsigned k;
      unsigned before;

      if (b6 > mx)
        {
          mx = b6;
        }

      if (mx <= CAM_HL_DESAT_T6)
        {
          continue;                        /* 正常亮度：原样 */
        }

      before = (r6 > g) ? (r6 - g) : 0;
      excess += before;

      /* 越接近满量程拉得越狠：mx=T6 不动，mx=63 完全去饱和 */

      k = (mx - CAM_HL_DESAT_T6) * 255u / (63u - CAM_HL_DESAT_T6);

      r6 += (mx - r6) * k / 255u;
      g  += (mx - g) * k / 255u;
      b6 += (mx - b6) * k / 255u;

      done += before - ((r6 > g) ? (r6 - g) : 0);

      r = (r6 + 1) >> 1;                   /* 6bit -> 5bit */
      b = (b6 + 1) >> 1;

      if (r > 31) r = 31;
      if (g > 63) g = 63;
      if (b > 31) b = 31;

      px = (uint16_t)((r << 11) | (g << 5) | b);
      fb[i * 2] = (uint8_t)(px & 0xff);
      fb[i * 2 + 1] = (uint8_t)(px >> 8);
    }

  if (excess_out != NULL)
    {
      *excess_out = excess;
    }

  return done;
}

#ifdef CONFIG_PLANT_CAM_DIAG

/* 千分比：红通道顶格（R5=31 即 R8>=248）的像素占比。
 * ⚠️ 它【不是】验收指标（原因见下面 pink_permille 里的教训），只作参考留档。 */

static unsigned cam_frame_red_clip_permille(const uint8_t *fb, size_t nbytes)
{
  unsigned n = (unsigned)(nbytes / 2);
  unsigned i;
  unsigned clip = 0;

  if (n == 0)
    {
      return 0;
    }

  for (i = 0; i < n; i++)
    {
      uint16_t px = (uint16_t)(fb[i * 2] | (fb[i * 2 + 1] << 8));
      if (((px >> 11) & 0x1f) == 0x1f)
        {
          clip++;
        }
    }

  return (unsigned)(((uint32_t)clip * 1000U) / n);
}

/* 千分比：粉块（品红高光）嫌疑占比 —— 衡量"去饱和到底有没有生效"的正指标。
 *
 * ⚠️ 2026-09-10 教训：一开始拿上面那个"红通道顶格率"当验收指标，结果
 * 恒判"未生效"。原因很简单 —— 去饱和按设计就是把粉色【拉成中性白】，红通道
 * 依然是满的（G/B 一起被拉上来），顶格率当然不降、甚至因取整微升。所谓
 * "6 次里 2 次没进上传帧"就是这个错指标造出来的假结论。
 *
 * 正确口径：品红 = 亮（红接近满量程）且红明显高于绿。去饱和做完，这类像素
 * 应当基本消失（变成中性白高光）。阈值与离线量化口径一致：R6>=56、R6-G6>=10。 */

static unsigned cam_frame_pink_permille(const uint8_t *fb, size_t nbytes)
{
  unsigned n = (unsigned)(nbytes / 2);
  unsigned i;
  unsigned pink = 0;

  if (n == 0)
    {
      return 0;
    }

  for (i = 0; i < n; i++)
    {
      uint16_t px = (uint16_t)(fb[i * 2] | (fb[i * 2 + 1] << 8));
      unsigned r5 = (px >> 11) & 0x1f;
      unsigned r6 = (r5 << 1) | (r5 >> 4);   /* 5bit -> 6bit（与去饱和同口径） */
      unsigned g6 = (px >> 5) & 0x3f;

      if (r6 >= 56 && (r6 - g6) >= 10)
        {
          pink++;
        }
    }

  return (unsigned)(((uint32_t)pink * 1000U) / n);
}

/* 亮部红偏强度 Σ max(0,R6-G6) 现在由 cam_highlight_desat() 顺手给出
 * （excess_out）—— 它和"消掉多少"（返回值）出自同一个循环、同一批像素，
 * 两者天然可比，不会再碰上"事后量回来"的量化噪声。
 *
 * 为什么不拿"粉块嫌疑占比"当验收证据：它的阈值卡在边界上，位于阈值附近的
 * 像素只被轻轻拉一点点（k 很小），占比可能只降千分之几，看着像"没生效"，
 * 其实已经生效了 —— 2026-09-10 就是这个假象让我误判了一轮。去饱和对每个
 * 高光像素的 (R-G) 差按 (1 - k/255) 等比缩小，所以红偏总量【必然】下降，
 * 下降百分比就是平均 k/255。总量不降 = 去饱和没作用在这块缓冲上。 */

#endif /* CONFIG_PLANT_CAM_DIAG */

/* 画面平均亮度（0-255；每 64 像素抽 1 个，够判断"是不是本来就很暗"） */

static unsigned cam_frame_mean_luma(const uint8_t *fb, size_t nbytes)
{
  unsigned n = (unsigned)(nbytes / 2);
  unsigned i;
  uint32_t sum = 0;
  unsigned cnt = 0;

  for (i = 0; i < n; i += 64)
    {
      uint16_t px = (uint16_t)(fb[i * 2] | (fb[i * 2 + 1] << 8));
      unsigned r = (((px >> 11) & 0x1f) * 255U) / 31U;
      unsigned g = (((px >> 5) & 0x3f) * 255U) / 63U;
      unsigned b = ((px & 0x1f) * 255U) / 31U;
      sum += (r + g + b) / 3U;
      cnt++;
    }

  return cnt ? (unsigned)(sum / cnt) : 0;
}

/****************************************************************************
 * Name: camera_photo_capture
 *
 * 抓一帧高清图，并对亮部做高光去饱和（见上面说明：粉色块 → 中性高光）。
 * 返回的帧指针仍指向驱动 rxbuf（已就地修过色），调用方借用上传。
 ****************************************************************************/

int camera_photo_capture(const uint8_t **frame, size_t *len_out)
{
  size_t total = 0;
  uint8_t *fb;
#ifdef CONFIG_PLANT_CAM_DIAG
  unsigned excess;
  unsigned desat_done;
  unsigned clip;
  unsigned pink;
  unsigned pink_after;
  unsigned luma;
#endif
  int ret;

  if (frame == NULL || len_out == NULL)
    {
      return -EINVAL;
    }

  *frame = NULL;
  *len_out = 0;

  ret = esp32s3_cam_dvp_init();
  if (ret < 0)
    {
      printf("[Cam] photo: dvp init failed %d\n", ret);
      return ret;
    }

  ret = cam_photo_capture_once(&total);
  if (ret < 0)
    {
      return ret;
    }

  fb = esp32s3_cam_dvp_get_frame();

#ifdef CONFIG_PLANT_CAM_DIAG
  /* 先量后修（诊断口径）：粉块嫌疑占比是"给用户看的现象指标"，红通道顶格率
   * 只作参考留档 —— 两者都不是验收证据，见 cam_frame_pink_permille 说明。 */

  clip = cam_frame_red_clip_permille(fb, total);
  pink = cam_frame_pink_permille(fb, total);
  luma = cam_frame_mean_luma(fb, total);
#endif

#ifdef CONFIG_PLANT_CAM_DIAG
  /* 高光去饱和：仅保留为诊断 A/B，正式链路默认关闭。 */

  if (s_cam_hl_desat)
    {
      desat_done = cam_highlight_desat(fb, (unsigned)(total / 2), &excess);
    }
  else
    {
      desat_done = 0;
      excess = 0;
    }

  printf("[Cam] photo: 粉块嫌疑 %.2f%%，亮部红偏强度 %u，红通道顶格 %.1f%%"
         "，亮度 %u/255 -> 高光去饱和 %s\n",
         (double)pink / 10.0, excess, (double)clip / 10.0, luma,
         s_cam_hl_desat ? "on" : "off");

  if (!s_cam_hl_desat)
    {
      printf("[Cam] photo: 高光去饱和关闭，保留 sensor/ISP 原始色彩\n");
    }
  else
    {
      /* 修完立刻在同一块缓冲上复测。 */

      pink_after = cam_frame_pink_permille(fb, total);
      if (excess == 0)
        {
          printf("[Cam] photo: 无高光红偏，去饱和无需作用（粉块 %.2f%%）\n",
                 (double)pink / 10.0);
        }
      else
        {
          printf("[Cam] photo: 去饱和已生效（亮部红偏 %u -> %u，消掉 %u%%；"
                 "粉块嫌疑 %.2f%% -> %.2f%%）\n",
                 excess, excess - desat_done,
                 (desat_done * 100U) / excess,
                 (double)pink / 10.0, (double)pink_after / 10.0);
        }
    }

#else
  /* 正式版：一行说清这一帧做了什么（诊断细节见 CONFIG_PLANT_CAM_DIAG）。 */

  printf("[Cam] photo: %ux%u %u 字节，高光去饱和 %s，亮度 %u/255\n",
         CAM_PHOTO_W, CAM_PHOTO_H, (unsigned)total,
         s_cam_hl_desat ? "on" : "off",
         cam_frame_mean_luma(fb, total));
#endif

  *frame = fb;
  *len_out = total;

  printf("[Cam] photo captured: %u bytes (%dx%d)\n",
         (unsigned)total, CAM_PHOTO_W, CAM_PHOTO_H);
  return 0;
}

/****************************************************************************
 * Name: camera_jpeg_capture
 *
 * 抓一帧变长 JPEG。驱动在 VSYNC EOF 后扫描 EOI（0xFFD9），返回值就是
 * 实际 JPEG 字节数；帧本体留在驱动 rxbuf，调用方借用读取。
 ****************************************************************************/

int camera_jpeg_capture(const uint8_t **frame, size_t *len_out)
{
  volatile bool eoi = false;
  int ret;

  if (frame == NULL || len_out == NULL)
    {
      return -EINVAL;
    }

  *frame = NULL;
  *len_out = 0;

  ret = esp32s3_cam_dvp_init();
  if (ret < 0)
    {
      printf("[Cam] jpeg: dvp init failed %d\n", ret);
      return ret;
    }

  ret = esp32s3_cam_dvp_start();
  if (ret < 0)
    {
      printf("[Cam] jpeg: dvp start failed %d\n", ret);
      return ret;
    }

  ret = esp32s3_cam_dvp_capture(NULL, NULL, &eoi);
  esp32s3_cam_dvp_stop();

  if (ret < 0)
    {
      printf("[Cam] jpeg capture failed: %d\n", ret);
      return ret;
    }

  *frame = esp32s3_cam_dvp_get_frame();
  *len_out = (size_t)ret;
  printf("[Cam] JPEG captured: %u bytes\n", (unsigned)ret);
  return 0;
}

int camera_jpeg_capture_to_sd(const char *path, size_t *len_out)
{
  const uint8_t *frame = NULL;
  size_t len = 0;
  FILE *fp;
  int ret;

  ret = camera_jpeg_capture(&frame, &len);
  if (ret < 0)
    {
      return ret;
    }

  if (path == NULL || frame == NULL || len == 0)
    {
      return -EINVAL;
    }

  esp32s3_cam_dvp_invalidate(frame, len);

  fp = fopen(path, "wb");
  if (fp == NULL)
    {
      printf("[Cam] jpeg: fopen %s failed\n", path);
      return -errno;
    }

  if (fwrite(frame, 1, len, fp) != len)
    {
      printf("[Cam] jpeg: short write to %s\n", path);
      fclose(fp);
      return -EIO;
    }

  fclose(fp);

  if (len_out != NULL)
    {
      *len_out = len;
    }

  printf("[Cam] JPEG saved: %s (%u bytes)\n", path, (unsigned)len);
  return 0;
}

/****************************************************************************
 * Name: camera_colorbar_test
 *
 * 传感器彩条测试图案验证（决定"字节序 + R/B 分量序"的唯一可靠方法）。
 *
 * 背景：0x4300=0x61 输出字节序经 2.49 彩条实锤为**大端（高字节先发）**、
 * swap 必须 ON；此测试用传感器自带标准 8 色条做最终交叉验证，同时确认
 * R/B 分量序（SWP 解码若与标准色吻合则无需任何额外换位）。
 * 方法：开 0x503D bit0 彩条 → 传感器输出已知 8 条标准色（白/黄/青/绿/
 * 品红/红/蓝/黑），与场景/曝光/白平衡无关 → 采样每条中心像素 → 打印
 * 4 种解码（LE 当前路径 / SWP 字节交换 / LE_RB 分量换位 / SWP_RB 双换），
 * 哪列和标准色吻合，就说明需要哪种变换。
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_CAM_DVP
int camera_colorbar_test(void)
{
  int ret;
  volatile bool eoi = false;
  const uint8_t *fp;
  int bar;

  /* 1. 开彩条（0x503D = 0x80：Bit7=Test enable + Bit[3:2]=00 标准 8 色条）。
   *    注意：OV3660 的 0x503D 是 PRE_ISP_TEST_SETTING_1，Bit[7] 才是彩条使能
   *    （官方 ov3660_regs.h + TEST_COLOR_BAR=0xC0）；bit0 未定义——上一版
   *    写 bit0 实际没开彩条，抓到的是实时场景。写完读回验证。 */

  {
    uint8_t v = 0;

    board_camera_sccb_read(0x503d, &v);
    printf("[Cam-Bar] 0x503d before = 0x%02x\n", v);
    ret = board_camera_sccb_write(0x503d, 0x80);
    if (ret < 0)
      {
        printf("[Cam] enable colorbar failed: %d\n", ret);
        return ret;
      }

    board_camera_sccb_read(0x503d, &v);
    printf("[Cam-Bar] 0x503d after  = 0x%02x (want 0x80)\n", v);
  }

  /* 2. 收一帧（复用驱动 rxbuf，零拷贝；彩条无需 AEC 收敛）。
   *    临时 swap=off 让 rxbuf 保持原始字节（双解码判字节序）；
   *    结束后恢复 swap=on（OV3660 大端，2.49）。 */

  esp32s3_cam_dvp_set_swap(false);
  esp32s3_cam_dvp_set_quiet(false);
  esp32s3_cam_dvp_init();
  esp32s3_cam_dvp_start();
  ret = esp32s3_cam_dvp_capture(NULL, NULL, &eoi);
  esp32s3_cam_dvp_stop();
  esp32s3_cam_dvp_set_swap(true);

  /* 3. 关彩条，恢复实时画面（无论 capture 成败都执行） */

  {
    uint8_t v = 0;

    board_camera_sccb_read(0x503d, &v);
    board_camera_sccb_write(0x503d, v & ~0x80);
  }

  if (ret < 0)
    {
      printf("[Cam] colorbar capture failed: %d\n", ret);
      return ret;
    }

  /* 4. 采样：中间行(y=60) 每条第中心 x=bar*20+10（160 宽 / 8 条 = 20px） */

  fp = esp32s3_cam_dvp_get_frame();
  printf("[Cam-Bar] expected: white yellow cyan green magenta red blue black\n");

  for (bar = 0; bar < 8; bar++)
    {
      int x = bar * 20 + 10;
      int off = 60 * CAM_FRAME_W * 2 + x * 2;
      uint16_t le  = fp[off] | (uint16_t)(fp[off + 1] << 8);
      uint16_t sw  = fp[off + 1] | (uint16_t)(fp[off] << 8);

      printf("[Cam-Bar] bar%d x=%d raw %02x %02x"
             " | LE r%02u g%02u b%02u"
             " | SWP r%02u g%02u b%02u"
             " | LE_RB r%02u g%02u b%02u"
             " | SWP_RB r%02u g%02u b%02u\n",
             bar, x, fp[off], fp[off + 1],
             (le >> 11) & 0x1f, (le >> 5) & 0x3f, le & 0x1f,
             (sw >> 11) & 0x1f, (sw >> 5) & 0x3f, sw & 0x1f,
             le & 0x1f, (le >> 5) & 0x3f, (le >> 11) & 0x1f,
             sw & 0x1f, (sw >> 5) & 0x3f, (sw >> 11) & 0x1f);
    }

  /* 帧头原始字节（核对条带结构） */

  printf("[Cam-Bar] head: %02x %02x %02x %02x %02x %02x %02x %02x "
         "%02x %02x %02x %02x %02x %02x %02x %02x\n",
         fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7],
         fp[8], fp[9], fp[10], fp[11], fp[12], fp[13], fp[14], fp[15]);

  printf("[Cam-Bar] done (pattern disabled, live image restored)\n");
  return 0;
}
#endif /* CONFIG_ESP32S3_CAM_DVP */
#else
int camera_capture_frame(uint8_t *buf, size_t max_size, size_t *out_len)
{
  (void)buf;
  (void)max_size;
  (void)out_len;

  return -ENOSYS;
}

int camera_colorbar_test(void)
{
  printf("[Cam] colorbar test not compiled (CONFIG_ESP32S3_CAM_DVP)\n");
  return -ENOSYS;
}

int camera_mode_preview(void)
{
  return -ENOSYS;
}

int camera_mode_photo(void)
{
  return -ENOSYS;
}

int camera_photo_capture(const uint8_t **frame, size_t *len_out)
{
  (void)frame;
  (void)len_out;
  return -ENOSYS;
}
#endif

/****************************************************************************
 * 动态预览（参考官方 dvp_spi_lcd：帧完成 → 直接画 LCD）
 *
 * camera_preview_start(x,y,w,h) 启动后台线程：
 *   连续 capture（静默）→ 软件缩放 → LCDDEVIO_PUTAREA 直接写 LCD 区域。
 * camera_preview_take() 拍照冻结：保留当前帧在 rxbuf（画面已定格在屏上）。
 * camera_preview_stop() 停止线程（页面退出时调用）。
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_CAM_DVP
static pthread_t s_preview_tid;
static volatile bool s_preview_run;
static volatile bool s_preview_freeze;
static volatile bool s_preview_idle;   /* v175：线程已真正停在空转点 */

/* 直写预览（= plant cam preview 的逻辑，同步到 UI 拍照页）：
 * 后台线程连续 capture（静默）→ lcd_put_rgb565 直接把每帧画到
 * 取景框区域（绕过 LVGL 渲染器）。目标区域由 camera_preview_start
 * 的 (x,y,w,h) 指定（UI 传绿框内部坐标）。
 * 不额外占内存（rxbuf 驱动已有）；capture 末尾不 re-arm（rxbuf 稳定）。 */

static int s_preview_x;
static int s_preview_y;
static int s_preview_w;
static int s_preview_h;

static volatile uint32_t s_preview_seq;   /* 帧序号：每次 capture 完成 +1 */

static _Atomic uint32_t s_display_epoch=1,s_display_done,s_display_count;
static _Atomic int s_display_error;
void camera_preview_invalidate(void){atomic_fetch_add(&s_display_epoch,1);}
static bool camera_preview_refresh_pending(void)
{return atomic_load(&s_display_epoch)!=atomic_load(&s_display_done);}
bool camera_preview_refresh_complete(void)
{return atomic_load(&s_display_count)>0 && !camera_preview_refresh_pending() && atomic_load(&s_display_error)>=0;}
void camera_preview_dump_status(void)
{
 printf("[CamPreview] running=%d frozen=%d acquired=%lu rendered=%lu error=%d refresh=%d\n",
 s_preview_run,s_preview_freeze,(unsigned long)s_preview_seq,
 (unsigned long)atomic_load(&s_display_count),atomic_load(&s_display_error),camera_preview_refresh_pending());
 if(s_preview_run&&s_rgb_qvga_ready){
  const uint16_t *fb=(const uint16_t *)esp32s3_cam_dvp_get_frame();
  if(fb){unsigned low=UINT32_MAX,high=0,white=0;uint32_t sum=0;
   for(unsigned i=0;i<64;i++){uint16_t p=fb[(size_t)i*320u*240u/64u];
    unsigned value=((p>>11)&31u)*8u+((p>>5)&63u)*4u+(p&31u)*8u;
    if(value<low)low=value;if(value>high)high=value;sum+=value;
    if(((p>>11)&31u)>=30u&&((p>>5)&63u)>=62u&&(p&31u)>=30u)white++;
   }
   printf("[CamPreviewPixels] low=%u high=%u mean=%u white=%u/64\n",low,high,(unsigned)(sum/64u),white);
  }
 }
}

/* 高清取景（2026-09-26 回退 1024x768）：2048x1536 帧尾截断竞态 +
 * 全帧解码耗时让取景只有 2-3fps 且偶发中段损坏（孩子等不了）。
 * 1024x768 用官方 pclk_div=8 时序（稳定完整），TJpgDec 1/2 解码到
 * 512x384 后直写 LCD（预览缓冲 384KB，省 1.1MB PSRAM），帧率数倍提升。
 * 快门仍只冻结当前已显示的这份 JPEG 并原样上传（所见即所传）。 */

#define CAM_HD_JPEG_W       1024
#define CAM_HD_JPEG_H       768
#define CAM_HD_JPEG_SCALE   1
#define CAM_HD_PREVIEW_W    (CAM_HD_JPEG_W >> CAM_HD_JPEG_SCALE)
#define CAM_HD_PREVIEW_H    (CAM_HD_JPEG_H >> CAM_HD_JPEG_SCALE)
#define CAM_HD_PREVIEW_BPP  2
#define CAM_HD_PREVIEW_SIZE (CAM_HD_PREVIEW_W * CAM_HD_PREVIEW_H * \
                             CAM_HD_PREVIEW_BPP)
#define CAM_HD_WORK_SIZE    4096

struct cam_hd_jpeg_io_s
{
  const uint8_t *data;
  size_t len;
  size_t pos;
};

static uint8_t *s_hd_preview_frame;
static const uint8_t *s_hd_jpeg;
static size_t s_hd_jpeg_len;
static uint8_t s_hd_work[CAM_HD_WORK_SIZE] __attribute__((aligned(8)));
static bool s_preview_hd;
static int s_preview_min_ms;   /* 取景最小更新间隔（0=不限速） */
static bool s_preview_hold;    /* 诊断：停采集、反复写同一帧 */
static bool s_prev_reframe;    /* 每帧重新对齐（单拍模式）*/
static bool s_prev_verify = true;  /* 帧校验：错位帧不上屏，避免"晃" */
static uint32_t s_prev_thresh = 35;  /* 校验阈值（每采样点 SAD 上限） */
static uint16_t s_frame_w = CAM_PREVIEW_W;  /* RGB 预览源帧几何（运行时） */
static uint16_t s_frame_h = CAM_PREVIEW_H;

/* ── v166：取景控件「让位」蒙版 ───────────────────────────────────
 * 取景框铺满整屏后，控制键不再独占底部黑栏，而是“浮”在画面上：它们就是普通
 * 的 LVGL 对象（像素由 LVGL 自己画），相机线程写屏时把这些区域让开不写——两者
 * 从此不抢同一批像素，既不闪也不会被盖掉。
 * 形状用圆角矩形描述：r = w/2 即圆形，r = 0 即直角矩形。
 * 生命周期：UI 线程建页时注册，预览线程 start 时拷一份快照，运行期只读快照
 * → 无竞态。 */

#define CAM_MASK_MAX  8

struct cam_mask_s
{
  int x;
  int y;
  int w;
  int h;
  int r;
};

static struct cam_mask_s s_mask_list[CAM_MASK_MAX];
static int s_mask_num;
static struct cam_mask_s s_pv_mask[CAM_MASK_MAX];
static int s_pv_mask_num;

void camera_preview_mask_clear(void)
{
  s_mask_num = 0;
}

int camera_preview_mask_add(int x, int y, int w, int h, int r)
{
  if (s_mask_num >= CAM_MASK_MAX || w <= 0 || h <= 0)
    {
      return -1;
    }

  s_mask_list[s_mask_num].x = x;
  s_mask_list[s_mask_num].y = y;
  s_mask_list[s_mask_num].w = w;
  s_mask_list[s_mask_num].h = h;
  s_mask_list[s_mask_num].r = r;
  return s_mask_num++;
}

static void cam_mask_take_snapshot(void)
{
  s_pv_mask_num = s_mask_num;
  if (s_pv_mask_num > 0)
    {
      memcpy(s_pv_mask, s_mask_list,
             sizeof(s_pv_mask[0]) * (size_t)s_pv_mask_num);
    }
}

static int cam_mask_isqrt(int v)
{
  int r = 0;
  int b = 1 << 14;

  while (b > v)
    {
      b >>= 2;
    }

  while (b != 0)
    {
      if (v >= r + b)
        {
          v -= r + b;
          r = (r >> 1) + b;
        }
      else
        {
          r >>= 1;
        }

      b >>= 2;
    }

  return r;
}

static bool cam_mask_row_hit(int sy)
{
  int i;

  for (i = 0; i < s_pv_mask_num; i++)
    {
      if (sy >= s_pv_mask[i].y && sy < s_pv_mask[i].y + s_pv_mask[i].h)
        {
          return true;
        }
    }

  return false;
}

/* 某一屏行上控件占用的 x 区间（局部坐标，已减 dx），成对写进
 * blocked[]，返回区间个数（已排序合并）。 */

static int cam_mask_blocked_row(int sy, int dx, int dw, int *blocked)
{
  int n = 0;
  int i;

  for (i = 0; i < s_pv_mask_num; i++)
    {
      const struct cam_mask_s *m = &s_pv_mask[i];
      int inset = 0;
      int x0;
      int x1;

      if (sy < m->y || sy >= m->y + m->h)
        {
          continue;
        }

      if (m->r > 0)
        {
          int r = m->r;
          int dy;

          if (r > m->w / 2)
            {
              r = m->w / 2;
            }

          if (r > m->h / 2)
            {
              r = m->h / 2;
            }

          dy = sy - m->y;
          if (dy < r)
            {
              int k = r - dy;

              inset = r - cam_mask_isqrt(r * r - (k - 1) * (k - 1));
            }
          else if (dy >= m->h - r)
            {
              int k = dy - (m->h - r) + 1;

              inset = r - cam_mask_isqrt(r * r - (k - 1) * (k - 1));
            }
        }

      if (inset > 0)
        {
          /* 曲线行多让 1px：控件边缘是抗锯齿的，宁可多让一点，
           * 也不能让画面啧掉控件（啧了就是永久的：LVGL 不会重绘）。 */

          inset--;
        }
      x0 = m->x - dx + inset;
      x1 = m->x - dx + m->w - inset;

      if (x0 < 0)
        {
          x0 = 0;
        }

      if (x1 > dw)
        {
          x1 = dw;
        }

      if (x1 <= x0)
        {
          continue;
        }

      blocked[2 * n] = x0;
      blocked[2 * n + 1] = x1;
      n++;
    }

  for (i = 1; i < n; i++)      /* 插入排序（n <= 8） */
    {
      int key0 = blocked[2 * i];
      int key1 = blocked[2 * i + 1];
      int k = i - 1;

      while (k >= 0 && blocked[2 * k] > key0)
        {
          blocked[2 * (k + 1)] = blocked[2 * k];
          blocked[2 * (k + 1) + 1] = blocked[2 * k + 1];
          k--;
        }

      blocked[2 * (k + 1)] = key0;
      blocked[2 * (k + 1) + 1] = key1;
    }

  {
    int out = 0;

    for (i = 0; i < n; i++)
      {
        if (out > 0 && blocked[2 * i] <= blocked[2 * out - 1])
          {
            if (blocked[2 * i + 1] > blocked[2 * out - 1])
              {
                blocked[2 * out - 1] = blocked[2 * i + 1];
              }
          }
        else
          {
            blocked[2 * out] = blocked[2 * i];
            blocked[2 * out + 1] = blocked[2 * i + 1];
            out++;
          }
      }

    n = out;
  }

  return n;
}

static size_t cam_hd_jpeg_input(JDEC *jd, uint8_t *buf, size_t nbytes)
{
  struct cam_hd_jpeg_io_s *io = (struct cam_hd_jpeg_io_s *)jd->device;
  size_t remain;
  size_t n;

  if (io == NULL || io->pos >= io->len)
    {
      return 0;
    }

  remain = io->len - io->pos;
  n = nbytes < remain ? nbytes : remain;
  if (buf != NULL)
    {
      memcpy(buf, io->data + io->pos, n);
    }

  io->pos += n;
  return n;
}

static unsigned s_hd_decoded_bottom;   /* 本次解码覆盖到的最大输出行 */
static unsigned s_hd_mono_streak;      /* 连续单色帧计数（镜头被挡/异常帧） */

/* 单色帧检测（2026-09-26）：全蓝异常帧（实测 93% 像素同一蓝色）或
 * 镜头被手捂住时，JPEG 解码能过但画面没有任何信息量 —— 这种帧不该
 * 成为快门快照上传给 AI（白等 10s 还得个"看不清"）。
 * 抽样 1/16 像素，与整幅均值逐通道偏差 ±2（RGB565 步进）内判同质；
 * 同质率 >98% 即单色帧。真实场景（哪怕一面墙）有纹理/噪声，不会误判。 */

static bool cam_hd_frame_is_mono(void)
{
  const uint16_t *p = (const uint16_t *)s_hd_preview_frame;
  uint32_t sum_r = 0;
  uint32_t sum_g = 0;
  uint32_t sum_b = 0;
  uint32_t n = 0;
  uint32_t same = 0;
  uint32_t i;
  int mr;
  int mg;
  int mb;

  for (i = 0; i < (uint32_t)CAM_HD_PREVIEW_W * CAM_HD_PREVIEW_H; i += 16)
    {
      uint16_t px = p[i];

      sum_r += (px >> 11) & 0x1f;
      sum_g += (px >> 5) & 0x3f;
      sum_b += px & 0x1f;
      n++;
    }

  mr = (int)(sum_r / n);
  mg = (int)(sum_g / n);
  mb = (int)(sum_b / n);

  for (i = 0; i < (uint32_t)CAM_HD_PREVIEW_W * CAM_HD_PREVIEW_H; i += 16)
    {
      uint16_t px = p[i];
      int dr = (int)((px >> 11) & 0x1f) - mr;
      int dg = (int)((px >> 5) & 0x3f) - mg;
      int db = (int)(px & 0x1f) - mb;

      if (dr >= -2 && dr <= 2 && dg >= -2 && dg <= 2 &&
          db >= -2 && db <= 2)
        {
          same++;
        }
    }

  return same * 100u > n * 98u;
}

static int cam_hd_jpeg_output(JDEC *jd, void *bitmap, JRECT *rect)
{
  const uint16_t *src = (const uint16_t *)bitmap;
  uint16_t *dst;
  unsigned row;
  unsigned width;

  if (s_hd_preview_frame == NULL || rect == NULL || bitmap == NULL)
    {
      return 0;
    }

  width = (unsigned)(rect->right - rect->left + 1);
  /* 2026-09-25: TJpgDec (JD_FASTDECODE=1) fabricates 0xFF stuff bits once
   * the entropy stream stalls at a marker/spurious-FF, and keeps decoding
   * to the bottom -- so rect->bottom alone CANNOT tell real rows from
   * fabricated ones.  Only count rows decoded while no marker stall has
   * been seen; mid-stream-corrupt frames then report their TRUE coverage
   * and get rejected by the >=760-row acceptance check. */
  if (jd->marker == 0 && (unsigned)rect->bottom + 1 > s_hd_decoded_bottom)
    {
      s_hd_decoded_bottom = (unsigned)rect->bottom + 1;
    }

  dst = (uint16_t *)s_hd_preview_frame +
        (unsigned)rect->top * CAM_HD_PREVIEW_W + (unsigned)rect->left;

  for (row = 0; row < (unsigned)(rect->bottom - rect->top + 1); row++)
    {
      memcpy(dst, src, width * sizeof(uint16_t));
      src += width;
      dst += CAM_HD_PREVIEW_W;
    }

  return 1;
}


static uint32_t cam_hd_jpeg_hash(const uint8_t *jpeg, size_t len)
{
  uint32_t hash = 2166136261u;
  size_t i;

  for (i = 0; i < len; i++)
    {
      hash ^= jpeg[i];
      hash *= 16777619u;
    }

  return hash;
}

static bool s_hd_bad_dumped;

/* 坏帧取证：扫描 SOI/EOI 位置并落盘 /mnt/sd/badframe.jpg。
 * 只读不写传感器；写文件失败静默（调试用途，不影响预览）。 */

static void cam_hd_dump_bad_frame(const uint8_t *jpeg, size_t len)
{
  uint32_t soi = UINT32_MAX;
  uint32_t eoi = 0;
  uint32_t k;

  for (k = 0; k + 1 < len; k++)
    {
      if (jpeg[k] == 0xff && jpeg[k + 1] == 0xd8 && soi == UINT32_MAX)
        {
          soi = k;
        }

      if (jpeg[k] == 0xff && jpeg[k + 1] == 0xd9)
        {
          eoi = k + 2;
        }
    }

  printf("[Cam-HD] badframe len=%u soi=%d eoi=%u head=%02x%02x%02x%02x "
         "tail=%02x%02x%02x%02x\n",
         (unsigned)len, (int)(soi == UINT32_MAX ? -1 : (int)soi),
         (unsigned)eoi,
         jpeg[0], jpeg[1], jpeg[2], jpeg[3],
         jpeg[len - 4], jpeg[len - 3], jpeg[len - 2], jpeg[len - 1]);

  {
    FAR FILE *fp = fopen("/mnt/sd/badframe.jpg", "wb");

    if (fp != NULL)
      {
        fwrite(jpeg, 1, len, fp);
        fclose(fp);
        printf("[Cam-HD] badframe saved /mnt/sd/badframe.jpg\n");
      }
  }
}

static int cam_hd_jpeg_decode(const uint8_t *jpeg, size_t len)
{
  struct cam_hd_jpeg_io_s io;
  JDEC jd;
  JRESULT rc;

  if (jpeg == NULL || len == 0 || s_hd_preview_frame == NULL)
    {
      return -EINVAL;
    }

  memset(&io, 0, sizeof(io));
  io.data = jpeg;
  io.len = len;
  s_hd_decoded_bottom = 0;

  rc = jd_prepare(&jd, cam_hd_jpeg_input, s_hd_work,
                  sizeof(s_hd_work), &io);
  if (rc != JDR_OK)
    {
      printf("[Cam-HD] jd_prepare failed: %d len=%u\n",
             (int)rc, (unsigned)len);
      return -EIO;
    }

  if (jd.width != CAM_HD_JPEG_W || jd.height != CAM_HD_JPEG_H)
    {
      printf("[Cam-HD] unexpected JPEG size %ux%u\n",
             (unsigned)jd.width, (unsigned)jd.height);
      return -EINVAL;
    }

  rc = jd_decomp(&jd, cam_hd_jpeg_output, CAM_HD_JPEG_SCALE);
  if (rc != JDR_OK)
    {
      /* snapshot is CPU-owned, not a DMA destination. Invalidating it
       * here would discard dirty copied bytes. Reject any decode error
       * and let the bounded capture loop acquire a fresh full frame.
       */

      printf("[Cam-HD] jd_decomp failed: %d %ux%u len=%u\n",
             (int)rc, (unsigned)jd.width, (unsigned)jd.height,
             (unsigned)len);
      return -EIO;
    }

  return 0;
}

/* Maintenance validation: the caller owns the stopped camera receiver.
 * No file writes, upload, or relaxation of the production JPEG checks. */
int camera_jpeg_check(const uint8_t *jpeg, size_t len)
{
  if(s_hd_preview_frame==NULL)
    s_hd_preview_frame=esp32s3_psram_static_alloc(CAM_HD_PREVIEW_SIZE);
  if(s_hd_preview_frame==NULL)return -ENOMEM;
  esp32s3_cam_dvp_invalidate(jpeg,len);
  int ret=cam_hd_jpeg_decode(jpeg,len);
  printf("[Cam-Check] result=%d bytes=%u fnv=%08x rows=%u\n",ret,
         (unsigned)len,(unsigned)cam_hd_jpeg_hash(jpeg,len),s_hd_decoded_bottom);
  return ret;
}

/* BOX preview: leased DVP frames, latest-frame mailbox and masked SPI DMA. */
static pthread_mutex_t s_dvp_lock = PTHREAD_MUTEX_INITIALIZER;
static hal_camera_lease_t s_dvp_pending;
static volatile bool s_dvp_display_run;
static bool s_dvp_drawing;
static pthread_t s_dvp_display_tid;
static uint8_t *s_dvp_stage;
static uint32_t s_dvp_drawn;
static uint32_t s_dvp_replaced;
static uint32_t s_dvp_errors;
static uint32_t s_dvp_window_tick;
static uint32_t s_dvp_busy_ticks;

static bool camera_dvp_display_idle(void)
{
  bool idle;
  pthread_mutex_lock(&s_dvp_lock);
  idle = !s_dvp_pending.valid && !s_dvp_drawing;
  pthread_mutex_unlock(&s_dvp_lock);
  return idle;
}

static int camera_dvp_draw(const hal_camera_lease_t *lease, int fd)
{
  enum { CAMERA_DMA = 0x4000u };
  int bounds[2 * CAM_MASK_MAX + 2];
  int nb = 0;
  int dw = s_preview_w;
  int dh = s_preview_h;
  int sw = s_frame_w;
  int sh = s_frame_h;
  int cw = sw;
  int ch = sh;
  int cx = 0;
  int cy = 0;
  int *mapx = (int *)(s_dvp_stage + 480u * 96u * 2u);
  int *mapy = mapx + 480;
  int i;

  if (dw <= 0 || dh <= 0 || dw > 480 || dh > 320 ||
      lease->length != (size_t)sw * sh * 2u)
    {
      return -EINVAL;
    }

  if (sw * dh > sh * dw)
    {
      cw = sh * dw / dh;
      cx = (sw - cw) / 2;
    }
  else
    {
      ch = sw * dh / dw;
      cy = (sh - ch) / 2;
    }

  for (i = 0; i < dw; i++) mapx[i] = cx + i * cw / dw;
  for (i = 0; i < dh; i++) mapy[i] = cy + i * ch / dh;

  /* Split at widget rectangle edges. No camera write touches a UI mask;
   * rounded widget corners use the existing conservative bounding box. */
  bounds[nb++] = 0;
  bounds[nb++] = dh;
  for (i = 0; i < s_pv_mask_num; i++)
    {
      int top = s_pv_mask[i].y - s_preview_y;
      int bottom = top + s_pv_mask[i].h;
      if (top > 0 && top < dh) bounds[nb++] = top;
      if (bottom > 0 && bottom < dh) bounds[nb++] = bottom;
    }

  for (i = 1; i < nb; i++)
    {
      int v = bounds[i];
      int j = i;
      while (j > 0 && bounds[j - 1] > v)
        {
          bounds[j] = bounds[j - 1];
          j--;
        }
      bounds[j] = v;
    }

  for (i = 0; i + 1 < nb; i++)
    {
      int top = bounds[i];
      int bottom = bounds[i + 1];
      int blocked[2 * CAM_MASK_MAX];
      int count = 0;
      int x = 0;
      int m;

      if (top == bottom) continue;
      for (m = 0; m < s_pv_mask_num; m++)
        {
          const struct cam_mask_s *mask = &s_pv_mask[m];
          int a = mask->x - s_preview_x;
          int b = a + mask->w;
          if (s_preview_y + top < mask->y ||
              s_preview_y + top >= mask->y + mask->h) continue;
          if (a < 0) a = 0;
          if (b > dw) b = dw;
          if (a < b)
            {
              int p = count++;
              while (p > 0 && blocked[(p - 1) * 2] > a)
                {
                  blocked[p * 2] = blocked[(p - 1) * 2];
                  blocked[p * 2 + 1] = blocked[(p - 1) * 2 + 1];
                  p--;
                }
              blocked[p * 2] = a;
              blocked[p * 2 + 1] = b;
            }
        }

      for (m = 0; m <= count; m++)
        {
          int end = m < count ? blocked[m * 2] : dw;
          int width = end - x;
          if (width > 0)
            {
              /* Keep each write at most 96 rows, matching BOX's existing
               * scan-overlap bound. Pack clipped rectangles contiguously
               * so the explicit SPI DMA transport also handles masks. */
              for (int y0 = top; y0 < bottom; y0 += 96)
                {
                  int rows = bottom - y0;
                  struct lcddev_area_s area;
                  if (rows > 96) rows = 96;
                  for (int y = 0; y < rows; y++)
                    {
                      const uint8_t *src = lease->data +
                        (size_t)mapy[y0 + y] * sw * 2u;
                      uint8_t *dst = s_dvp_stage + (size_t)y * width * 2u;
                      for (int k = 0; k < width; k++)
                        {
                          int sx = mapx[x + k] * 2;
                          dst[k * 2] = src[sx];
                          dst[k * 2 + 1] = src[sx + 1];
                        }
                    }
                  memset(&area, 0, sizeof(area));
                  area.col_start = s_preview_x + x;
                  area.col_end = area.col_start + width - 1;
                  area.row_start = s_preview_y + y0;
                  area.row_end = area.row_start + rows - 1;
                  area.stride = width * 2u | CAMERA_DMA;
                  area.data = s_dvp_stage;
                  if (ioctl(fd, LCDDEVIO_PUTAREA_PRESWAPPED,
                            (unsigned long)&area) < 0) return -errno;
                }
            }
          if (m < count && blocked[m * 2 + 1] > x)
            x = blocked[m * 2 + 1];
        }
    }
  return 0;
}

/* Color-bar integrity check while real preview and SPI DMA are running. */
static bool s_dvp_phase_check;
static uint8_t s_dvp_phase_pattern;
static uint8_t s_dvp_phase_row[640];
static uint32_t s_dvp_phase_skip, s_dvp_phase_frames;
static uint32_t s_dvp_phase_bad_rows, s_dvp_phase_changed;

int camera_preview_phase_audit(bool enable)
{
  if (enable)
    {
      if (!s_preview_run || s_dvp_phase_check) return -EBUSY;
      if (board_camera_sccb_read(0x503d, &s_dvp_phase_pattern) < 0) return -EIO;
      if (board_camera_sccb_write(0x503d, 0x80) < 0) return -EIO;
      pthread_mutex_lock(&s_dvp_lock);
      s_dvp_phase_frames = s_dvp_phase_bad_rows = s_dvp_phase_changed = 0;
      s_dvp_phase_skip = 8;
      s_dvp_phase_check = true;
      pthread_mutex_unlock(&s_dvp_lock);
      printf("[DvpPhaseLive] armed\n");
      return 0;
    }

  pthread_mutex_lock(&s_dvp_lock);
  bool was_enabled = s_dvp_phase_check;
  s_dvp_phase_check = false;
  uint32_t frames = s_dvp_phase_frames;
  uint32_t bad_rows = s_dvp_phase_bad_rows;
  uint32_t changed = s_dvp_phase_changed;
  pthread_mutex_unlock(&s_dvp_lock);
  if (was_enabled && board_camera_sccb_write(0x503d, s_dvp_phase_pattern) < 0)
    return -EIO;
  printf("[DvpPhaseLive] frames=%lu bad_rows=%lu changed=%lu\n",
         (unsigned long)frames, (unsigned long)bad_rows, (unsigned long)changed);
  return frames && !bad_rows && !changed ? 0 : -EIO;
}

static void camera_dvp_phase_audit_frame(const hal_camera_lease_t *lease)
{
  pthread_mutex_lock(&s_dvp_lock);
  if (s_dvp_phase_check)
    {
      if (s_dvp_phase_skip) s_dvp_phase_skip--;
      else
        {
          bool varied = false;
          for (unsigned y = 1; y < 240; y++)
            if (memcmp(lease->data, lease->data + y * 640u, 640u))
              s_dvp_phase_bad_rows++;
          for (unsigned x = 1; x < 320; x++)
            if (memcmp(lease->data, lease->data + x * 2u, 2)) { varied = true; break; }
          if (!varied) s_dvp_phase_bad_rows++;
          if (s_dvp_phase_frames && memcmp(s_dvp_phase_row, lease->data, 640u))
            s_dvp_phase_changed++;
          memcpy(s_dvp_phase_row, lease->data, 640u);
          s_dvp_phase_frames++;
        }
    }
  pthread_mutex_unlock(&s_dvp_lock);
}

static void *camera_dvp_display_loop(void *arg)
{
  int fd = open("/dev/lcd0", O_WRONLY);
  (void)arg;
  if (fd < 0)
    {
      s_dvp_errors++;
      s_dvp_display_run = false;
      return NULL;
    }

  while (s_dvp_display_run)
    {
      hal_camera_lease_t lease = {0};
      uint32_t start;
      pthread_mutex_lock(&s_dvp_lock);
      if (s_dvp_pending.valid)
        {
          lease = s_dvp_pending;
          s_dvp_pending.valid = false;
          s_dvp_drawing = true;
        }
      pthread_mutex_unlock(&s_dvp_lock);
      if (!lease.valid)
        {
          usleep(2000);
          continue;
        }

      start = clock_systime_ticks();
      if (s_dvp_drawn == 0) s_dvp_window_tick = start;
      uint32_t display_epoch = atomic_load(&s_display_epoch);
      int draw_result = camera_dvp_draw(&lease, fd);
      atomic_store(&s_display_error, draw_result);
      if (draw_result < 0) s_dvp_errors++;
      else
        {
          atomic_store(&s_display_done, display_epoch);
          atomic_fetch_add(&s_display_count, 1);
        }
      hal_camera_release(&lease);
      pthread_mutex_lock(&s_dvp_lock);
      s_dvp_drawing = false;
      pthread_mutex_unlock(&s_dvp_lock);
      s_dvp_busy_ticks += clock_systime_ticks() - start;
      s_dvp_drawn++;
      if (s_dvp_drawn % 32u == 0)
        {
          uint32_t now = clock_systime_ticks();
          uint32_t dt = now - s_dvp_window_tick;
          hal_camera_health_t health;
          hal_camera_get_health(&health);
          printf("[DvpPreview] fps10=%lu busy_ms=%lu drawn=%lu replaced=%lu "
                 "error=%lu bytes=%lu short=%lu lease_err=%lu ovf=%lu held=%02x\n",
                 (unsigned long)(dt ? 320u * TICK_PER_SEC / dt : 0),
                 (unsigned long)((uint64_t)s_dvp_busy_ticks * 1000u /
                                 (32u * TICK_PER_SEC)),
                 (unsigned long)s_dvp_drawn,
                 (unsigned long)s_dvp_replaced, (unsigned long)s_dvp_errors,
                 (unsigned long)health.frame_bytes,
                 (unsigned long)health.short_frames,
                 (unsigned long)health.lease_errors,
                 (unsigned long)health.done_overflow, health.lease_held_mask);
          s_dvp_window_tick = now;
          s_dvp_busy_ticks = 0;
        }
    }
  close(fd);
  return NULL;
}

static void *camera_dvp_preview_loop(void *arg)
{
  hal_camera_config_t config = {0};
  hal_camera_lease_t lease = {0};
  bool display_started = false;
  int ret;
  int failures = 0;
  (void)arg;

  config.width = s_frame_w;
  config.height = s_frame_h;
  config.pixfmt = HAL_CAMERA_PIXFMT_RGB565;
  config.flags = HAL_CAMERA_CONFIG_PLAN_A;
  config.swap_bytes = false;
  s_dvp_stage = memalign(64, 480u * 96u * 2u + 800u * sizeof(int));
  s_dvp_drawn = s_dvp_replaced = s_dvp_errors = s_dvp_busy_ticks = 0;
  s_dvp_window_tick = clock_systime_ticks();
  s_dvp_pending.valid = false;
  s_dvp_drawing = false;
  ret = s_dvp_stage ? hal_camera_open(&config) : -ENOMEM;
  if (ret == 0) ret = hal_camera_start();
  if (ret < 0) goto out;
  esp32s3_cam_dvp_set_quiet(true);
  s_dvp_display_run = true;
  ret = pthread_create(&s_dvp_display_tid, NULL, camera_dvp_display_loop, NULL);
  if (ret != 0)
    {
      ret = -ret;
      goto out;
    }
  display_started = true;

  while (s_preview_run && s_dvp_display_run)
    {
      if (s_preview_freeze || s_preview_hold)
        {
          s_preview_idle = camera_dvp_display_idle();
          usleep(5000);
          continue;
        }
      s_preview_idle = false;
      ret = hal_camera_acquire(&lease, 1500);
      if (ret < 0)
        {
          printf("[DvpPreview] acquire=%d consecutive=%d\n", ret, ++failures);
          if (failures >= 5) break;
          continue;
        }
      failures = 0;
      camera_dvp_phase_audit_frame(&lease);
      s_preview_seq++;
      if (s_preview_freeze || !s_preview_run)
        {
          hal_camera_release(&lease);
          continue;
        }

      {
        hal_camera_lease_t old;
        pthread_mutex_lock(&s_dvp_lock);
        old = s_dvp_pending;
        s_dvp_pending = lease;
        lease.valid = false;
        pthread_mutex_unlock(&s_dvp_lock);
        if (old.valid)
          {
            hal_camera_release(&old);
            s_dvp_replaced++;
          }
      }
    }

out:
  if (s_dvp_phase_check) camera_preview_phase_audit(false);
  s_dvp_display_run = false;
  if (display_started) pthread_join(s_dvp_display_tid, NULL);
  extern void esp32s3_spi_camera_buffers_release(void);
  esp32s3_spi_camera_buffers_release();
  if (lease.valid) hal_camera_release(&lease);
  if (s_dvp_pending.valid) hal_camera_release(&s_dvp_pending);
  hal_camera_close();
  esp32s3_cam_dvp_set_swap(true);
  esp32s3_cam_dvp_set_quiet(false);
  free(s_dvp_stage);
  s_dvp_stage = NULL;
  s_preview_idle = true;
  s_preview_run = false;
  printf("[DvpPreview] stopped ret=%d drawn=%lu error=%lu\n", ret,
         (unsigned long)s_dvp_drawn, (unsigned long)s_dvp_errors);
  return NULL;
}

int camera_dvp_phase_test(unsigned samples)
{
  hal_camera_config_t cfg = {320, 240, HAL_CAMERA_PIXFMT_RGB565,
                             HAL_CAMERA_CONFIG_PLAN_A, false};
  hal_camera_lease_t lease = {0};
  hal_camera_health_t health;
  uint8_t old_pattern = 0;
  uint8_t *previous = NULL;
  unsigned bad_rows = 0;
  unsigned changed_frames = 0;
  int ret;
  int frame;

  if (s_preview_run) return -EBUSY;
  ret = camera_init();
  if (ret < 0) return ret;
  ret = camera_config_rgb565_wh(320, 240);
  if (ret < 0) return ret;
  previous = malloc(320u * 240u * 2u);
  if (!previous) return -ENOMEM;
  board_camera_sccb_read(0x503d, &old_pattern);
  board_camera_sccb_write(0x503d, 0x80);
  ret = hal_camera_open(&cfg);
  if (ret == 0) ret = hal_camera_start();
  if (ret < 0) goto cleanup;

  for (frame = -8; frame < (int)samples; frame++)
    {
      unsigned rows = 0;
      uint16_t low = UINT16_MAX;
      uint16_t high = 0;
      ret = hal_camera_acquire(&lease, 5000);
      if (ret < 0) goto cleanup;
      if (frame >= 0)
        {
          for (int y = 0; y < 240; y++)
            if (memcmp(lease.data, lease.data + (size_t)y * 640u, 640u))
              rows++;
          for (int x = 0; x < 320; x++)
            {
              uint16_t color = (lease.data[x * 2] << 8) |
                                lease.data[x * 2 + 1];
              if (color < low) low = color;
              if (color > high) high = color;
            }
          if (low == high) rows++;
          if (frame > 0 && memcmp(previous, lease.data, lease.length))
            changed_frames++;
          memcpy(previous, lease.data, lease.length);
          bad_rows += rows;
          printf("[DvpPhase] frame=%d bytes=%u bad_rows=%u range=%04x..%04x\n",
                 frame + 1, (unsigned)lease.length, rows, low, high);
        }
      hal_camera_release(&lease);
    }
  extern void esp32s3_cam_dvp_audit(void);
  esp32s3_cam_dvp_audit();
  hal_camera_get_health(&health);
  printf("[DvpPhase] frames=%u bad_rows=%u changed=%u short=%lu "
         "lease_err=%lu overflow=%lu held=%02x\n", samples, bad_rows, changed_frames,
         (unsigned long)health.short_frames,
         (unsigned long)health.lease_errors,
         (unsigned long)health.done_overflow, health.lease_held_mask);
  ret = bad_rows || changed_frames || health.lease_errors ||
        health.done_overflow || health.lease_held_mask ? -EIO : 0;
cleanup:
  if (lease.valid) hal_camera_release(&lease);
  hal_camera_close();
  board_camera_sccb_write(0x503d, old_pattern);
  free(previous);
  camera_shutdown();
  printf("[DvpPhase] result=%d\n", ret);
  return ret;
}


static void *camera_preview_loop(void *arg)
{
  volatile bool eoi = false;
  int ret;
  int fails = 0;
  int dfails = 0;

  (void)arg;

  if (s_preview_hd)
    {
      bool reconfigured = false;

      esp32s3_cam_dvp_set_stream(false);
      esp32s3_cam_dvp_set_quiet(true);
      lcd_put_rgb565_set_sharpen(false);

      if (s_hd_preview_frame == NULL)
        {
          s_hd_preview_frame = (uint8_t *)esp32s3_psram_static_alloc(
                                                    CAM_HD_PREVIEW_SIZE);
        }

      if (s_hd_preview_frame == NULL)
        {
          printf("[Cam-HD] preview buffer alloc failed (%u bytes)\n",
                 (unsigned)CAM_HD_PREVIEW_SIZE);
          s_preview_run = false;
          lcd_put_rgb565_set_sharpen(true);
          esp32s3_cam_dvp_set_quiet(false);
          return NULL;
        }

      /* 顺序对齐 CLI 已验证路径（plant cam jpeg）：先配传感器
       * （camera_config_jpeg_wh 内部已 set_jpeg(true)，DVP init 时仍按
       * 变长 JPEG 建链），传感器流稳定后再建 DMA/中断。实测反过来
       * （init 在 config 前）冷启动首帧窗口拿不到完整 JPEG（-61）。 */

      if (camera_config_jpeg_wh(CAM_HD_JPEG_W, CAM_HD_JPEG_H) < 0)
        {
          printf("[Cam-HD] JPEG config failed\n");
          s_preview_run = false;
          lcd_put_rgb565_set_sharpen(true);
          esp32s3_cam_dvp_set_quiet(false);
          return NULL;
        }

      esp32s3_cam_dvp_set_jpeg(true);
      if (esp32s3_cam_dvp_init() < 0)
        {
          printf("[Cam-HD] DVP init failed\n");
          s_preview_run = false;
          lcd_put_rgb565_set_sharpen(true);
          esp32s3_cam_dvp_set_quiet(false);
          return NULL;
        }

      while (s_preview_run)
        {
          const uint8_t *jpeg;
          uint8_t *snapshot;
          size_t snapshot_size;
          size_t jpeg_len;

          if (s_preview_freeze)
            {
              s_preview_idle = true;
              usleep(20000);
              continue;
            }

          s_preview_idle = false;

          ret = esp32s3_cam_dvp_start();
          if (ret == 0)
            {
              ret = esp32s3_cam_dvp_capture(NULL, NULL, &eoi);
            }

          esp32s3_cam_dvp_stop();

          if (ret <= 0)
            {
              fails++;
              printf("[Cam-HD] JPEG capture failed: %d (连续 %d 次)\n",
                     ret, fails);

              /* 冷启动自愈：POR 后首轮 JPEG 配置下传感器 DVP 不出流
               * （vsync 死、DMA 零字节；同 boot 第二次全量配置即恢复，
               * 已实测验证）。首个失败立刻重跑一次完整配置，代价 ~1.2s，
               * 只在本会话首次失败时触发一次。 */

              if (!reconfigured)
                {
                  reconfigured = true;

                  /* 冷启动自愈 = 复刻 CLI 已验证的"两段式"序列：
                   * 第一次失败后 pins_release 路径里的 camera_shutdown()
                   * （DVP stop + 传感器 0x3008=0x82 软复位 + 20ms 稳定）
                   * 是第二次配置能出流的必要前置——只重配置不复位无效
                   * （v52h 实测：裸重配后依旧 chunks=0；CLI 带 shutdown
                   * 的第二次 jpeg 即成功）。 */

                  /* 冷启动自愈 v2 = 复刻唯一实测成功的冷启动序列
                   * （v52_rgb_cold: RGB565 配置 -> 传感器出流 -> JPEG
                   * 配置 -> 首抓即得 81KB 完整帧）：shutdown 后先
                   * RGB565 QVGA 让传感器真正出流 ~800ms（RGB565 模式
                   * VSYNC 实测有沿，DVP 输出链路被唤醒），再切回 JPEG。
                   * 只 shutdown+重配 JPEG 无效（v52j 实测 chunks=0）。 */

                  printf("[Cam-HD] 冷启动锁死，RGB565 热身自愈\n");
                  camera_shutdown();
                  camera_config_rgb565_qvga();
                  usleep(800 * 1000);
                  if (camera_config_jpeg_wh(CAM_HD_JPEG_W,
                                            CAM_HD_JPEG_H) == 0)
                    {
                      esp32s3_cam_dvp_set_jpeg(true);
                      esp32s3_cam_dvp_reinit_hw();
                      fails = 0;
                      continue;
                    }
                }

              if (fails >= 5)
                {
                  /* 2026-09-27 (v54d): never kill the preview thread on
                   * -ENODATA.  Dark scenes stretch the JPEG frame period
                   * past the capture silence window, so bursts of empty
                   * captures are normal; keep polling at a calm rate and
                   * paint whenever a frame lands instead of leaving a
                   * dead viewfinder until page re-entry. */
                  fails = 0;
                  usleep(500 * 1000);
                  continue;
                }

              usleep(100 * 1000);
              continue;
            }

          fails = 0;
          jpeg = esp32s3_cam_dvp_get_frame();
          jpeg_len = (size_t)ret;
          snapshot = esp32s3_cam_dvp_get_shadow();
          snapshot_size = esp32s3_cam_dvp_get_shadow_size();

          if (jpeg == NULL || snapshot == NULL || jpeg_len == 0 ||
              jpeg_len > snapshot_size)
            {
              printf("[Cam-HD] bad snapshot: len=%u cap=%u\n",
                     (unsigned)jpeg_len, (unsigned)snapshot_size);
              continue;
            }

          /* ``stop`` can leave the CPU data cache holding the old zero-filled
           * PSRAM lines even though GDMA wrote the JPEG to physical memory.
           * Invalidate now, immediately before the CPU copy/decoder read. */

          esp32s3_cam_dvp_invalidate(jpeg, jpeg_len);

          /* 2048 帧尾截断是常态（逐帧随机）：严格 JDR_OK 的完整率随场景
           * 在 0~50% 波动，只收完整帧会让取景框长时间空白。改为接受
           * 解码覆盖 >=760/768 行的帧（缺失只在底部 8 行右段，占 0.5%，
           * 上屏与 AI 诊断无感；上传原图服务器 Pillow 可正常解码）。
           * 中段损坏（覆盖远低于此）仍丢弃重抓。 */

          /* 2026-09-25: validate on rxbuf first, then snapshot.  DMA is
           * stopped so rxbuf is stable; shadow must only ever hold an
           * accepted frame, otherwise a corrupt in-flight frame at
           * shutter-freeze time overwrites shadow while s_hd_jpeg still
           * points at it -> unvalidated corrupt JPEG gets uploaded. */
          if (cam_hd_jpeg_decode(jpeg, jpeg_len) < 0 &&
              s_hd_decoded_bottom < CAM_HD_PREVIEW_H - 8)
            {
              /* 取证（每次预览会话只落盘一次）：坏帧存 SD，离线看
               * 损坏位置（头/中/尾）；同时打 SOI/EOI 扫描结果。 */

              if (!s_hd_bad_dumped)
                {
                  s_hd_bad_dumped = true;
                  cam_hd_dump_bad_frame(jpeg, jpeg_len);
                }

              /* 解码失败也计入失败预算：偶发重抓无碍，连续失败（如镜头
               * 排线松）必须退出，否则空转热循环烧 CPU + 刷屏日志。 */

              dfails++;
              if (dfails >= 30)
                {
                  printf("[Cam-HD] 连续 %d 次解码失败，放弃预览\n",
                         dfails);
                  break;
                }

              usleep(50 * 1000);
              continue;
            }

          dfails = 0;

          /* 单色帧照样上屏（孩子能看到"黑了/红了"知道镜头被挡），
           * 但不进 shadow、不作快门快照 —— shadow 里永远保留最后一张
           * 有信息量的有效帧。 */

          {
            bool mono = cam_hd_frame_is_mono();

            if (mono)
              {
                s_hd_mono_streak++;
                if (s_hd_mono_streak == 3)
                  {
                    printf("[Cam-HD] 连续单色帧（镜头被挡？），"
                           "上屏但不留档\n");
                  }
              }
            else
              {
                s_hd_mono_streak = 0;
                memcpy(snapshot, jpeg, jpeg_len);
              }

            if (lcd_put_rgb565_cover(s_hd_preview_frame,
                                     CAM_HD_PREVIEW_W, CAM_HD_PREVIEW_H,
                                     s_preview_x, s_preview_y,
                                     s_preview_w, s_preview_h) == 0)
              {
                if (!mono)
                  {
                    s_hd_jpeg = snapshot;
                    s_hd_jpeg_len = jpeg_len;
                  }

                s_preview_seq++;
              }
          }

          usleep(2000);
        }

      esp32s3_cam_dvp_stop();
      lcd_put_rgb565_set_sharpen(true);
      esp32s3_cam_dvp_set_quiet(false);
      return NULL;
    }

  return camera_dvp_preview_loop(arg);

  /* OV3660 RGB565 大端输出（2.49 彩条实锤）：强制字节交换（防任何残留/误设） */

  esp32s3_cam_dvp_set_swap(true);
  esp32s3_cam_dvp_set_quiet(true);
  s_prev_reframe = false;   /* 滚动流 */
  s_preview_min_ms = 0;     /* v167：不限速。实测采集 156ms + 处理 ~150ms，
                             * 循环自己就是 ~300ms/帧，再限速只会更慢。 */
  esp32s3_cam_dvp_set_stream(!s_prev_reframe);
  esp32s3_cam_dvp_set_stream_direct(false);  /* 走 shadow 拷贝路径 */
  lcd_put_rgb565_set_sharpen(false);   /* 预览走快路径，单帧拍照仍锐化 */
  esp32s3_cam_dvp_init();
  if (!s_prev_reframe)
    {
      esp32s3_cam_dvp_start();
    }

  while (s_preview_run)
    {
      if (s_preview_freeze)
        {
          s_preview_idle = true;   /* v175：在这里置位 = 上一轮写屏已结束 */
          usleep(20000);
          continue;
        }

      s_preview_idle = false;

      if (s_preview_hold)
        {
          /* 诊断：不采集，直接重复写上一帧（shadow 保持不动） */

          lcd_put_rgb565_cover(esp32s3_cam_dvp_get_frame(),
                               s_frame_w, s_frame_h,
                               s_preview_x, s_preview_y,
                               s_preview_w, s_preview_h);
          usleep(100 * 1000);
          continue;
        }

      if (s_prev_reframe)
        {
          ret = esp32s3_cam_dvp_start();
          if (ret == 0)
            {
              ret = esp32s3_cam_dvp_capture(NULL, NULL, &eoi);
            }

          esp32s3_cam_dvp_stop();
        }
      else
        {
          ret = esp32s3_cam_dvp_capture(NULL, NULL, &eoi);
        }

      if (ret < 0)
        {
          /* ⚠️ 2026-09-18：采集失败不要立刻退出。这里原来是裸 break ——
           * 偶发一次超时（-110）就会让预览线程静默死掉，取景框一片空白
           * 且没有任何日志，只能靠猜、还要重启板卡。改成：连续失败才放弃，
           * 每次都把错误码打出来。 */

          fails++;
          printf("[Cam] preview capture failed: %d (连续 %d 次)\n",
                 ret, fails);
          if (fails >= 5)
            {
              printf("[Cam] preview giving up after %d failures\n", fails);
              break;
            }

          usleep(200 * 1000);
          continue;
        }

      fails = 0;
      s_preview_seq++;

      /* 直写 LCD 取景框区域（v65：640x480 -> 480x256 是降采样，最近邻
       * 查表会和叶片细纹拍频出摩尔纹——用户实测"油膜花纹+抖动"。
       * 换回 4-tap 双线性（自带抗混叠），花纹消除；代价每帧多 ~100ms。 */

      {
        static uint32_t s_tm_cap, s_tm_lcd, s_tm_n, s_tm_t0, s_tm_diag;
        static uint32_t s_prev_iter_t0;
        static uint32_t s_sh_acc_bestx, s_sh_acc_besty, s_sh_acc;
        uint32_t t_iter = (uint32_t)clock_systime_ticks();
        uint32_t ta = t_iter;
        uint32_t tb;
        uint32_t tc;
        uint32_t td = ta;

        if (s_prev_verify)
          {
            /* 帧校验 + 静态跳过：
             * 1) 与上一"已接受"帧做两段式配准：先在 ±16 范围 4 像素步长
             *    粗搜，再在最优位置 ±3 细化。实测错位可达 16x9 像素，
             *    只有大范围搜索才认得出这种坏帧（旧版 ±4 会漏掉）。
             * 2) sad0 超阈值且平移能解释差异 → 判为错位帧，不上屏。
             * 3) sad0 极小（画面几乎没变）→ 跳过本次写屏，静止场景
             *    彻底没有撕裂（面板保持上一帧）。 */

            static uint16_t s_vf_prev[128];
            static uint32_t s_vf_avg;
            static uint32_t s_vf_ok;
            static uint32_t s_vf_bad;
            static uint32_t s_vf_skip;
            static uint32_t s_vf_jump;
            static int s_vf_lastx;
            static int s_vf_lasty;
            static bool s_vf_have;
            const uint16_t *fv =
              (const uint16_t *)esp32s3_cam_dvp_get_frame();
            int vw = s_frame_w;
            int vh = s_frame_h;

            if (fv != NULL && vw > 64 && vh > 64)
              {
                uint32_t sad0 = 0;
                uint32_t best = 0xffffffffu;
                int bx = 0;
                int by = 0;
                int cx;
                int cy;
                int dx;
                int dy;
                int k;

                for (k = 0; k < 128; k++)
                  {
                    int px = ((k * 37) % (vw - 40)) + 20;
                    int py = ((k * 91) % (vh - 40)) + 20;
                    uint16_t cur = fv[(size_t)py * vw + px];
                    uint32_t lc = ((cur >> 11) & 31u) * 8u +
                                  ((cur >> 5) & 63u) * 4u + (cur & 31u) * 8u;
                    uint32_t lp = s_vf_prev[k];

                    sad0 += (lc > lp) ? (lc - lp) : (lp - lc);
                  }

                for (dy = -32; dy <= 32; dy += 4)
                  {
                    for (dx = -32; dx <= 32; dx += 4)
                      {
                        uint32_t sad = 0;

                        for (k = 0; k < 128; k++)
                          {
                            int px = ((k * 37) % (vw - 40)) + 20;
                            int py = ((k * 91) % (vh - 40)) + 20;
                            uint16_t cur =
                              fv[(size_t)(py + dy) * vw + (px + dx)];
                            uint32_t lc = ((cur >> 11) & 31u) * 8u +
                                          ((cur >> 5) & 63u) * 4u +
                                          (cur & 31u) * 8u;
                            uint32_t lp = s_vf_prev[k];

                            sad += (lc > lp) ? (lc - lp) : (lp - lc);
                          }

                        if (sad < best)
                          {
                            best = sad;
                            bx = dx;
                            by = dy;
                          }
                      }
                  }

                cx = bx;
                cy = by;

                for (dy = cy - 3; dy <= cy + 3; dy++)
                  {
                    for (dx = cx - 3; dx <= cx + 3; dx++)
                      {
                        uint32_t sad = 0;

                        for (k = 0; k < 128; k++)
                          {
                            int px = ((k * 37) % (vw - 40)) + 20;
                            int py = ((k * 91) % (vh - 40)) + 20;
                            uint16_t cur =
                              fv[(size_t)(py + dy) * vw + (px + dx)];
                            uint32_t lc = ((cur >> 11) & 31u) * 8u +
                                          ((cur >> 5) & 63u) * 4u +
                                          (cur & 31u) * 8u;
                            uint32_t lp = s_vf_prev[k];

                            sad += (lc > lp) ? (lc - lp) : (lp - lc);
                          }

                        if (sad < best)
                          {
                            best = sad;
                            bx = dx;
                            by = dy;
                          }
                      }
                  }

                sad0 /= 128u;
                best /= 128u;

                if (s_vf_have)
                  {
                    uint32_t lim = s_prev_thresh;

                    if (s_vf_avg > 0 && s_vf_avg * 2u > lim)
                      {
                        lim = s_vf_avg * 2u;
                      }

                    /* 判据 A：内容差异大且能被平移解释 → 错位帧 */

                    /* 改善必须显著（降到 70% 以下）才算"平移能解释"，
                     * 否则 ±32 大范围搜索会因多重比较产生假匹配，
                     * 把好帧也全判成错位（v111 实测 48/48 全拒绝）。 */

                    if (sad0 > lim && best * 10u < sad0 * 7u)
                      {
                        s_vf_bad++;
                        if ((s_vf_bad + s_vf_ok) >= 48u)
                          {
                            printf("[Cam] 帧校验: 错位拒绝 %lu/%lu 突变 %lu "
                                   "跳过 %lu",
                                   (unsigned long)s_vf_bad,
                                   (unsigned long)(s_vf_bad + s_vf_ok),
                                   (unsigned long)s_vf_jump,
                                   (unsigned long)s_vf_skip);
                            printf("\n");
                            s_vf_bad = 0;
                            s_vf_ok = 0;
                            s_vf_skip = 0;
                            s_vf_jump = 0;
                          }

                        s_tm_t0 = (uint32_t)clock_systime_ticks();
                        usleep(2000);
                        continue;
                      }

                    s_vf_lastx = bx;
                    s_vf_lasty = by;

                    s_vf_ok++;
                    s_vf_avg = (s_vf_avg * 7u + sad0) / 8u;

                    /* 静态跳过：画面几乎没变 → 不写面板（静止时零撕裂） */

                    if (sad0 < 2u && best >= sad0 && !camera_preview_refresh_pending())
                      {
                        s_vf_skip++;
                        s_tm_t0 = (uint32_t)clock_systime_ticks();
                        usleep(2000);
                        continue;
                      }
                  }

                s_vf_have = true;

                for (k = 0; k < 128; k++)
                  {
                    int px = ((k * 37) % (vw - 40)) + 20;
                    int py = ((k * 91) % (vh - 40)) + 20;
                    uint16_t cur = fv[(size_t)py * vw + px];

                    s_vf_prev[k] = ((cur >> 11) & 31u) * 8u +
                                   ((cur >> 5) & 63u) * 4u + (cur & 31u) * 8u;
                  }
              }
          }

        {
          /* 过曝量化（v144）：统计"通道顶满"像素占比。若持续 >3%，
           * 说明曝光/增益偏高，画面高光被削平 → 运动时呈"油膜"观感。 */

          static uint32_t s_clip_n;
          static uint32_t s_clip_sum;
          static uint32_t s_clip_tot;
          const uint16_t *fc = (const uint16_t *)esp32s3_cam_dvp_get_frame();

          if (fc != NULL)
            {
              size_t npc = (size_t)s_frame_w * (size_t)s_frame_h;
              size_t ic;
              uint32_t clipc = 0;

              for (ic = 0; ic < npc; ic += 37u)
                {
                  uint16_t pc = fc[ic];

                  if (((pc >> 11) & 31u) >= 30u && ((pc >> 5) & 63u) >= 62u &&
                      (pc & 31u) >= 30u)
                    {
                      clipc++;
                    }

                  s_clip_tot++;
                }

              s_clip_sum += clipc;
              s_clip_n++;
              if (s_clip_n >= 32u)
                {
                  uint32_t tot = s_clip_tot ? s_clip_tot : 1u;

                  printf("[Cam] 过曝(顶满)像素: %lu.%lu%%",
                         (unsigned long)(s_clip_sum * 100u / tot),
                         (unsigned long)((s_clip_sum * 1000u / tot) % 10u));
                  printf("\n");
                  s_clip_sum = 0;
                  s_clip_tot = 0;
                  s_clip_n = 0;
                }
            }
        }

        {
          /* 帧间位移估计（加强版）：±8 像素 2D 搜索 + 512 采样点。
           * 静止场景下"最佳位移"若稳定在 (0,0) → 对齐良好；
           * 若每帧乱跳且 |d| 有数像素 → 采集帧边界在漂。 */

          static uint16_t s_sh_prev[128];
          static uint32_t s_sh_n;
          static uint32_t s_sh_acc;
          static uint32_t s_sh_adx;
          static uint32_t s_sh_ady;
          static uint32_t s_sh_mdx;
          static uint32_t s_sh_mdy;
          const uint16_t *fsh =
            (const uint16_t *)esp32s3_cam_dvp_get_frame();
          int sw = s_frame_w;
          int sh = s_frame_h;

          if (fsh != NULL && sw > 48 && sh > 48 && s_sh_n > 0)
            {
              int dx;
              int dy;
              int bdx = 0;
              int bdy = 0;
              uint32_t bsad = 0xffffffffu;
              uint32_t sad0 = 0;

              for (dy = -4; dy <= 4; dy++)
                {
                  for (dx = -4; dx <= 4; dx++)
                    {
                      uint32_t sad = 0;
                      int k;

                      for (k = 0; k < 128; k++)
                        {
                          int px = ((k * 37) % (sw - 40)) + 16;
                          int py = ((k * 91) % (sh - 40)) + 16;
                          uint16_t cur = fsh[(size_t)(py + dy) * sw +
                                             (px + dx)];
                          uint32_t lc = ((cur >> 11) & 31u) * 8u +
                                        ((cur >> 5) & 63u) * 4u +
                                        (cur & 31u) * 8u;
                          uint32_t lp = s_sh_prev[k];

                          sad += (lc > lp) ? (lc - lp) : (lp - lc);
                        }

                      if (dx == 0 && dy == 0)
                        {
                          sad0 = sad;
                        }

                      if (sad < bsad)
                        {
                          bsad = sad;
                          bdx = dx;
                          bdy = dy;
                        }
                    }
                }

              {
                uint32_t adx = (bdx < 0) ? (uint32_t)-bdx : (uint32_t)bdx;
                uint32_t ady = (bdy < 0) ? (uint32_t)-bdy : (uint32_t)bdy;

                s_sh_adx += adx;
                s_sh_ady += ady;
                if (adx > s_sh_mdx)
                  {
                    s_sh_mdx = adx;
                  }

                if (ady > s_sh_mdy)
                  {
                    s_sh_mdy = ady;
                  }

                s_sh_acc++;
                if (s_sh_acc >= 32)
                  {
                    printf("[PrevShift] dx:avg=%u.%u max=%u | dy:avg=%u.%u "
                           "max=%u | sad0=%u sadmin=%u last=(%d,%d)",
                           (unsigned)(s_sh_adx * 10u / s_sh_acc / 10u),
                           (unsigned)((s_sh_adx * 10u / s_sh_acc) % 10u),
                           (unsigned)s_sh_mdx,
                           (unsigned)(s_sh_ady * 10u / s_sh_acc / 10u),
                           (unsigned)((s_sh_ady * 10u / s_sh_acc) % 10u),
                           (unsigned)s_sh_mdy,
                           (unsigned)(sad0 / 128u), (unsigned)(bsad / 128u),
                           bdx, bdy);
                    printf("\n");
                    s_sh_adx = 0;
                    s_sh_ady = 0;
                    s_sh_mdx = 0;
                    s_sh_mdy = 0;
                    s_sh_acc = 0;
                  }
              }
            }

          if (fsh != NULL && sw > 48 && sh > 48)
            {
              int k;

              for (k = 0; k < 128; k++)
                {
                  int px = ((k * 37) % (sw - 40)) + 16;
                  int py = ((k * 91) % (sh - 40)) + 16;
                  uint16_t cur = fsh[(size_t)py * sw + px];

                  s_sh_prev[k] = ((cur >> 11) & 31u) * 8u +
                                 ((cur >> 5) & 63u) * 4u + (cur & 31u) * 8u;
                }

              s_sh_n++;
            }
        }

        {
          /* 逐帧诊断：静止场景帧间差应≈0（仅噪声）；若很大说明传感器
           * 曝光/时序在抖（QVGA PLL 配置可疑）。 */

          static uint32_t s_dg_prev[64];
          static uint32_t s_dg_sum;
          static uint32_t s_dg_r;
          static uint32_t s_dg_g;
          static uint32_t s_dg_b;
          static uint32_t s_dg_diff;
          static uint32_t s_dg_n;
          const uint16_t *fdiag =
            (const uint16_t *)esp32s3_cam_dvp_get_frame();
          size_t npix = (size_t)s_frame_w * (size_t)s_frame_h;

          if (fdiag != NULL && npix > 64)
            {
              uint32_t dsum = 0;
              uint32_t ddiff = 0;
              int di;

              for (di = 0; di < 64; di++)
                {
                  size_t idx = (npix / 64u) * (size_t)di;
                  uint16_t pxd = fdiag[idx];
                  uint32_t lum = ((pxd >> 11) & 31u) * 8u +
                                 ((pxd >> 5) & 63u) * 4u + (pxd & 31u) * 8u;

                  dsum += lum;
                  s_dg_r += (pxd >> 11) & 31u;
                  s_dg_g += (pxd >> 5) & 63u;
                  s_dg_b += pxd & 31u;
                  if (s_dg_n > 0)
                    {
                      ddiff += (lum > s_dg_prev[di]) ? (lum - s_dg_prev[di])
                                                     : (s_dg_prev[di] - lum);
                    }

                  s_dg_prev[di] = lum;
                }

              s_dg_sum += dsum;
              s_dg_diff += ddiff;
              s_dg_n++;
              if (s_dg_n >= 32)
                {
                  printf("[PrevDiag] R=%u G=%u B=%u diff=%u",
                         (unsigned)(s_dg_r / 32u / 64u),
                         (unsigned)(s_dg_g / 32u / 64u),
                         (unsigned)(s_dg_b / 32u / 64u),
                         (unsigned)(s_dg_diff / 32u / 64u));
                  printf("\n");
                  s_dg_sum = 0;
                  s_dg_r = 0;
                  s_dg_g = 0;
                  s_dg_b = 0;
                  s_dg_diff = 0;
                  s_dg_n = 0;
                }
            }
        }

        td = (uint32_t)clock_systime_ticks();

        {
          /* v156：B 模式快照 —— 1:1 写取景框，只写变化的行，写入期间
           * DISPOFF（写完整帧才需要，写几行时闪断 <1ms 不可见）。
           * 静止画面完全不变 → 一次都不写 → 零撕裂、零闪断。 */

          extern int lcd_put_rgb565_snapshot(const uint8_t *fb,
                                             int sw, int sh,
                                             int dx, int dy,
                                             int dw, int dh);

          int draw=lcd_put_rgb565_snapshot(esp32s3_cam_dvp_get_frame(),
                                  s_frame_w, s_frame_h,
                                  s_preview_x, s_preview_y,
                                  s_preview_w, s_preview_h);
          int previous=atomic_exchange(&s_display_error,draw);
          if(draw<0&&draw!=previous)printf("[CamPreview] display failed=%d\n",draw);
          if(draw==0&&previous<0)printf("[CamPreview] display recovered\n");
        }

        tb = (uint32_t)clock_systime_ticks();

        /* 小睡一拍：给 UI 线程留呼吸；滚动采集下写屏是主要耗时 */

        usleep(2000);
        tc = (uint32_t)clock_systime_ticks();

        s_tm_cap += ta - s_tm_t0;   /* 上帧结束->本帧取回 = EOF 等待+memcpy */
        s_tm_diag += td - ta;       /* 诊断/帧校验/位移估计 */
        s_tm_lcd += tb - td;        /* 缩放+写屏 */
        s_tm_n++;
        s_tm_t0 = tc;

        /* 取景更新限速：撕裂只在"写入 vs 面板扫描"重叠时出现，
         * 降低更新率 = 线性减少撕裂出现次数（代价是取景更顿）。 */

        if (s_preview_min_ms > 0)
          {
            uint32_t el = tc - t_iter;
            uint32_t want = (uint32_t)((s_preview_min_ms + 9) / 10);
            if (want > el)
              {
                uint32_t d = want - el;
                usleep(d * 10000);
                s_tm_t0 += d;
                tc += d;
              }
          }

        s_prev_iter_t0 = tc;
        (void)s_prev_iter_t0;
        if (s_tm_n >= 32)
          {
            printf("[Prev] %ums/帧: 采集 %ums, 诊断 %ums, 缩放+写屏 %ums (%.1f fps)\n",
                   (unsigned)((s_tm_cap + s_tm_diag + s_tm_lcd) * 10 / s_tm_n),
                   (unsigned)(s_tm_cap * 10 / s_tm_n),
                   (unsigned)(s_tm_diag * 10 / s_tm_n),
                   (unsigned)(s_tm_lcd * 10 / s_tm_n),
                   (double)s_tm_n * 100.0 /
                     (double)(s_tm_cap + s_tm_diag + s_tm_lcd + 1));
            s_tm_cap = 0;
            s_tm_diag = 0;
            s_tm_lcd = 0;
            s_tm_n = 0;
          }
      }
    }

  esp32s3_cam_dvp_stop();
  lcd_put_rgb565_set_sharpen(true);
  esp32s3_cam_dvp_set_stream_direct(false);
  esp32s3_cam_dvp_set_stream(false);
  esp32s3_cam_dvp_set_quiet(false);
  return NULL;
}

/* 快门专用：JPEG 单拍 + TJpgDec 严格解码验证 + 失败重抓。
 *
 * 2048 帧尾截断是逐帧随机竞态（v1.3.34 寄存器提速后完整率 ~25-50%），
 * 截断帧 jd_decomp 报 JDR_INTR —— 解码不过就重抓，直到拿到完整帧。
 * 成功后把解码结果（1024x768）定格到取景框 (x,y,w,h)，返回 shadow 里
 * 的原始 2048 字节（所见即所传）。调用前必须先 camera_preview_stop()。 */

int camera_hd_jpeg_capture_verified(const uint8_t **jpeg, size_t *len,
                                    int x, int y, int w, int h,
                                    int max_tries)
{
  volatile bool eoi = false;
  int attempt;
  int ret;

  if (jpeg == NULL || len == NULL || max_tries < 1)
    {
      return -EINVAL;
    }

  *jpeg = NULL;
  *len = 0;

  if (s_hd_preview_frame == NULL)
    {
      s_hd_preview_frame = (uint8_t *)esp32s3_psram_static_alloc(
                              CAM_HD_PREVIEW_SIZE);
    }

  if (s_hd_preview_frame == NULL)
    {
      return -ENOMEM;
    }

  hal_camera_close();

  /* 初始化 DVP 前必须先进入 JPEG 模式（与 HD 预览同序） */

  esp32s3_cam_dvp_set_jpeg(true);
  if (esp32s3_cam_dvp_init() < 0)
    {
      printf("[Cam-HD] shot: DVP init failed\n");
      return -EIO;
    }

  if (camera_config_jpeg_wh(CAM_HD_JPEG_W, CAM_HD_JPEG_H) < 0)
    {
      printf("[Cam-HD] shot: JPEG config failed\n");
      return -EIO;
    }

  bool reinited = false;

  for (attempt = 1; attempt <= max_tries; attempt++)
    {
      const uint8_t *fb;
      uint8_t *snapshot;
      size_t cap;

#ifdef CONFIG_PLANT_UI
      int quiet=ui_app_render_pause(true);
      if(quiet<0)return quiet;
#endif
      ret = esp32s3_cam_dvp_start();
      if (ret == 0)
        {
          ret = esp32s3_cam_dvp_capture(NULL, NULL, &eoi);
        }

      esp32s3_cam_dvp_stop();

      if (ret <= 0)
        {
#ifdef CONFIG_PLANT_UI
          ui_app_render_pause(false);
#endif
          printf("[Cam-HD] shot #%d capture failed: %d\n", attempt, ret);

          /* 2026-09-27 (v55c): 冷启动 JPEG DMA 锁死（chunks=0）时，
           * 第一次失败立刻重跑完整硬件配置（GPIO/LCD_CAM/DMA 链），
           * 传感器此时正持续 RGB565 出流 = 实测唯一可靠的恢复序列
           * 的最后一个环节。 */

          if (!reinited)
            {
              reinited = true;
              printf("[Cam-HD] shot: 触发 hw reinit 自愈\n");
              esp32s3_cam_dvp_reinit_hw();
            }

          continue;
        }

      fb = esp32s3_cam_dvp_get_frame();
      snapshot = esp32s3_cam_dvp_get_shadow();
      cap = esp32s3_cam_dvp_get_shadow_size();

      if (fb == NULL || snapshot == NULL || (size_t)ret > cap)
        {
#ifdef CONFIG_PLANT_UI
          ui_app_render_pause(false);
#endif
          printf("[Cam-HD] shot #%d bad snapshot\n", attempt);
          continue;
        }

      esp32s3_cam_dvp_invalidate(fb, (size_t)ret);
      memcpy(snapshot, fb, (size_t)ret);
#ifdef CONFIG_PLANT_UI
      ui_app_render_pause(false);
#endif

      if (cam_hd_jpeg_decode(snapshot, (size_t)ret) == 0)
        {
          lcd_put_rgb565_cover(s_hd_preview_frame,
                               CAM_HD_PREVIEW_W, CAM_HD_PREVIEW_H,
                               x, y, w, h);
          *jpeg = snapshot;
          *len = (size_t)ret;
          printf("[Cam-HD] shot #%d verified: len=%u fnv=%08x\n",
                 attempt, (unsigned)*len,
                 (unsigned)cam_hd_jpeg_hash(*jpeg, *len));
          return 0;
        }

      printf("[Cam-HD] shot #%d truncated (%u/%u rows), retry\n",
             attempt, s_hd_decoded_bottom, (unsigned)CAM_HD_PREVIEW_H);

      /* Consecutive decode errors can leave receiver state unhealthy.
       * Re-arm from the known hardware mode before retrying, rather than
       * repeatedly accepting bytes from the same failed receive state.
       */
      if ((attempt % 2) == 0 && attempt < max_tries)
        {
          esp32s3_cam_dvp_reinit_hw();
        }

    }

  printf("[Cam-HD] shot: no usable frame after %d tries\n", max_tries);
  return -ENODATA;
}

int camera_preview_start(int x, int y, int w, int h)
{
  if (s_preview_run)
    {
      return 0;
    }

  cam_mask_take_snapshot();

  s_preview_hd = false;
  s_frame_w = CAM_PREVIEW_W;
  s_frame_h = CAM_PREVIEW_H;

  /* 回到预览几何：拍照后传感器停在 320×240，若直接启动预览线程，直写会
   * 把 320 宽的帧当 160×120 用 → 画面错乱。已在该模式时零开销返回。 */

  if (camera_mode_preview() < 0)
    {
      printf("[Cam] preview: mode switch failed, keep current geometry\n");
    }

  s_preview_x = x;
  s_preview_y = y;
  s_preview_w = w;
  s_preview_h = h;
  s_preview_seq = 0;
  s_preview_freeze = false;
  s_preview_run = true;

  if (pthread_create(&s_preview_tid, NULL, camera_preview_loop, NULL) != 0)
    {
      s_preview_run = false;
      return -1;
    }

  return 0;
}

int camera_preview_start_hd(int x, int y, int w, int h)
{
  if (s_preview_run)
    {
      return 0;
    }

  s_preview_hd = true;
  s_preview_x = x;
  s_preview_y = y;
  s_preview_w = w;
  s_preview_h = h;
  s_preview_seq = 0;
  s_preview_freeze = false;
  s_hd_jpeg = NULL;
  s_hd_jpeg_len = 0;
  s_preview_run = true;

  if (pthread_create(&s_preview_tid, NULL, camera_preview_loop, NULL) != 0)
    {
      s_preview_run = false;
      return -1;
    }

  return 0;
}

int camera_preview_start_rgb(int x, int y, int w, int h)
{
  if (s_preview_run)
    {
      return 0;
    }

  /* v61：640x480 RGB565 直采取景（VSYNC 对齐 DMA + 滚动快照），
   * 实测稳态 ~80ms/帧（12.5fps）；无 JPEG 编解码，没有中段损坏重抓。
   * 清晰度高于 512x384 的 JPEG 半尺度解码。快门时切 JPEG 1024x768
   * 冷拍（camera_hd_jpeg_capture_verified，静模式链路 100% 完好）。 */

  /* 2026-09-28：显示仍是 320x240 取景框（撕裂控制），但采集源回到
   * 640x480。原因：QVGA 分支是为提帧率改过 PLL 的，实测帧周期 ~200ms
   * 且静止画面曝光/时序在抖（用户反馈"画面跳来跳去"）；640x480 是
   * 长期验证稳定的配置（帧周期 136ms），缩放 2:1 写入量不变。 */

  /* 2026-09-28：取景采集回 QVGA 320x240。aligncheck 实测（静态场景、
   * 同一采集路径）640x480 帧间差 sad0=145-233、60-93% 行异常，而
   * 320x240 只有 sad0=5-71、0-32% —— QVGA 分支当年为 AEC 稳定调过
   * PLL（pclk_div 8->10），是明显更稳的一条路。显示仍是 320x240，
   * 1:1 直写、无需缩放。 */

  if (!s_rgb_qvga_ready)
    {
      if (camera_config_rgb565_wh(320, 240) < 0)
        {
          printf("[Cam] rgb preview: 320x240 config failed\n");
          return -EIO;
        }

      s_rgb_qvga_ready = true;
    }

  s_preview_hd = false;
  s_frame_w = 320;
  s_frame_h = 240;
  cam_mask_take_snapshot();
  s_preview_x = x;
  s_preview_y = y;
  s_preview_w = w;
  s_preview_h = h;
  camera_preview_invalidate();
  s_preview_seq = 0;
  s_preview_freeze = false;
  s_preview_run = true;

  if (pthread_create(&s_preview_tid, NULL, camera_preview_loop, NULL) != 0)
    {
      s_preview_run = false;
      return -1;
    }

  return 0;
}

/* 连续单色帧计数（镜头被挡提示用，见 cam_hd_frame_is_mono） */

unsigned camera_preview_mono_streak(void)
{
  return s_hd_mono_streak;
}

void camera_preview_set_verify(bool on)
{
  s_prev_verify = on;
  printf("[Cam] frame verify %s (thresh=%lu)\n", on ? "ON" : "OFF",
         (unsigned long)s_prev_thresh);
}

void camera_preview_set_hold(bool on)
{
  s_preview_hold = on;
  printf("[Cam] preview hold %s\n", on ? "ON（停采集，重复写同帧）" : "OFF");
}

void camera_preview_set_min_interval_ms(int ms)
{
  s_preview_min_ms = (ms < 0) ? 0 : ms;
  printf("[Cam] preview min interval = %d ms\n", s_preview_min_ms);
}

int camera_preview_set_min_interval_ms_get(void)
{
  return s_preview_min_ms;
}

int camera_preview_get_jpeg(const uint8_t **jpeg, size_t *len)
{
  if (jpeg == NULL || len == NULL)
    {
      return -EINVAL;
    }

  *jpeg = s_hd_jpeg;
  *len = s_hd_jpeg_len;
  if (s_hd_jpeg == NULL || s_hd_jpeg_len == 0)
    {
      return -ENODATA;
    }

  printf("[Cam-HD] capture ready: len=%u fnv=%08x\n",
         (unsigned)s_hd_jpeg_len,
         (unsigned)cam_hd_jpeg_hash(s_hd_jpeg, s_hd_jpeg_len));
  return 0;
}

int camera_preview_take(void)
{
  /* 冻结：预览线程停取帧，s_preview_latest 保留最后一帧（画面定格）。
   *
   * v175（用户报：点“相册”卡住 + 雪花屏）：原来只置标志就返回，而线程
   * 可能正在“写一整屏”的中途（现在取景框铺满整屏，一次写屏 ~250ms）：
   * 等于切页后它还会把相机画面盖到新页面上，两边交替刷 →
   * 花屏/卡住。现在同步等它真停下（最多 1s）再返回。 */

  int i;

  s_preview_freeze = true;

  for (i = 0; i < 200 && !s_preview_idle && s_preview_run; i++)
    {
      usleep(5 * 1000);
    }

  return (s_preview_idle || !s_preview_run) ? 0 : -ETIMEDOUT;
}

int camera_preview_resume(void)
{
  if(s_preview_freeze)camera_preview_invalidate();
  s_preview_idle = false;
  s_preview_freeze = false;
  return 0;
}

bool camera_preview_is_running(void)
{
  return s_preview_run;
}

int camera_preview_stop(void)
{
  if (!s_preview_run && s_preview_tid == 0)
    {
      return 0;
    }

  s_preview_run = false;
  pthread_join(s_preview_tid, NULL);
  s_preview_tid = 0;
  s_preview_idle = true;   /* v175：线程已退出，不再有人写屏 */
  return 0;
}

/* 返回最新完整帧（= 驱动 rxbuf，零拷贝）；帧序号递增即新帧 */

const uint8_t *camera_preview_get_frame(void)
{
  return esp32s3_cam_dvp_get_frame();
}

int camera_preview_get_frame_index(void)
{
  return (int)s_preview_seq;
}
#endif /* CONFIG_ESP32S3_CAM_DVP */

/****************************************************************************
 * Name: lcd_put_rgb565
 *
 * 直接写 LCD（绕过 LVGL 渲染器）：软件最近邻缩放 RGB565 帧 → LCDDEVIO_PUTAREA。
 *
 * 背景：本 LVGL 版本的 lv_image 渲染 RGB565 有 bug（缩放上半错乱、对象
 * 删除后区域不重绘导致"蓝白色残留遮挡"），摄像头/测试图改用直接写屏
 * （= 官方 dvp_spi_lcd 例程的 draw_bitmap 模式）。
 * 数据为小端 RGB565（st7789 驱动内部会转成大端送给面板）。
 *
 * 输入：fb=源帧(sw×sh×2，标准 LE RGB565 —— capture swap=on 后即此格式),
 * 目标区域 (dx,dy,dw,dh)。
 * 逐行缩放 + ioctl 写一行，内存占用仅一行缓冲。
 *
 * ⚠️ 2026-09-01 缩放质量升级：最近邻 → 双线性（2×2 加权平均）+ 源帧锐化。
 * 背景：160×120 放大 2.59× 到取景框，最近邻像素块感明显（用户反馈"模糊"）。
 * 实现：整数定点（256 缩放权重 + 移位），零除法逐像素（除法只在每帧
 * 预计算权重表时发生 dw+dh 次）；RGB565 逐通道（R5/G6/B5）插值防色偏。
 * 锐化：双线性角点在源帧上先做十字拉普拉斯 unsharp（补偿放大发虚），
 * 源帧完整在内存（rxbuf）→ 零额外缓冲；CPU 每帧 +~8ms。
 * 资源账：+1440B .bss（权重表 static，栈 4KB 放不下 2.4KB）；CPU 每帧
 * +~18ms@240MHz（SPI 提频 40MHz 后写屏 120→30ms，预算内）。 */

/* ⚠️ 2026-09-01 源帧十字拉普拉斯锐化（unsharp）：
 *   sharp = ((64+K)*v*4 - K*十字邻域和) / 256，K=48 → 强度 0.75（温和）。
 * 在 fb 上直接取 4 邻域（边缘 clamp），逐通道 R5/G6/B5 运算防色偏。
 * K 可调：48 温和（默认）；过大有光晕/噪点放大（binning 帧噪声低，可接受）。 */


/* 预览快路径开关：连续预览（stream）关锐化提帧率；单帧拍照保持默认锐化。 */

static bool s_lcd_sharpen = true;
static bool s_lcd_cover;      /* cover-fit: crop source instead of stretching */
static bool s_lcd_src_be;     /* 源为传感器原始大端序（免拷贝直通时用） */

void lcd_put_rgb565_set_sharpen(bool on)
{
  s_lcd_sharpen = on;
}
/* 缩放/锐化允许的最大【源】宽度（留足余量，当前最大源 = 拍照 320）：
 * s_shrow 每行要放 sw 个 RGB565 像素。 */

#define CAM_FRAME_MAX_W   640
#define CAM_SHARP_K   48

static inline uint16_t cam_sharp_px(const uint8_t *fb, int sw, int sh,
                                    int sx, int sy)
{
#define CAM_PX(X, Y) \
  (fb[((size_t)(Y) * sw + (X)) * 2] | (fb[((size_t)(Y) * sw + (X)) * 2 + 1] << 8))
  uint16_t v = CAM_PX(sx, sy);
  uint16_t vl = CAM_PX(sx > 0 ? sx - 1 : sx, sy);
  uint16_t vr = CAM_PX(sx + 1 < sw ? sx + 1 : sx, sy);
  uint16_t vu = CAM_PX(sx, sy > 0 ? sy - 1 : sy);
  uint16_t vd = CAM_PX(sx, sy + 1 < sh ? sy + 1 : sy);
  int r  = (v >> 11) & 31;
  int g  = (v >> 5) & 63;
  int b  = v & 31;
  int rl = (vl >> 11) & 31;
  int rr = (vr >> 11) & 31;
  int ru = (vu >> 11) & 31;
  int rd = (vd >> 11) & 31;
  int gl = (vl >> 5) & 63;
  int gr = (vr >> 5) & 63;
  int gu = (vu >> 5) & 63;
  int gd = (vd >> 5) & 63;
  int bl = vl & 31;
  int br = vr & 31;
  int bu = vu & 31;
  int bd = vd & 31;
  int nr = ((448 * r - CAM_SHARP_K * (rl + rr + ru + rd)) >> 8);
  int ng = ((448 * g - CAM_SHARP_K * (gl + gr + gu + gd)) >> 8);
  int nb = ((448 * b - CAM_SHARP_K * (bl + br + bu + bd)) >> 8);
#undef CAM_PX

  if (nr < 0)
    {
      nr = 0;
    }
  else if (nr > 31)
    {
      nr = 31;
    }

  if (ng < 0)
    {
      ng = 0;
    }
  else if (ng > 63)
    {
      ng = 63;
    }

  if (nb < 0)
    {
      nb = 0;
    }
  else if (nb > 31)
    {
      nb = 31;
    }

  return (uint16_t)((nr << 11) | (ng << 5) | nb);
}

int lcd_put_rgb565(const uint8_t *fb, int sw, int sh,
                   int dx, int dy, int dw, int dh)
{
  static int s_lcdfd = -1;
  static uint8_t *s_fbuf;
  int fd;
  struct lcddev_area_s area;
  static int16_t s_sx0[480];   /* target x -> source integer x0 (bilinear) */
  static uint8_t s_fx8[480];   /* target x -> source fraction x256 */
  static uint8_t s_shrow[2][CAM_FRAME_MAX_W * 2];  /* pre-sharpened rows */
  static int s_shrow_y[2] = {-2, -2};
  uint8_t *fbuf;
  int y;

  if (s_lcdfd < 0)
    {
      s_lcdfd = open("/dev/lcd0", O_WRONLY);
      if (s_lcdfd < 0)
        {
          printf("[LCD] open /dev/lcd0 failed: %d\n", s_lcdfd);
          return -1;
        }
    }

  fd = s_lcdfd;

  if (dw > 480)
    {
      dw = 480;
    }

  if (dw <= 0 || dh <= 0)
    {
      return -1;
    }

  /* Optional cover fit: crop the source to the target aspect ratio and
   * center it, so 4:3 camera frames do not stretch into the wide viewport. */

  int cw = sw;
  int ch = sh;
  int cx = 0;
  int cy = 0;

  if (s_lcd_cover)
    {
      if ((long)sw * dh > (long)sh * dw)
        {
          cw = (sh * dw) / dh;
          if (cw < 1) cw = 1;
          cx = (sw - cw) / 2;
        }
      else
        {
          ch = (sw * dh) / dw;
          if (ch < 1) ch = 1;
          cy = (sh - ch) / 2;
        }
    }

  /* Eight rows bound persistent staging to 7680 bytes. A full-screen
   * allocation (307200 bytes) exceeds the measured free contiguous heap
   * after voice/UI initialization and silently loses photo stills.
   * Keep the same scaler/preswapped pixels; submit complete row bands. */

  if (s_fbuf == NULL)
    {
#ifdef CONFIG_ESP32S3_SPIRAM_COMMON_HEAP
      s_fbuf = esp32s3_psram_static_alloc(480ul * 8ul * 2ul);
#else
      s_fbuf = malloc(480ul * 8ul * 2ul);
#endif
      if (s_fbuf == NULL)
        {
          printf("[LCD] malloc staging failed\n");
          return -ENOMEM;
        }
    }

  fbuf = s_fbuf;

  memset(&area, 0, sizeof(area));
  area.stride = 0;

  /* Precompute x bilinear weights once per frame (dw + dh integer divs) */

  {
    int x;

    for (x = 0; x < dw; x++)
      {
        int pos = x * cw;
        int s0 = pos / dw;
        int f = pos % dw;

        s_sx0[x] = (int16_t)(s0 >= cw ? cw - 1 : s0);
        s_fx8[x] = (uint8_t)((f * 256) / dw);
      }
  }

  for (y = 0; y < dh; y++)
    {
      int vpos = y * ch;
      int sy0 = vpos / dh;
      int fy = vpos % dh;
      int sy1;
      int fy8;
      int x0;
      uint8_t *dst = fbuf + (size_t)(y % 8) * (size_t)dw * 2;

      if (sy0 >= ch)
        {
          sy0 = ch - 1;
        }

      sy1 = (sy0 + 1 >= ch) ? sy0 : sy0 + 1;
      fy8 = (fy * 256) / dh;

      /* Pre-sharpen the two source rows once per band, then
       * bilinear-sample them (keeps the 2026-09-01 quality upgrade at a
       * fraction of the CPU cost). */

      if (s_lcd_sharpen && !s_lcd_cover)
        {
      for (x0 = 0; x0 < 2; x0++)
        {
          int sy = x0 == 0 ? sy0 : sy1;

          if (sy != s_shrow_y[x0])
            {
              int sx;

              for (sx = 0; sx < sw; sx++)
                {
                  uint16_t sp = cam_sharp_px(fb, sw, sh, sx, sy);
                  s_shrow[x0][sx * 2]     = (uint8_t)(sp & 0xff);
                  s_shrow[x0][sx * 2 + 1] = (uint8_t)(sp >> 8);
                }

              s_shrow_y[x0] = sy;
            }
        }
        }

      {
        const uint8_t *row0 = fb + ((size_t)(cy + sy0) * sw + cx) * 2;
        const uint8_t *row1 = fb + ((size_t)(cy + sy1) * sw + cx) * 2;
        int wy0 = 256 - fy8;

      for (x0 = 0; x0 < dw; x0++)
        {
          int sx0 = s_sx0[x0];
          int fx = s_fx8[x0];
          int sx1 = (sx0 + 1 >= cw) ? sx0 : sx0 + 1;
          uint16_t p00;
          uint16_t p01;
          uint16_t p10;
          uint16_t p11;
          int r;
          int g;
          int b;
          uint16_t px;

          if (s_lcd_sharpen && !s_lcd_cover)
            {
              p00 = s_shrow[0][sx0 * 2] | (s_shrow[0][sx0 * 2 + 1] << 8);
              p01 = s_shrow[0][sx1 * 2] | (s_shrow[0][sx1 * 2 + 1] << 8);
              p10 = s_shrow[1][sx0 * 2] | (s_shrow[1][sx0 * 2 + 1] << 8);
              p11 = s_shrow[1][sx1 * 2] | (s_shrow[1][sx1 * 2 + 1] << 8);
            }
          else
            {
              const uint8_t *q0 = row0 + (size_t)sx0 * 2;
              const uint8_t *q1 = row1 + (size_t)sx0 * 2;
              int o1 = (sx0 + 1 >= cw) ? 0 : 2;

              if (s_lcd_src_be)
                {
                  p00 = (uint16_t)(q0[0] << 8) | q0[1];
                  p01 = (uint16_t)(q0[o1] << 8) | q0[o1 + 1];
                  p10 = (uint16_t)(q1[0] << 8) | q1[1];
                  p11 = (uint16_t)(q1[o1] << 8) | q1[o1 + 1];
                }
              else
                {
                  p00 = q0[0] | (uint16_t)(q0[1] << 8);
                  p01 = q0[o1] | (uint16_t)(q0[o1 + 1] << 8);
                  p10 = q1[0] | (uint16_t)(q1[1] << 8);
                  p11 = q1[o1] | (uint16_t)(q1[o1 + 1] << 8);
                }
            }

          /* 先列向混合、再行间混合：每通道 6 次乘法（原 12 次）。
           * 权重和恒为 256*256，与原 4-tap 双线性严格等价。 */

          {
            int wx0 = 256 - fx;
            int t0;
            int t1;

            t0 = ((p00 >> 11) & 31) * wx0 + ((p01 >> 11) & 31) * fx;
            t1 = ((p10 >> 11) & 31) * wx0 + ((p11 >> 11) & 31) * fx;
            r = (t0 * wy0 + t1 * fy8) >> 16;

            t0 = ((p00 >> 5) & 63) * wx0 + ((p01 >> 5) & 63) * fx;
            t1 = ((p10 >> 5) & 63) * wx0 + ((p11 >> 5) & 63) * fx;
            g = (t0 * wy0 + t1 * fy8) >> 16;

            t0 = (p00 & 31) * wx0 + (p01 & 31) * fx;
            t1 = (p10 & 31) * wx0 + (p11 & 31) * fx;
            b = (t0 * wy0 + t1 * fy8) >> 16;
          }

          px = (uint16_t)((r << 11) | (g << 5) | b);
          /* Panel byte order is high byte first; swap while the pixel is
           * hot in cache so st7789 can send the staging buffer directly. */

          dst[x0 * 2]     = (uint8_t)(px >> 8);
          dst[x0 * 2 + 1] = (uint8_t)(px & 0xff);
        }
      }
      unsigned rows=(unsigned)(y % 8)+1;
      if(rows==8||y==dh-1){
        area.col_start=dx;area.col_end=dx+dw-1;
        area.row_start=dy+y+1-(int)rows;area.row_end=dy+y;
        area.data=fbuf;
        if(ioctl(fd,LCDDEVIO_PUTAREA_PRESWAPPED,(unsigned long)&area)<0)
          return errno?-errno:-EIO;
      }
    }
  printf("[LCD] bounded blit %dx%d bands=%d staging=7680 result=0\n",dw,dh,(dh+7)/8);
  return 0;
}

/* 取景器专用快路径：cover 裁剪 + 最近邻查表采样 + 字节交换，单次 PUTAREA。
 * 相对 lcd_put_rgb565（双线性 4-tap + 每帧 open/malloc/close）：
 * 几何不变时列/行映射与 staging 缓冲静态缓存，LCD fd 常驻打开；
 * 76k 目标像素从 ~12 次乘加/px 降到 1 次查表 + 1 次交换/px。 */

int lcd_put_rgb565_cover_fast(const uint8_t *fb, int sw, int sh,
                              int dx, int dy, int dw, int dh)
{
  static int s_fast_fd = -1;
  static uint8_t *s_fast_buf;
  static int s_fast_geo[6] = { -1, -1, -1, -1, -1, -1 };
  static int32_t s_fast_mapx[480];
  static int32_t s_fast_mapy[320];
  struct lcddev_area_s area;
  int cw;
  int ch;
  int cx;
  int cy;
  int x;
  int y;

  if (fb == NULL || dw <= 0 || dh <= 0 || dw > 480 || dh > 320)
    {
      return -EINVAL;
    }

  if (s_fast_fd < 0)
    {
      s_fast_fd = open("/dev/lcd0", O_WRONLY);
      if (s_fast_fd < 0)
        {
          printf("[LCD] fast: open /dev/lcd0 failed: %d\n", s_fast_fd);
          return -1;
        }
    }

  if (s_fast_buf == NULL)
    {
      s_fast_buf = malloc(480ul * 320ul * 2ul);
      if (s_fast_buf == NULL)
        {
          printf("[LCD] fast: staging malloc failed\n");
          return -ENOMEM;
        }
    }

  /* cover 裁剪（与 lcd_put_rgb565 同规则） */

  cw = sw;
  ch = sh;
  cx = 0;
  cy = 0;
  if ((long)sw * dh > (long)sh * dw)
    {
      cw = (sh * dw) / dh;
      if (cw < 1) cw = 1;
      cx = (sw - cw) / 2;
    }
  else
    {
      ch = (sw * dh) / dw;
      if (ch < 1) ch = 1;
      cy = (sh - ch) / 2;
    }

  if (s_fast_geo[0] != sw || s_fast_geo[1] != sh ||
      s_fast_geo[2] != dx || s_fast_geo[3] != dy ||
      s_fast_geo[4] != dw || s_fast_geo[5] != dh)
    {
      for (x = 0; x < dw; x++)
        {
          s_fast_mapx[x] = cx + (int)(((long)x * cw) / dw);
        }

      for (y = 0; y < dh; y++)
        {
          s_fast_mapy[y] = cy + (int)(((long)y * ch) / dh);
        }

      s_fast_geo[0] = sw;
      s_fast_geo[1] = sh;
      s_fast_geo[2] = dx;
      s_fast_geo[3] = dy;
      s_fast_geo[4] = dw;
      s_fast_geo[5] = dh;
    }

  for (y = 0; y < dh; y++)
    {
      const uint8_t *srow = fb + (size_t)s_fast_mapy[y] * (size_t)sw * 2;
      uint8_t *drow = s_fast_buf + (size_t)y * (size_t)dw * 2;

      for (x = 0; x < dw; x++)
        {
          const uint8_t *px = srow + (size_t)s_fast_mapx[x] * 2;

          drow[x * 2]     = px[1];   /* 面板序：高字节在前 */
          drow[x * 2 + 1] = px[0];
        }
    }

  memset(&area, 0, sizeof(area));
  area.col_start = dx;
  area.col_end = dx + dw - 1;
  area.row_start = dy;
  area.row_end = dy + dh - 1;
  area.data = s_fast_buf;
  ioctl(s_fast_fd, LCDDEVIO_PUTAREA_PRESWAPPED, (unsigned long)&area);
  return 0;
}

/****************************************************************************
 * lcd_put_rgb565_snapshot —— B 模式取景快照写屏（1:1 源→取景框）
 *
 * 与 lcd_put_rgb565_cover 的区别：
 *  - 源尺寸与显示尺寸相同（320x240 → 320x240），直接 1:1 拷贝 + 字节交换，
 *    不再走双线性缩放（快、且不会因缩放引入混叠）；
 *  - 与上一帧比较，**只写发生变化的行**（组连续行段提交），并只在写屏
 *    期间 DISPOFF（写完整帧时才有明显闪断，写几行时几乎不可见）；
 *  - 完全没变化时一次都不写 → 静止画面零闪断、零撕裂。
 ****************************************************************************/

static uint32_t s_snap_calls;
static uint32_t s_snap_bytes;

/* v170：用"每行校验和"代替 s_prev 整帧 memcmp。
 * 原做法每帧要读 2 × 307KB（PSRAM）=39ms，还要再 memcpy 307KB（再 19ms）；
 * 而校验和在缩放循环里顺手就算了（每像素 +1 add +1 xor），两边都省。
 * "相似就跳写"：碰撞概率极低，即使撞了也只是该行晚一帧刷新。 */

#define SNAP_ROWS_MAX  512

/* v179：这四张表原来是静态 .bss（4x512x4 = 8KB），而 .bss 占的是
 * **内部 SRAM** —— 正是 WiFi 驱动要用的那块。改成堆上分配
 * （PSRAM），把内部 SRAM 还回去。分配失败就退化为“每帧全写”，
 * 不会出现少写行的错误行为。 */

static uint32_t *s_rowsum_cur;
static uint32_t *s_rowxor_cur;
static uint32_t *s_rowsum_pv;
static uint32_t *s_rowxor_pv;
static int s_rowsum_valid;

static int snap_rowsum_alloc(void)
{
  size_t bytes = sizeof(uint32_t) * (size_t)SNAP_ROWS_MAX;

  if (s_rowsum_cur != NULL)
    {
      return 0;
    }

  s_rowsum_cur = (uint32_t *)malloc(bytes);
  s_rowxor_cur = (uint32_t *)malloc(bytes);
  s_rowsum_pv = (uint32_t *)malloc(bytes);
  s_rowxor_pv = (uint32_t *)malloc(bytes);

  if (s_rowsum_cur == NULL || s_rowxor_cur == NULL ||
      s_rowsum_pv == NULL || s_rowxor_pv == NULL)
    {
      free(s_rowsum_cur);free(s_rowxor_cur);free(s_rowsum_pv);free(s_rowxor_pv);
      s_rowsum_cur=s_rowxor_cur=s_rowsum_pv=s_rowxor_pv=NULL;
      printf("[Snap] rowsum alloc failed, fall back to full write");
      printf("\n");
      return -ENOMEM;
    }

  return 0;
}

/* v182：让位区用【包围盒】x 区间（整段常量），不再按圆角逐行求交。
 * 圆角让每行 blocked[] 都不同 → 无法合并 → 每帧上百次碎 PUTAREA，
 * 底栏观感就是拼接块/油膜条。包围盒略多让几个像素，但整段可 2~6 次写完。 */

static int cam_mask_blocked_bbox(int sy0, int sy1, int dx, int dw, int *blocked)
{
  int raw[2 * CAM_MASK_MAX];
  int n = 0;
  int i;
  int j;

  for (i = 0; i < s_pv_mask_num; i++)
    {
      const struct cam_mask_s *m = &s_pv_mask[i];
      int x0;
      int x1;

      if (m->y >= sy1 || m->y + m->h <= sy0)
        {
          continue;
        }

      x0 = m->x - dx;
      x1 = m->x + m->w - dx;
      if (x0 < 0)
        {
          x0 = 0;
        }

      if (x1 > dw)
        {
          x1 = dw;
        }

      if (x1 <= x0)
        {
          continue;
        }

      raw[2 * n] = x0;
      raw[2 * n + 1] = x1;
      n++;
    }

  /* 按 x0 排序后合并重叠/相邻 */

  for (i = 0; i < n; i++)
    {
      for (j = i + 1; j < n; j++)
        {
          if (raw[2 * j] < raw[2 * i])
            {
              int a0 = raw[2 * i];
              int a1 = raw[2 * i + 1];
              raw[2 * i] = raw[2 * j];
              raw[2 * i + 1] = raw[2 * j + 1];
              raw[2 * j] = a0;
              raw[2 * j + 1] = a1;
            }
        }
    }

  {
    int out = 0;

    for (i = 0; i < n; i++)
      {
        if (out > 0 && raw[2 * i] <= blocked[2 * out - 1])
          {
            if (raw[2 * i + 1] > blocked[2 * out - 1])
              {
                blocked[2 * out - 1] = raw[2 * i + 1];
              }
          }
        else
          {
            blocked[2 * out] = raw[2 * i];
            blocked[2 * out + 1] = raw[2 * i + 1];
            out++;
          }
      }

    return out;
  }
}

/* v169：把一批"让位区间相同"的行用一次 PUTAREA 写掉（stride 告诉驱动
 * 行间跳过多少字节）。原来每行一次调用，实测每次调用固定开销
 * ~0.6ms，244 次/帧就是 150ms —— 写屏的真正瓶颈。 */

static int snap_write_intervals(int fd, const uint8_t *cur, int dw,
                                 int dx, int dy, int y0, int rows,
                                 const int *blocked, int nb)
{
  int curx = 0;
  int bi;

  for (bi = 0; bi <= nb; bi++)
    {
      int x1 = (bi < nb) ? blocked[2 * bi] : dw;

      if (x1 > curx)
        {
          struct lcddev_area_s ar;

          memset(&ar, 0, sizeof(ar));
          ar.col_start = (fb_coord_t)(dx + curx);
          ar.col_end = (fb_coord_t)(dx + x1 - 1);
          ar.row_start = (fb_coord_t)(dy + y0);
          ar.row_end = (fb_coord_t)(dy + y0 + rows - 1);
          ar.data = cur + (size_t)y0 * (size_t)dw * 2u + (size_t)curx * 2u;
          ar.stride = dw * 2;

          if(ioctl(fd,LCDDEVIO_PUTAREA_PRESWAPPED,(unsigned long)&ar)<0)return -errno;
          s_snap_calls++;
          s_snap_bytes += (size_t)(x1 - curx) * 2u * (size_t)rows;
        }

      if (bi < nb)
        {
          curx = blocked[2 * bi + 1];
        }
    }
  return 0;
}

int lcd_put_rgb565_snapshot(const uint8_t *fb, int sw, int sh,
                            int dx, int dy, int dw, int dh)
{
  /* A complete480x320 staging frame required307200 contiguous heap bytes.
   * Render eight rows at a time instead, keeping pixel math and masks. */
  enum { BAND_ROWS=8, MAX_WIDTH=480 };
  static uint8_t *s_cur;
  static int s_fd=-1;
  static uint32_t seen_epoch;
  int y,ret=0,wrote_any=0;
  uint32_t epoch=atomic_load(&s_display_epoch);
  if(!fb||sw<=0||sh<=0||dw<=0||dh<=0||dw>MAX_WIDTH||dh>320||
     dx<0||dy<0||dx+dw>480||dy+dh>320)return -EINVAL;
  if(!s_cur){
#ifdef CONFIG_ESP32S3_SPIRAM_COMMON_HEAP
    s_cur=esp32s3_psram_static_alloc(MAX_WIDTH*BAND_ROWS*2u);
#else
    s_cur=malloc(MAX_WIDTH*BAND_ROWS*2u);
#endif
    if(!s_cur)return -ENOMEM;
    s_rowsum_valid=0;
    snap_rowsum_alloc();
  }
  if(seen_epoch!=epoch)s_rowsum_valid=0;
  if(s_fd<0){s_fd=open("/dev/lcd0",O_WRONLY);if(s_fd<0)return -errno;}
  for(int base=0;base<dh;base+=BAND_ROWS){
    int end=base+BAND_ROWS<dh?base+BAND_ROWS:dh;
  {
    int cw = sw;
    int ch = sh;
    int cx = 0;
    int cy = 0;
    uint32_t hstep;

    if ((long)sw * dh > (long)sh * dw)
      {
        cw = (sh * dw) / dh;
        if (cw < 1) cw = 1;
        cx = (sw - cw) / 2;
      }
    else
      {
        ch = (sw * dh) / dw;
        if (ch < 1) ch = 1;
        cy = (sh - ch) / 2;
      }

    /* v167：水平采样点改用 16.16 定点累加。Xtensa 没有硬件除法，
     * 原实现每个像素要做 2 次整数除法（共 ~30 万次/帧）。采样点与
     * 权重与原实现等价（累积截断误差 < 1/65536 个步进）。 */

    hstep = ((uint32_t)cw << 16) / (uint32_t)dw;

    for (y = base; y < end; y++)
      {
        int vpos = y * ch;
        int sy0 = vpos / dh;
        int fy8;
        int sy1;
        uint8_t *dst = s_cur + (size_t)(y-base) * (size_t)dw * 2u;
        const uint8_t *row0;
        const uint8_t *row1;
        uint32_t hacc = 0;
        uint32_t rsum = 0;
        uint32_t rxor = 0;
        int x;

        if (sy0 >= ch) sy0 = ch - 1;
        sy1 = (sy0 + 1 >= ch) ? sy0 : sy0 + 1;
        fy8 = ((vpos % dh) * 256) / dh;

        /* 源行基址提出内循环：原实现每像素还要做 4 次行乘加 */

        row0 = fb + (size_t)(cy + sy0) * (size_t)sw * 2u;
        row1 = fb + (size_t)(cy + sy1) * (size_t)sw * 2u;

        for (x = 0; x < dw; x++)
          {
            int sx0 = (int)(hacc >> 16);
            int fx8 = (int)(((hacc & 0xffffu) * 256u) >> 16);
            int sx1;
            const uint8_t *q00;
            const uint8_t *q01;
            const uint8_t *q10;
            const uint8_t *q11;
            uint16_t p00;
            uint16_t p01;
            uint16_t p10;
            uint16_t p11;
            int wx0;
            int t0;
            int t1;
            int r;
            int g;
            int bl;
            uint16_t px;

            hacc += hstep;

            if (sx0 >= cw) sx0 = cw - 1;
            sx1 = (sx0 + 1 >= cw) ? sx0 : sx0 + 1;

            q00 = row0 + (size_t)(cx + sx0) * 2u;
            q01 = row0 + (size_t)(cx + sx1) * 2u;
            q10 = row1 + (size_t)(cx + sx0) * 2u;
            q11 = row1 + (size_t)(cx + sx1) * 2u;

            p00 = (uint16_t)(q00[0] | (q00[1] << 8));
            p01 = (uint16_t)(q01[0] | (q01[1] << 8));
            p10 = (uint16_t)(q10[0] | (q10[1] << 8));
            p11 = (uint16_t)(q11[0] | (q11[1] << 8));

            wx0 = 256 - fx8;
            t0 = ((p00 >> 11) & 31) * wx0 + ((p01 >> 11) & 31) * fx8;
            t1 = ((p10 >> 11) & 31) * wx0 + ((p11 >> 11) & 31) * fx8;
            r = (t0 * (256 - fy8) + t1 * fy8) >> 16;

            t0 = ((p00 >> 5) & 63) * wx0 + ((p01 >> 5) & 63) * fx8;
            t1 = ((p10 >> 5) & 63) * wx0 + ((p11 >> 5) & 63) * fx8;
            g = (t0 * (256 - fy8) + t1 * fy8) >> 16;

            t0 = (p00 & 31) * wx0 + (p01 & 31) * fx8;
            t1 = (p10 & 31) * wx0 + (p11 & 31) * fx8;
            bl = (t0 * (256 - fy8) + t1 * fy8) >> 16;

            px = (uint16_t)((r << 11) | (g << 5) | bl);
            dst[x * 2]     = (uint8_t)(px >> 8);
            dst[x * 2 + 1] = (uint8_t)(px & 0xff);

            rsum += (uint32_t)px;
            rxor ^= (uint32_t)px;
          }

        if (s_rowsum_cur != NULL && y < SNAP_ROWS_MAX)
          {
            s_rowsum_cur[y] = rsum;
            s_rowxor_cur[y] = rxor;
          }
      }
  }


    /* Commit changed runs while their eight-row staging data are alive. */
    for(int local=base;local<end;){
      bool dirty=!s_rowsum_cur||!s_rowsum_valid||
        s_rowsum_cur[local]!=s_rowsum_pv[local]||s_rowxor_cur[local]!=s_rowxor_pv[local];
      if(!dirty){local++;continue;}
      int first=local;bool hit=cam_mask_row_hit(dy+local);local++;
      while(local<end&&cam_mask_row_hit(dy+local)==hit&&
        (!s_rowsum_cur||!s_rowsum_valid||s_rowsum_cur[local]!=s_rowsum_pv[local]||s_rowxor_cur[local]!=s_rowxor_pv[local]))local++;
      if(hit){
        int blocked[2*CAM_MASK_MAX];int nb=cam_mask_blocked_bbox(dy+first,dy+local,dx,dw,blocked);
        ret=snap_write_intervals(s_fd,s_cur,dw,dx,dy+base,first-base,local-first,blocked,nb);
      }else{
        struct lcddev_area_s ar;memset(&ar,0,sizeof(ar));
        ar.col_start=dx;ar.col_end=dx+dw-1;ar.row_start=dy+first;ar.row_end=dy+local-1;
        ar.data=s_cur+(size_t)(first-base)*dw*2u;
        ret=ioctl(s_fd,LCDDEVIO_PUTAREA_PRESWAPPED,(unsigned long)&ar);
        if(ret<0)ret=-errno;
        else{s_snap_calls++;s_snap_bytes+=(size_t)dw*2u*(local-first);}
      }
      if(ret<0){s_rowsum_valid=0;return ret;}
      wrote_any=1;
    }
  }
  if(s_rowsum_cur)for(y=0;y<dh;y++){
    s_rowsum_pv[y]=s_rowsum_cur[y];s_rowxor_pv[y]=s_rowxor_cur[y];
  }
  s_rowsum_valid=s_rowsum_cur!=NULL;seen_epoch=epoch;
  atomic_store(&s_display_done,epoch);
  if(wrote_any)atomic_fetch_add(&s_display_count,1);
  return 0;
}

int lcd_put_rgb565_cover(const uint8_t *fb, int sw, int sh,
                         int dx, int dy, int dw, int dh)
{
  bool old = s_lcd_cover;
  int ret;

  /* v189：必须整幅一次写。
   * v148 曾拆成 4 条横带、每条都调 lcd_put_rgb565(整幅源, 1/4 高)，
   * 等于把同一张图压扁后叠 4 层 —— 快门定格时用户看到"同一画面压缩
   * 显示 4 次"。分带若要做必须按源图切片裁剪，这里直接单次写完。 */

  s_lcd_cover = true;
  ret = lcd_put_rgb565(fb, sw, sh, dx, dy, dw, dh);
  s_lcd_cover = old;
  return ret;
}

int lcd_put_rgb565_cover_be(const uint8_t *fb, int sw, int sh,
                            int dx, int dy, int dw, int dh)
{
  bool oldc = s_lcd_cover;
  bool oldb = s_lcd_src_be;
  int ret;

  s_lcd_cover = true;
  s_lcd_src_be = true;
  ret = lcd_put_rgb565(fb, sw, sh, dx, dy, dw, dh);
  s_lcd_cover = oldc;
  s_lcd_src_be = oldb;
  return ret;
}


/****************************************************************************
 * Name: camera_shutdown
 *
 * Description:
 *   2026-09-09 M2（土壤↔拍照互斥）：完整关闭摄像头硬件，放行 IO42/40。
 *   1) esp32s3_cam_dvp_stop()：停 CAM/DMA（若预览线程已停则幂等无操作）；
 *   2) SCCB 软复位 OV3660（0x3008=0x82，驱动已验证的复位序列起点）→ 传感器
 *      回到冷启动默认态（未配置/未流出的传感器不驱动 DVP 输出）→
 *      IO40(Y9)/IO42(VSYNC) 不再被外部芯片推挽占用，可安全切回 UART0。
 *   调用方（screen_camera 离页）随后需 esp32s3_uart0_reclaim_pins() 把引脚
 *   路由回 UART0，并恢复 sensor_service 轮询。
 *
 *   Returned Value: 0；SCCB 复位失败时返回负 errno（不阻塞，引脚仍可切回）
 ****************************************************************************/

int camera_shutdown(void)
{
#ifdef CONFIG_ESP32S3_CAM_DVP
  int ret;

  esp32s3_cam_dvp_stop();

  /* 软复位传感器：0x3008=0x82（与 init 序列开头一致，已验证可复现） */

  ret = board_camera_sccb_write(OV3660_SYSTEM_CTROL0, 0x82);
  if (ret < 0)
    {
      printf("[Cam] shutdown: sensor soft-reset failed %d\n", ret);
      return ret;
    }

  usleep(20 * 1000);   /* 让复位生效（传感器停止驱动 DVP 输出） */
#endif

  /* ⚠️ 软复位把 OV3660 打回冷启动默认态：驱动侧"已配置/当前几何"的缓存
   * 必须一起作废。否则下次进拍照页 camera_mode_preview() 会以为"已在该
   * 模式"直接返回 0 → 跳过完整配置 → 第二次进页面没画面。 */

  s_rgb565_ready = false;
  s_rgb_hd_ready = false;
  s_rgb_qvga_ready = false;
  s_sensor_mode = CAM_MODE_NONE;
  return 0;
}
