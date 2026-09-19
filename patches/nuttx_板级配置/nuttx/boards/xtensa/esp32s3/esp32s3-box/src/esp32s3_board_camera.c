/****************************************************************************
 * esp32s3_board_camera.c — OV3660 SCCB probe + XMCLK init
 *
 * Uses ported IDF cam_ll HAL (hal_cam_ll.h) for LEDC-based 20MHz XMCLK.
 * SCCB over hardware I2C0 (IO8=SDA, IO18=SCL), sensor address 0x3C.
 *
 * Phase 15 ✅ Camera OV3660 — PID=0x3660 confirmed.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>

#include "xtensa.h"
#include "esp32s3_gpio.h"
#include "esp32s3_i2c.h"

#include "hal_cam_ll.h"

/* ── XMCLK GPIO output ──────────────────────────────────────────── */

#define XMCLK_SIG_IDX  73   /* LEDC_LS_SIG_OUT0_IDX */
#define XMCLK_GPIO     39   /* CHD ESP32-S3-BOX: XMCLK=IO39 (confirmed) */

/* ── SCCB over hardware I2C ──────────────────────────────────────── */

#define SCCB_ADDR 0x3C
#define SCCB_FREQ 100000
#define LITTLETOBIG(x) (((x) << 8) | ((x) >> 8))

static int sccb_read(FAR struct i2c_master_s *i2c,
                     const struct i2c_config_s *cfg, uint16_t reg)
{
  uint16_t s = LITTLETOBIG(reg);
  uint8_t buf[2] = { s & 0xff, (s >> 8) & 0xff }, data = 0;
  int ret = i2c_write(i2c, cfg, buf, 2);
  if (ret < 0) return ret;
  ret = i2c_read(i2c, cfg, &data, 1);
  return (ret < 0) ? ret : (int)data;
}

static int sccb_write(FAR struct i2c_master_s *i2c,
                      const struct i2c_config_s *cfg, uint16_t reg, uint8_t val)
{
  uint16_t s = LITTLETOBIG(reg);
  uint8_t buf[3] = { s & 0xff, (s >> 8) & 0xff, val };
  return i2c_write(i2c, cfg, buf, 3);
}

/* ── Public API ──────────────────────────────────────────────────── */

int board_camera_xmclk_init(void)
{
  static bool s_ready;

  /* Idempotent: only configure the LEDC XMCLK ONCE.
   *
   * Every `plant cam capture/reg` calls camera_init() -> probe -> this
   * function.  Re-running the LEDC timer/channel reset mid-stream GLITCHES
   * the sensor clock (PLL/AEC/AWB get disturbed) -> the image never
   * converges (observed: frames oscillate dark/bright and green/red/blue
   * across repeated captures).  Keep the first config and leave it. */

  if (s_ready)
    {
      return 0;
    }

  cam_ll_xmclk_init(20000000);
  esp32s3_configgpio(XMCLK_GPIO, OUTPUT);
  esp32s3_gpio_matrix_out(XMCLK_GPIO, XMCLK_SIG_IDX, 0, 0);
  s_ready = true;
  return 0;
}

int board_camera_sccb_probe(uint16_t *pid)
{
  if (!pid) return -EINVAL;

  board_camera_xmclk_init();

  FAR struct i2c_master_s *i2c = esp32s3_i2cbus_initialize(0);
  if (!i2c) return -ENODEV;

  struct i2c_config_s cfg = {
    .frequency = SCCB_FREQ,
    .address   = SCCB_ADDR,
    .addrlen   = 7,
  };

  /* Wake sensor: clear software standby */
  sccb_write(i2c, &cfg, 0x3008, 0x02);

  /* PID = (PID_H << 8) | PID_L, 0x300A=high, 0x300B=low */
  int hi = sccb_read(i2c, &cfg, 0x300A);
  int lo = sccb_read(i2c, &cfg, 0x300B);
  if (hi < 0 || lo < 0) return -ENODEV;

  *pid = ((uint16_t)(uint8_t)hi << 8) | (uint8_t)lo;
  return (*pid == 0x3660) ? 0 : -ENODEV;
}

/****************************************************************************
 * Name: board_camera_sccb_write / board_camera_sccb_read
 *
 * 通用 SCCB 读写（供 apps 层 camera_capture 配置 OV3660 寄存器）。
 * 每次调用重新初始化 I2C0（轻量，SCCB 频率低）。
 ****************************************************************************/

int board_camera_sccb_write(uint16_t reg, uint8_t val)
{
  FAR struct i2c_master_s *i2c = esp32s3_i2cbus_initialize(0);
  struct i2c_config_s cfg;
  int ret;

  if (!i2c) return -ENODEV;

  memset(&cfg, 0, sizeof(cfg));
  cfg.frequency = SCCB_FREQ;
  cfg.address   = SCCB_ADDR;
  cfg.addrlen   = 7;

  ret = sccb_write(i2c, &cfg, reg, val);
  if (ret < 0)
    {
      return ret;
    }

  return 0;
}

int board_camera_sccb_read(uint16_t reg, uint8_t *val)
{
  FAR struct i2c_master_s *i2c = esp32s3_i2cbus_initialize(0);
  struct i2c_config_s cfg;
  int r;

  if (!i2c || !val) return -EINVAL;

  memset(&cfg, 0, sizeof(cfg));
  cfg.frequency = SCCB_FREQ;
  cfg.address   = SCCB_ADDR;
  cfg.addrlen   = 7;

  r = sccb_read(i2c, &cfg, reg);
  if (r < 0)
    {
      return r;
    }

  *val = (uint8_t)r;
  return 0;
}
