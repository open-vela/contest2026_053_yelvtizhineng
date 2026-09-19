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
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>   /* malloc/free */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>   /* usleep */
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <nuttx/lcd/lcd_dev.h>

#include "camera_capture.h"
#include "../../services/sd_write.h"

/* board 层导出：SCCB 读写 + XMCLK + 探测 */

int board_camera_sccb_probe(uint16_t *pid);
int board_camera_sccb_write(uint16_t reg, uint8_t val);
int board_camera_sccb_read(uint16_t reg, uint8_t *val);

/* 驱动层导出：帧几何运行时切换（预览 160×120 / 拍照 320×240） */

#ifdef CONFIG_ESP32S3_CAM_DVP
int esp32s3_cam_dvp_set_framesize(uint32_t w, uint32_t h);
void esp32s3_cam_dvp_get_framesize(uint32_t *w, uint32_t *h);
#endif

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
  {0x3004, 0xef},
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
  {0x5481, 0x05},
  {0x5482, 0x09},
  {0x5483, 0x10},
  {0x5484, 0x3a},
  {0x5485, 0x4c},
  {0x5486, 0x5a},
  {0x5487, 0x68},
  {0x5488, 0x74},
  {0x5489, 0x80},
  {0x548a, 0x8e},
  {0x548b, 0xa4},
  {0x548c, 0xb4},
  {0x548d, 0xc8},
  {0x548e, 0xde},
  {0x548f, 0xf0},
  {0x5490, 0x15},
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

/* set_image_options（官方第 242-303 行）：RGB565 固定调用。
 * pixformat 非 JPEG → reg21 不带 0x20。
 * binning 由调用者传入（set_framesize 计算）。 */

