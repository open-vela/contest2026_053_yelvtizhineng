/****************************************************************************
 * GC0308 camera sensor helpers for M5Stack CoreS3.
 *
 * Register tables are the official Espressif esp_cam_sensor GC0308 RGB565
 * tables, checked in next to this file.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/signal.h>

#include "gc0308.h"
#include "gc0308_settings.h"

#define GC0308_REG_OUTPUT_EN 0x25
#define GC0308_REG_PAGE      0xfe

static const gc0308_reginfo_t g_gc0308_qvga[] =
{
  gc0308_select_page0_default,
  gc0308_settings_rgb565_be,
  init_reglist_DVP_8bit_320x240_20fps_subsample,
};

static const gc0308_reginfo_t g_gc0308_vga[] =
{
  gc0308_select_page0_default,
  gc0308_settings_rgb565_be,
  init_reglist_DVP_8bit_640x480_16fps,
};

static const gc0308_reginfo_t g_gc0308_yuv_qvga[] =
{
  gc0308_select_page0_default,
  gc0308_settings_yuv422_yuyv,
  init_reglist_DVP_8bit_320x240_20fps_subsample,
};

static const gc0308_reginfo_t g_gc0308_yuv_vga[] =
{
  gc0308_select_page0_default,
  gc0308_settings_yuv422_yuyv,
  init_reglist_DVP_8bit_640x480_16fps,
};

extern int board_camera_sccb_write(uint16_t reg, uint8_t val);

static const uint8_t g_gc0308_yuv_qvga_verify[][2] =
{
  {0x24, 0xa2},  /* YUV422 Y-Cb-Y-Cr */
  {0x25, 0x0f},  /* data output enabled */
  {0xfe, 0x00},  /* page 0 */
  {0x01, 0x32},
  {0x02, 0x0c},
  {0x10, 0x26},
  {0x11, 0x0d},
  {0x12, 0x2a},
  {0x50, 0x14},
};

