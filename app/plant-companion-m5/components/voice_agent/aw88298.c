/****************************************************************************
 * apps/plant-companion/components/voice_agent/aw88298.c
 *
 * CoreS3 AW88298 16-bit I2S amplifier.  Register values follow the
 * official esp_codec_dev AW88298 driver.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include <nuttx/i2c/i2c_master.h>

#include "aw88298.h"

#define AW88298_ADDR          0x36
#define AW88298_FREQ          (400 * 1000)

#define AW88298_REG_RESET     0x00
#define AW88298_REG_SYSCTRL   0x04
#define AW88298_REG_SYSCTRL2  0x05
#define AW88298_REG_I2SCTRL   0x06
#define AW88298_REG_VOLUME    0x0c
#define AW88298_REG_BSTCTRL2  0x61

static int aw88298_write(FAR struct i2c_master_s *i2c, uint8_t reg,
                         uint16_t value)
{
  struct i2c_config_s cfg = {
    .frequency = AW88298_FREQ,
    .address   = AW88298_ADDR,
    .addrlen   = 7,
  };
  uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
  int ret;

  ret = i2c_write(i2c, &cfg, buf, sizeof(buf));
  return ret < 0 ? ret : OK;
}

static int aw88298_read(FAR struct i2c_master_s *i2c, uint8_t reg,
                        FAR uint16_t *value)
{
  struct i2c_config_s cfg = {
    .frequency = AW88298_FREQ,
    .address   = AW88298_ADDR,
    .addrlen   = 7,
  };
  uint8_t buf[2];
  int ret;

  if (value == NULL)
    {
      return -EINVAL;
    }

  ret = i2c_write(i2c, &cfg, &reg, 1);
  if (ret < 0)
    {
      return ret;
    }

  ret = i2c_read(i2c, &cfg, buf, sizeof(buf));
  if (ret < 0)
    {
      return ret;
    }

  *value = ((uint16_t)buf[0] << 8) | buf[1];
  return OK;
}

int aw88298_probe(FAR struct i2c_master_s *i2c)
{
  uint16_t value;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  return aw88298_read(i2c, AW88298_REG_SYSCTRL, &value);
}

int aw88298_init(FAR struct i2c_master_s *i2c)
{
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  /* CoreS3 official M5Unified sequence.  Do not substitute the ESP-BOX
   * 0x3cc8 I2SCTRL value: it programs a different BCK mode and leaves the
   * speaker silent on CoreS3. */

  ret = aw88298_write(i2c, AW88298_REG_BSTCTRL2, 0x0673);
  ret |= aw88298_write(i2c, AW88298_REG_SYSCTRL, 0x4040);
  ret |= aw88298_write(i2c, AW88298_REG_SYSCTRL2, 0x0008);
  ret |= aw88298_write(i2c, AW88298_REG_I2SCTRL, 0x14c0);
  ret |= aw88298_write(i2c, AW88298_REG_VOLUME, 0x3064);
  return ret;
}

int aw88298_set_format(FAR struct i2c_master_s *i2c, int sample_rate)
{
  uint16_t value;
  uint8_t rate_code;
  int ret;

  switch (sample_rate)
    {
      case 8000:  rate_code = 0x00; break;
      case 11025: rate_code = 0x01; break;
      case 12000: rate_code = 0x02; break;
      case 16000: rate_code = 0x03; break;
      case 22050: rate_code = 0x04; break;
      case 24000: rate_code = 0x05; break;
      case 32000: rate_code = 0x06; break;
      case 44100: rate_code = 0x07; break;
      case 48000: rate_code = 0x08; break;
      default:    return -EINVAL;
    }

  ret = aw88298_read(i2c, AW88298_REG_I2SCTRL, &value);
  if (ret < 0)
    {
      return ret;
    }

  /* 16-bit samples, codec is I2S slave. */
  value &= ~0xf0u;
  value = (value & ~0x0fu) | rate_code;
  return aw88298_write(i2c, AW88298_REG_I2SCTRL, value);
}

int aw88298_start(FAR struct i2c_master_s *i2c)
{
  if (i2c == NULL)
    {
      return -EINVAL;
    }

  /* I2SEN=1, AMPPD=0, PWDN=0 (official M5Unified CoreS3). */

  return aw88298_write(i2c, AW88298_REG_SYSCTRL, 0x4040);
}

int aw88298_standby(FAR struct i2c_master_s *i2c,bool standby)
{
  uint16_t value=0;
  int rc=aw88298_mute(i2c,true);if(rc<0)return rc;
  rc=aw88298_read(i2c,AW88298_REG_SYSCTRL,&value);if(rc<0)return rc;
  /* AMPPD and PWDN; leave I2S format and microphone clock intact. */
  value=standby?(value|3u):(value&~3u);
  return aw88298_write(i2c,AW88298_REG_SYSCTRL,value);
}

int aw88298_set_volume(FAR struct i2c_master_s *i2c, uint8_t vol)
{
  if (i2c == NULL)
    {
      return -EINVAL;
    }

  /* AW88298 datasheet V1.6, HAGCCFG4 (0x0c), page 35:
   * VOL[7:4] attenuates in 6dB steps; VOL[3:0] in 0.5dB steps.
   * They are NOT one binary half-dB counter. Keep the public 0..255
   * linear-amplitude scale and the existing hardware mute at zero.
   * Canonical fine values 0..11 cover each coarse step; 0xfc is -96dB.
   */
  unsigned steps=vol?(unsigned)(-40.0f*log10f((float)vol/255.0f)+0.5f):192;
  if(steps>192)steps=192;
  unsigned coarse=steps/12;
  if(coarse>15)coarse=15;
  unsigned fine=steps-coarse*12;
  unsigned attenuation=(coarse<<4)|fine;
  int ret=aw88298_write(i2c,AW88298_REG_VOLUME,(uint16_t)((attenuation<<8)|0x64));
  return ret<0?ret:aw88298_mute(i2c,vol==0);
}

int aw88298_mute(FAR struct i2c_master_s *i2c, bool mute)
{
  uint16_t value;
  int ret;

  ret = aw88298_read(i2c, AW88298_REG_SYSCTRL2, &value);
  if (ret < 0)
    {
      return ret;
    }

  if (mute)
    {
      value |= 1u << 4;
    }
  else
    {
      value &= ~(1u << 4);
    }

  return aw88298_write(i2c, AW88298_REG_SYSCTRL2, value);
}

int aw88298_dump_regs(FAR struct i2c_master_s *i2c)
{
  uint16_t value;
  int reg;

  for (reg = 0; reg <= 0x14; reg++)
    {
      if (reg == 0x08 || reg == 0x0d || reg == 0x0e || reg == 0x0f ||
          reg == 0x11)
        {
          continue;
        }

      if (aw88298_read(i2c, (uint8_t)reg, &value) == 0)
        {
          printf("AW88298[%02x]=%04x\n", reg, value);
        }
    }

  return OK;
}