static int ov3660_set_image_options(bool binning)
{
  uint8_t reg20 = 0;
  uint8_t reg21 = 0;
  uint8_t reg4514 = 0;
  uint8_t reg4514_test = 0;
  int ret;

  /* 非 JPEG：不加 0x20 到 reg21 */

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

  /* 无 vflip / 无 hmirror */

  switch (reg4514_test)
    {
      case 0: reg4514 = 0x88; break;   /* no binning, normal */
      case 4: reg4514 = 0xaa; break;   /* binning, normal */
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

static int ov3660_set_framesize(uint16_t w, uint16_t h)
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
      ret = ov3660_write_addr_reg(OV3660_X_TOTAL_SIZE_H, s->total_x,
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

  ret = ov3660_set_image_options(binning);
  if (ret < 0) return ret;

  /* 记下本次选的参数（供写后读回校验） */

  g_pll_binning = binning;

  /* PLL（官方第 360-378 行）：非 JPEG 分支，官方值 tuned for 16MHz XCLK。
   * 本板 XMCLK = 20MHz（board 层 LEDC），PCLK 会按 1.25× 上偏：
   *   calc_sysclk(20M, M=8, sys=1, pre=0, root2x=0, seld5=0, pclk=8)
   *     → VCO=160M PLLCLK=160M PCLK=10M（应为 8M）→ 帧率偏快 1.25× →
   *     AEC 曝光积分时序错位 → 曝光/白平衡反复振荡（实测帧值每拍不同）
   * 修正：pclk_div 8→10 → PCLK = 160M/2/10 = 8MHz（与官方调优一致）*/

  if (w * h <= 160 * 120)
    {
      /* framesize < QVGA：40M SYSCLK / 8MHz PCLK（20MHz XCLK 下用 pclk=10） */

      ret = ov3660_set_pll(false, 8, 1, 0, false, 0, true, 10);
      g_pll_mul = 8;
      g_pll_div = 10;
    }
  else if (w * h <= 320 * 240)
    {
      /* framesize == QVGA：20M SYSCLK / 8MHz PCLK（20MHz XCLK 下 pclk=5） */

      ret = ov3660_set_pll(false, 8, 1, 0, false, 2, true, 5);
      g_pll_mul = 8;
      g_pll_div = 5;
    }
  else
    {
      /* framesize >= HVGA：10M SYSCLK / 8MHz PCLK（20MHz XCLK 下 pclk≈2.5，
       * 取 2 → 10MHz，或 3 → 6.7MHz；未用分支暂保留官方值） */

      ret = ov3660_set_pll(false, 4, 1, 0, false, 2, true, 2);
      g_pll_mul = 4;
      g_pll_div = 2;
    }

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

  s_probed = true;
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

  ret = ov3660_set_framesize(320, 240);
  if (ret < 0)
    {
      printf("[Cam] framesize failed: %d\n", ret);
      return ret;
    }

  /* 启动输出 */

  ret = board_camera_sccb_write(OV3660_SYSTEM_CTROL0, 0x02);
  if (ret < 0) return ret;

  printf("[Cam] OV3660 configured: JPEG 320x240\n");
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

  ret = ov3660_set_framesize(w, h);
  if (ret < 0)
    {
      printf("[Cam] framesize failed: %d\n", ret);
      return ret;
    }

  /* ⚠️ 传感器改了输出尺寸，驱动层回读长度必须同步改 —— 否则会按 160×120
   * 去读 640 宽的帧：既拿不到完整画面（只有 38400B），又是错行垃圾。
   * 2026-09-10 实测症状："[Cam] photo captured: 38400 bytes (640x480)"。 */

#ifdef CONFIG_ESP32S3_CAM_DVP
  esp32s3_cam_dvp_set_framesize(w, h);
#endif

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
      {0x3820, (uint16_t)(g_pll_binning ? 0x01 : 0x40)},   /* binning/无 */
      {0x3821, (uint16_t)(g_pll_binning ? 0x01 : 0x00)},   /* binning no-JPEG */
      {0x303b, g_pll_mul},   /* PLL multiplier（当前模式） */
      {0x3824, g_pll_div},   /* PCLK_RATIO（当前模式） */
      {0x5001, 0xa3},   /* AWB+色彩矩阵+SDE(0x83) | scale(0x20) */
      {0x5800, 0x0c},   /* gamma 表点（此前缺失 → 默认 0x00 压暗） */
      {0x583d, 0xcf},   /* gamma 表点 */
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

  usleep(1000 * 1000);

  printf("[Cam] OV3660 configured: RGB565 %ux%u\n", (unsigned)w, (unsigned)h);

  s_rgb565_ready = true;
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

  ret = ov3660_set_framesize(CAM_PREVIEW_W, CAM_PREVIEW_H);
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

  ret = ov3660_set_framesize(CAM_PHOTO_W, CAM_PHOTO_H);
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
  unsigned excess;
  unsigned desat_done;
  int ret;
#ifdef CONFIG_PLANT_CAM_DIAG
  unsigned clip;
  unsigned pink;
  unsigned pink_after;
  unsigned luma;
#endif

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

  /* 高光去饱和：粉色块 → 中性白。算法内部顺手给出 excess（本来有多少红偏）
   * 与返回值（真正消掉多少），两者同源，excess >= 返回值 恒成立。 */

  desat_done = cam_highlight_desat(fb, (unsigned)(total / 2), &excess);

#ifdef CONFIG_PLANT_CAM_DIAG
  printf("[Cam] photo: 粉块嫌疑 %.2f%%，亮部红偏强度 %u，红通道顶格 %.1f%%"
         "，亮度 %u/255 -> 高光去饱和\n",
         (double)pink / 10.0, excess, (double)clip / 10.0, luma);

  /* 修完立刻在【同一块缓冲】上复测：这一行就是"去饱和有没有进上传帧"的
   * 铁证。2026-09-10 用户报"时有时无"，只有逐帧量化才说得清 —— 靠肉眼
   * 看屏幕和看上传图会互相打脸。 */

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

#else
  /* 正式版：一行说清这一帧做了什么（诊断细节见 CONFIG_PLANT_CAM_DIAG）。 */

  printf("[Cam] photo: %ux%u %u 字节，高光去饱和 红偏 %u -> %u（消掉 %u%%），"
         "亮度 %u/255\n",
         CAM_PHOTO_W, CAM_PHOTO_H, (unsigned)total, excess, excess - desat_done,
         excess ? (desat_done * 100U) / excess : 0U,
         cam_frame_mean_luma(fb, total));
#endif

  *frame = fb;
  *len_out = total;

  printf("[Cam] photo captured: %u bytes (%dx%d)\n",
         (unsigned)total, CAM_PHOTO_W, CAM_PHOTO_H);
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

static void *camera_preview_loop(void *arg)
{
  volatile bool eoi = false;
  int ret;
  int fails = 0;

  (void)arg;

  /* OV3660 RGB565 大端输出（2.49 彩条实锤）：强制字节交换（防任何残留/误设） */

  esp32s3_cam_dvp_set_swap(true);
  esp32s3_cam_dvp_set_quiet(true);
  esp32s3_cam_dvp_set_stream(true);
  lcd_put_rgb565_set_sharpen(false);   /* 预览走快路径，单帧拍照仍锐化 */
  esp32s3_cam_dvp_init();
  esp32s3_cam_dvp_start();

  while (s_preview_run)
    {
      if (s_preview_freeze)
        {
          usleep(20000);
          continue;
        }

      ret = esp32s3_cam_dvp_capture(NULL, NULL, &eoi);
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

      /* 直写 LCD 取景框区域（与 plant cam preview 同链路：软件最近邻
       * 缩放 + LCDDEVIO_PUTAREA 逐行分块 ≤320px，不越界） */

      /* 去饱和后再上屏：否则取景框里那片亮窗/灯同样是粉色块 */

      cam_highlight_desat(esp32s3_cam_dvp_get_frame(),
                          (unsigned)(CAM_FRAME_W * CAM_FRAME_H), NULL);
      lcd_put_rgb565(esp32s3_cam_dvp_get_frame(), CAM_FRAME_W, CAM_FRAME_H,
                     s_preview_x, s_preview_y, s_preview_w, s_preview_h);

      /* 小睡一拍：给 UI 线程留呼吸；滚动采集下写屏是主要耗时 */

      usleep(8000);   /* 给 UI 线程留呼吸；滚动采集下写屏已是主要耗时 */
    }

  esp32s3_cam_dvp_stop();
  lcd_put_rgb565_set_sharpen(true);
  esp32s3_cam_dvp_set_stream(false);
  esp32s3_cam_dvp_set_quiet(false);
  return NULL;
}

int camera_preview_start(int x, int y, int w, int h)
{
  if (s_preview_run)
    {
      return 0;
    }

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

int camera_preview_take(void)
{
  /* 冻结：预览线程停取帧，s_preview_latest 保留最后一帧（画面定格） */

  s_preview_freeze = true;
  return 0;
}

int camera_preview_resume(void)
{
  s_preview_freeze = false;
  return 0;
}

int camera_preview_stop(void)
{
  if (!s_preview_run)
    {
      return 0;
    }

  s_preview_run = false;
  pthread_join(s_preview_tid, NULL);
  s_preview_tid = 0;
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
  int fd = open("/dev/lcd0", O_WRONLY);
  struct lcddev_area_s area;
  static int16_t s_sx0[480];   /* target x -> source integer x0 (bilinear) */
  static uint8_t s_fx8[480];   /* target x -> source fraction x256 */
  static uint8_t s_shrow[2][CAM_FRAME_MAX_W * 2];  /* pre-sharpened rows */
  static int s_shrow_y[2] = {-2, -2};
  uint8_t *fbuf;
  int y;

  if (fd < 0)
    {
      printf("[LCD] open /dev/lcd0 failed: %d\n", fd);
      return -1;
    }

  if (dw > 480)
    {
      dw = 480;
    }

  if (dw <= 0 || dh <= 0)
    {
      close(fd);
      return -1;
    }

  /* Whole-frame staging buffer: scale into one contiguous area, then one
   * LCDDEVIO_PUTAREA pushes the whole viewport in a single transaction.
   * st7789 wrram swaps/sends in rowbuff_be-sized chunks, so large areas
   * never overflow the driver working buffer. */

  fbuf = malloc((size_t)dw * (size_t)dh * 2);
  if (fbuf == NULL)
    {
      printf("[LCD] malloc staging %dx%d failed\n", dw, dh);
      close(fd);
      return -1;
    }

  memset(&area, 0, sizeof(area));
  area.stride = 0;

  /* Precompute x bilinear weights once per frame (dw + dh integer divs) */

  {
    int x;

    for (x = 0; x < dw; x++)
      {
        int pos = x * sw;
        int s0 = pos / dw;
        int f = pos % dw;

        s_sx0[x] = (int16_t)(s0 >= sw ? sw - 1 : s0);
        s_fx8[x] = (uint8_t)((f * 256) / dw);
      }
  }

  for (y = 0; y < dh; y++)
    {
      int vpos = y * sh;
      int sy0 = vpos / dh;
      int fy = vpos % dh;
      int sy1;
      int fy8;
      int x0;
      uint8_t *dst = fbuf + (size_t)y * (size_t)dw * 2;

      if (sy0 >= sh)
        {
          sy0 = sh - 1;
        }

      sy1 = (sy0 + 1 >= sh) ? sy0 : sy0 + 1;
      fy8 = (fy * 256) / dh;

      /* Pre-sharpen the two source rows once per band, then
       * bilinear-sample them (keeps the 2026-09-01 quality upgrade at a
       * fraction of the CPU cost). */

      if (s_lcd_sharpen)
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

      for (x0 = 0; x0 < dw; x0++)
        {
          int sx0 = s_sx0[x0];
          int fx = s_fx8[x0];
          int sx1 = (sx0 + 1 >= sw) ? sx0 : sx0 + 1;
          int w00;
          int w01;
          int w10;
          int w11;
          uint16_t p00;
          uint16_t p01;
          uint16_t p10;
          uint16_t p11;
          int r;
          int g;
          int b;
          uint16_t px;

          if (s_lcd_sharpen)
            {
              p00 = s_shrow[0][sx0 * 2] | (s_shrow[0][sx0 * 2 + 1] << 8);
              p01 = s_shrow[0][sx1 * 2] | (s_shrow[0][sx1 * 2 + 1] << 8);
              p10 = s_shrow[1][sx0 * 2] | (s_shrow[1][sx0 * 2 + 1] << 8);
              p11 = s_shrow[1][sx1 * 2] | (s_shrow[1][sx1 * 2 + 1] << 8);
            }
          else
            {
              const uint8_t *q00 = fb + ((size_t)sy0 * sw + sx0) * 2;
              const uint8_t *q01 = fb + ((size_t)sy0 * sw + sx1) * 2;
              const uint8_t *q10 = fb + ((size_t)sy1 * sw + sx0) * 2;
              const uint8_t *q11 = fb + ((size_t)sy1 * sw + sx1) * 2;

              p00 = q00[0] | (uint16_t)(q00[1] << 8);
              p01 = q01[0] | (uint16_t)(q01[1] << 8);
              p10 = q10[0] | (uint16_t)(q10[1] << 8);
              p11 = q11[0] | (uint16_t)(q11[1] << 8);
            }

          w00 = (256 - fx) * (256 - fy8);
          w01 = fx * (256 - fy8);
          w10 = (256 - fx) * fy8;
          w11 = fx * fy8;

          r = ((((p00 >> 11) & 31) * w00 + ((p01 >> 11) & 31) * w01 +
                ((p10 >> 11) & 31) * w10 + ((p11 >> 11) & 31) * w11) >> 16);
          g = ((((p00 >> 5) & 63) * w00 + ((p01 >> 5) & 63) * w01 +
                ((p10 >> 5) & 63) * w10 + ((p11 >> 5) & 63) * w11) >> 16);
          b = (((p00 & 31) * w00 + (p01 & 31) * w01 +
                (p10 & 31) * w10 + (p11 & 31) * w11) >> 16);

          px = (uint16_t)((r << 11) | (g << 5) | b);
          dst[x0 * 2]     = (uint8_t)(px & 0xff);
          dst[x0 * 2 + 1] = (uint8_t)(px >> 8);
        }
    }

  area.col_start = dx;
  area.col_end = dx + dw - 1;
  area.row_start = dy;
  area.row_end = dy + dh - 1;
  area.data = fbuf;
  ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)&area);

  free(fbuf);
  close(fd);
  return 0;
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
  s_sensor_mode = CAM_MODE_NONE;
  return 0;
}