static int gc0308_write_array(FAR const gc0308_reginfo_t *regs, size_t count)
{
  size_t i;
  int ret;

  for (i = 0; i < count; i++)
    {
      ret = board_camera_sccb_write(regs[i].reg, regs[i].val);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

int gc0308_init(void)
{
  int ret;

  ret = board_camera_sccb_write(GC0308_REG_PAGE, 0x80);
  nxsig_usleep(10 * 1000);
  ret |= board_camera_sccb_write(GC0308_REG_PAGE, 0x00);
  nxsig_usleep(20 * 1000);
  return ret;
}

int gc0308_config_rgb565(uint16_t w, uint16_t h)
{
  FAR const gc0308_reginfo_t *regs;
  size_t count;
  bool subsample_160x120 = false;
  int ret;

  if (w == 320 && h == 240)
    {
      regs = g_gc0308_qvga;
      count = sizeof(g_gc0308_qvga) / sizeof(g_gc0308_qvga[0]);
    }
  else if (w == 640 && h == 480)
    {
      regs = g_gc0308_vga;
      count = sizeof(g_gc0308_vga) / sizeof(g_gc0308_vga[0]);
    }
  else if (w == 160 && h == 120)
    {
      /* Linux gc0308_frame_sizes[]: 0x44 subsample / 160x120. */
      regs = g_gc0308_qvga;
      count = sizeof(g_gc0308_qvga) / sizeof(g_gc0308_qvga[0]);
      subsample_160x120 = true;
    }
  else
    {
      return -EINVAL;
    }

  ret = gc0308_write_array(regs, count);
  if (ret < 0)
    {
      return ret;
    }

  if (subsample_160x120)
    {
      static const gc0308_reginfo_t page1[] =
      {
        {GC0308_REG_PAGE, 0x01},
        {0x54, 0x44}, /* subsample 4:1 both axes */
        {0x55, 0x03},
        {0x56, 0x00},
        {0x57, 0x00},
        {0x58, 0x00},
        {0x59, 0x00},
      };

      static const gc0308_reginfo_t page0_crop[] =
      {
        {GC0308_REG_PAGE, 0x00},
        {0x46, 0x80}, /* crop window enable */
        {0x47, 0x00},
        {0x48, 0x00},
        {0x49, 0x00},
        {0x4a, 0x78}, /* height 120 */
        {0x4b, 0x00},
        {0x4c, 0xa0}, /* width 160 */
      };

      ret = gc0308_write_array(page1, sizeof(page1) / sizeof(page1[0]));
      if (ret < 0)
        {
          return ret;
        }

      ret = gc0308_write_array(page0_crop,
                               sizeof(page0_crop) /
                               sizeof(page0_crop[0]));
      if (ret < 0)
        {
          return ret;
        }
    }

  {
    uint8_t analog = 0;

    ret = board_camera_sccb_read(0x1a, &analog);
    if (ret >= 0)
      {
        analog &= (uint8_t)~0x01;
        ret = board_camera_sccb_write(0x1a, analog);
        if (ret < 0)
          {
            return ret;
          }
      }
  }

  ret = board_camera_sccb_write(GC0308_REG_OUTPUT_EN, 0x0f);
  if (ret < 0)
    {
      return ret;
    }

  /* Let AEC/AWB settle before the first displayed frame. */
  nxsig_usleep(300 * 1000);
  return OK;
}

int gc0308_config_yuv422(uint16_t w, uint16_t h)
{
  FAR const gc0308_reginfo_t *regs;
  size_t count;
  uint8_t analog;
  int ret;

  if (w == 320 && h == 240)
    {
      regs = g_gc0308_yuv_qvga;
      count = sizeof(g_gc0308_yuv_qvga) / sizeof(g_gc0308_yuv_qvga[0]);
    }
  else if (w == 640 && h == 480)
    {
      regs = g_gc0308_yuv_vga;
      count = sizeof(g_gc0308_yuv_vga) / sizeof(g_gc0308_yuv_vga[0]);
    }
  else
    {
      return -EINVAL;
    }

  ret = gc0308_write_array(regs, count);
  if (ret < 0)
    {
      return ret;
    }

  ret = board_camera_sccb_read(0x1a, &analog);
  if (ret >= 0)
    {
      analog &= (uint8_t)~0x01;
      ret = board_camera_sccb_write(0x1a, analog);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = board_camera_sccb_write(GC0308_REG_OUTPUT_EN, 0x0f);
  if (ret < 0)
    {
      return ret;
    }

  /* Let the first streamed frames start before SCCB readback.  Reading the
   * sensor in the exact transition cycle can transiently fail on this board;
   * the official driver likewise lets the sensor settle before the first
   * application frame is consumed.
   */

  nxsig_usleep(300 * 1000);

  {
    unsigned int i;
    int mismatches = 0;

    for (i = 0;
         i < sizeof(g_gc0308_yuv_qvga_verify) /
             sizeof(g_gc0308_yuv_qvga_verify[0]);
         i++)
      {
        uint8_t value = 0;
        uint8_t expected = g_gc0308_yuv_qvga_verify[i][1];
        int rr = board_camera_sccb_read(g_gc0308_yuv_qvga_verify[i][0],
                                        &value);

        if (rr < 0)
          {
            printf("[Cam] GC0308 verify 0x%02x read failed: %d\n",
                   g_gc0308_yuv_qvga_verify[i][0], rr);
            mismatches++;
          }
        else if (value != expected)
          {
            printf("[Cam] GC0308 verify 0x%02x=%02x expected %02x\n",
                   g_gc0308_yuv_qvga_verify[i][0], value, expected);
            mismatches++;
          }
      }

    printf("[Cam] GC0308 official QVGA register verify: %s (%u regs)\n",
           mismatches == 0 ? "OK" : "MISMATCH",
           (unsigned int)(sizeof(g_gc0308_yuv_qvga_verify) /
                          sizeof(g_gc0308_yuv_qvga_verify[0])));
  }
  return OK;
}

int gc0308_resume_yuv_qvga(void)
{
  uint8_t analog=0;
  int ret=board_camera_sccb_write(GC0308_REG_PAGE,0x00);
  if(ret<0)return ret;
  ret=board_camera_sccb_read(0x1a,&analog);
  if(ret<0)return ret;
  ret=board_camera_sccb_write(0x1a,analog & (uint8_t)~0x01);
  if(ret<0)return ret;
  ret=board_camera_sccb_write(GC0308_REG_OUTPUT_EN,0x0f);
  if(ret<0)return ret;
  nxsig_usleep(300 * 1000); /* Retain the existing fresh-light settling window. */
  for(unsigned i=0;i<sizeof(g_gc0308_yuv_qvga_verify)/sizeof(g_gc0308_yuv_qvga_verify[0]);i++)
    {
      uint8_t value=0;
      ret=board_camera_sccb_read(g_gc0308_yuv_qvga_verify[i][0],&value);
      if(ret<0)return ret;
      if(value!=g_gc0308_yuv_qvga_verify[i][1])return -EIO;
    }
  printf("[Cam] GC0308 retained YUYV configuration resumed\n");
  return 0;
}

int gc0308_shutdown(void)
{
  uint8_t analog = 0;
  int ret;

  /* Match gc0308_set_stream(false): page 0, analog-mode bit 0 set, then
   * disable the output.  A soft reset here is not part of the official
   * driver and can discard the sensor state needed by the next start.
   */

  ret = board_camera_sccb_write(GC0308_REG_PAGE, 0x00);
  ret |= board_camera_sccb_read(0x1a, &analog);
  ret |= board_camera_sccb_write(0x1a, analog | 0x01);
  ret |= board_camera_sccb_write(GC0308_REG_OUTPUT_EN, 0x00);
  return ret;
}
