/****************************************************************************
 * boards/xtensa/esp32s3/m5stack-cores3/src/esp32s3_board_cores3.c
 *
 * CoreS3 board control: AW9523B I/O expander and AXP2101 PMIC.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>

#include "esp32s3_i2c.h"
#include "esp32s3_gpio.h"

#include "esp32s3-cores3.h"

#define AW9523_REG_INPUT0       0x00
#define AW9523_REG_OUTPUT0      0x02
#define AW9523_REG_OUTPUT1      0x03
#define AW9523_REG_CONFIG0      0x04
#define AW9523_REG_CONFIG1      0x05
#define AW9523_REG_GCR          0x11
#define AW9523_REG_LEDMODE0     0x12
#define AW9523_REG_LEDMODE1     0x13

static FAR struct i2c_master_s *g_i2c;
static uint8_t g_pmic_en;
static bool g_pmic_en_valid;
static bool g_ioexp_initialized;
static mutex_t g_ioexp_lock = NXMUTEX_INITIALIZER;

static bool cores3_i2c_bus_free(void)
{
  esp32s3_configgpio(BOARD_I2C_SCL, INPUT | PULLUP);
  esp32s3_configgpio(BOARD_I2C_SDA, INPUT | PULLUP);
  nxsig_usleep(1 * 1000);
  return esp32s3_gpioread(BOARD_I2C_SCL) && esp32s3_gpioread(BOARD_I2C_SDA);
}

static int cores3_i2c_bus_recover(void)
{
  int attempt;
  int i;

  /* A CPU-only reset can leave a peripheral holding SDA (mid-byte) or SCL
   * (clock stretch) low, which then blocks every transfer with ETIMEDOUT.
   * Recover with the standard sequence, but verify the line levels instead
   * of clocking blindly:
   *
   * 1. Release both lines and wait longer than the SMBus 25..35 ms slave
   *    timeout so a stretching slave lets go of SCL on its own.
   * 2. If only SDA is stuck, clock SCL until the slave releases it.
   * 3. Finish with a STOP condition and re-check the levels.
   */

  up_putc('1');
  up_putc(esp32s3_gpioread(BOARD_I2C_SCL) ? 'H' : 'L');
  up_putc(esp32s3_gpioread(BOARD_I2C_SDA) ? 'h' : 'l');

  for (attempt = 0; attempt < 3; attempt++)
    {
      if (cores3_i2c_bus_free())
        {
          up_putc('R');
          return OK;
        }

      nxsig_usleep(60 * 1000);

      if (esp32s3_gpioread(BOARD_I2C_SCL) &&
          !esp32s3_gpioread(BOARD_I2C_SDA))
        {
          esp32s3_configgpio(BOARD_I2C_SCL, OUTPUT | OPEN_DRAIN | PULLUP);

          for (i = 0; i < 32 && !esp32s3_gpioread(BOARD_I2C_SDA); i++)
            {
              esp32s3_gpiowrite(BOARD_I2C_SCL, false);
              nxsig_usleep(5 * 1000);
              esp32s3_gpiowrite(BOARD_I2C_SCL, true);
              nxsig_usleep(5 * 1000);
            }

          esp32s3_configgpio(BOARD_I2C_SCL, INPUT | PULLUP);
        }

      /* STOP: SDA low while SCL rises, then SDA rises */

      esp32s3_configgpio(BOARD_I2C_SDA, OUTPUT | OPEN_DRAIN | PULLUP);
      esp32s3_configgpio(BOARD_I2C_SCL, OUTPUT | OPEN_DRAIN | PULLUP);
      esp32s3_gpiowrite(BOARD_I2C_SDA, false);
      nxsig_usleep(5 * 1000);
      esp32s3_gpiowrite(BOARD_I2C_SCL, true);
      nxsig_usleep(5 * 1000);
      esp32s3_gpiowrite(BOARD_I2C_SDA, true);
      nxsig_usleep(5 * 1000);

      if (cores3_i2c_bus_free())
        {
          return OK;
        }
    }

  up_putc('N');
  return -ETIMEDOUT;
}

/* GPIO-level recovery used when a transfer times out at runtime (e.g. a
 * slave got confused mid-transaction).  The driver instance is deliberately
 * kept alive: tearing it down while its interrupt is enabled crashes the
 * ISR (esp32s3_i2c.c:507), and a slave-side wedge is fixed by recovering
 * the pins alone.
 */

static void cores3_i2c_bus_reset(void)
{
  cores3_i2c_bus_recover();
}

static int cores3_i2c_get(FAR struct i2c_master_s **i2c)
{
  if (g_i2c == NULL)
    {
      cores3_i2c_bus_recover();
      g_i2c = esp32s3_i2cbus_initialize(BOARD_I2C_BUS);
      nxsig_usleep(10 * 1000);  /* let pins/FSM settle after bus recovery - first transfer raced the reconfig and silently reset the chip (2026-10-19 boot loop) */
    }

  if (g_i2c == NULL)
    {
      return -ENODEV;
    }

  *i2c = g_i2c;
  return OK;
}

static int cores3_i2c_transfer_write(uint8_t addr, FAR const uint8_t *buf,
                                     size_t len)
{
  FAR struct i2c_master_s *i2c;
  struct i2c_config_s cfg;
  int ret;

  ret = cores3_i2c_get(&i2c);
  if (ret < 0)
    {
      return ret;
    }

  cfg.frequency = BOARD_I2C_FREQ;
  cfg.address   = addr;
  cfg.addrlen   = 7;

  return i2c_write(i2c, &cfg, buf, len);
}

static int cores3_i2c_write(uint8_t addr, FAR const uint8_t *buf,
                            size_t len)
{
  int ret = cores3_i2c_transfer_write(addr, buf, len);

  if (ret == -ETIMEDOUT)
    {
      cores3_i2c_bus_reset();
      ret = cores3_i2c_transfer_write(addr, buf, len);
    }

  return ret < 0 ? ret : OK;
}

static int cores3_i2c_read_reg(uint8_t addr, uint8_t reg, FAR uint8_t *val)
{
  FAR struct i2c_master_s *i2c;
  struct i2c_config_s cfg;
  int ret;

  ret = cores3_i2c_get(&i2c);
  if (ret < 0)
    {
      return ret;
    }

  cfg.frequency = BOARD_I2C_FREQ;
  cfg.address   = addr;
  cfg.addrlen   = 7;

  /* Keep the register pointer and data read in one bus-locked transfer.
   * Separate i2c_write/read calls let another task select a different
   * register, and a later RMW can then overwrite unrelated GPIO bits. */
  ret = i2c_writeread(i2c, &cfg, &reg, 1, val, 1);
  if (ret == -ETIMEDOUT)
    {
      cores3_i2c_bus_reset();
      ret = cores3_i2c_get(&i2c);
      if (ret == OK)
        {
          ret = i2c_writeread(i2c, &cfg, &reg, 1, val, 1);
        }
    }

  return ret < 0 ? ret : OK;
}

static int cores3_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
  uint8_t buf[2] = {reg, val};
  return cores3_i2c_write(addr, buf, sizeof(buf));
}

int board_cores3_ioexp_read_reg(uint8_t reg, FAR uint8_t *value)
{
  if (value == NULL)
    {
      return -EINVAL;
    }

  return cores3_i2c_read_reg(BOARD_IOEXP_ADDR, reg, value);
}

int board_cores3_pmic_read_reg(uint8_t reg, FAR uint8_t *value)
{
  if (value == NULL)
    {
      return -EINVAL;
    }

  return cores3_i2c_read_reg(BOARD_PMIC_ADDR, reg, value);
}

int board_cores3_touch_firmware_id(FAR uint8_t *firmware_id)
{
  uint8_t work_mode[2] = {0x00, 0x00};
  int ret;

  if (firmware_id == NULL)
    {
      return -EINVAL;
    }

  ret = cores3_i2c_write(TOUCHSCEEN_ADDR, work_mode, sizeof(work_mode));
  if (ret < 0)
    {
      /* A wedge that only affects the touch controller (it was interrupted
       * mid-read and now stretches SCL whenever it is addressed) does not
       * recover with GPIO clocking, because that would need SCL to be free.
       * P0_0 is BUS_OUT_EN on CoreS3, not a touch reset, so just give the
       * controller time to release SCL and retry once.
       */

      nxsig_usleep(300 * 1000);

      ret = cores3_i2c_write(TOUCHSCEEN_ADDR, work_mode, sizeof(work_mode));
      if (ret < 0)
        {
          return ret;
        }
    }

  nxsig_usleep(20 * 1000);
  return cores3_i2c_read_reg(TOUCHSCEEN_ADDR, 0xa6, firmware_id);
}

static int cores3_pmic_charge_init(void)
{
  uint8_t value,charge,verify;
  int ret;
  /* Retain the existing fuel-gauge and ADC setup. Never disable charge,
   * thermal, voltage, watchdog or battery protections here. */
  ret=cores3_i2c_read_reg(BOARD_PMIC_ADDR,0x18,&value);
  if(ret<0)return ret;
  if(!(value&0x08))
    {ret=cores3_i2c_write_reg(BOARD_PMIC_ADDR,0x18,value|0x08);if(ret<0)return ret;}
  ret=cores3_i2c_read_reg(BOARD_PMIC_ADDR,0x30,&value);
  if(ret<0)return ret;
  ret=cores3_i2c_write_reg(BOARD_PMIC_ADDR,0x30,value|0x3f);
  if(ret<0)return ret;
  /* Exact official v1.5.1 Pmic constructor policy:700mA, code13.
   * Preserve reg62[7:5], and DO NOT overwrite reg16 input-current policy.
   * The old code10 was400mA (not its500mA comment). This alignment does not
   * establish that charging was the motor fault's cause. */
  ret=cores3_i2c_read_reg(BOARD_PMIC_ADDR,0x62,&charge);
  if(ret<0)return ret;
  charge=(charge&0xe0)|0x0d;
  ret=cores3_i2c_write_reg(BOARD_PMIC_ADDR,0x62,charge);
  if(ret<0)return ret;
  ret=cores3_i2c_read_reg(BOARD_PMIC_ADDR,0x62,&verify);
  if(ret<0)return ret;
  return verify==charge?OK:-EIO;
}

static int cores3_pmic_boot_rails_init(void)
{
  uint8_t enabled;
  int ret;
  /* Match the official StackChan PMIC boot rails, including ALDO4 and
   * BLDO2. A CPU reset preserves these registers; a full power cycle does
   * not. Never depend on a previous factory-firmware boot to set them. */
  const uint8_t voltage_regs[]={0x94,0x95,0x97};
  for(unsigned i=0;i<sizeof(voltage_regs);i++)
    {
      ret=cores3_i2c_write_reg(BOARD_PMIC_ADDR,voltage_regs[i],0x1c);
      if(ret<0)return ret;
    }
  ret=cores3_i2c_read_reg(BOARD_PMIC_ADDR,BOARD_PMIC_REG_EN,&enabled);
  if(ret<0)return ret;
  enabled|=0xbf;
  ret=cores3_i2c_write_reg(BOARD_PMIC_ADDR,BOARD_PMIC_REG_EN,enabled);
  if(ret<0)return ret;
  g_pmic_en=enabled;g_pmic_en_valid=true;
  nxsig_usleep(20*1000);
  return OK;
}

static int board_cores3_ioexp_init_locked(void)
{
  int ret;

  /* SD, LCD and touch initialization all need the expander.  Re-running
   * the full output reset from the later touch step would clear P1_1 and
   * put the already-initialized LCD back into reset, leaving the panel
   * completely black.  Configure the expander exactly once per boot.
   */

  if (g_ioexp_initialized)
    {
      return OK;
    }

  ret=cores3_pmic_boot_rails_init();
  if(ret<0)return ret;

  /* Set the PMIC input/charge limits while the USB charge path is still
   * closed; closing it afterwards cannot draw a 1.5 A default inrush.
   */

  ret=cores3_pmic_charge_init();
  if(ret<0)return ret;

  /* Every port must be GPIO rather than LED PWM.  GCR bit 4 makes P0
   * push-pull on CoreS3; P1 is push-pull by default on AW9523B.
   */

  ret = cores3_i2c_write_reg(BOARD_IOEXP_ADDR, AW9523_REG_LEDMODE0, 0xff);
  if (ret < 0)
    {
      return ret;
    }

  ret = cores3_i2c_write_reg(BOARD_IOEXP_ADDR, AW9523_REG_LEDMODE1, 0xff);
  if (ret < 0)
    {
      return ret;
    }

  ret = cores3_i2c_write_reg(BOARD_IOEXP_ADDR, AW9523_REG_GCR, 0x10);
  if (ret < 0)
    {
      return ret;
    }

  /* P0_0 (BUS_OUT_EN) high enables U14 (ME1502A, active-high) so the USB-C
   * 5V rail reaches the board 5V net; P0_1 is the AW88298 reset (active
   * low), P0_2 its enable.  P0_4 (USB_OTG_EN) latch stays 0 and the pin is
   * configured as an input below, exactly like the official M5 firmware.
   * P0_5 must also stay LOW: it gates the charge input path.  Driving it
   * high (an earlier experiment, confounded by a half-seated body
   * connector) held VBUS off the AXP2101 forever, so the battery never
   * charged.  Official output latch value is 0x07 - keep it verbatim.
   */

  ret = cores3_i2c_write_reg(BOARD_IOEXP_ADDR, AW9523_REG_OUTPUT0, 0x07);
  if (ret < 0)
    {
      return ret;
    }

  /* 0x8f: CAM_RST, LCD_RST, P1_2/P1_3 latches and P1_7 high, exactly as
   * the M5 StackChan reference firmware programs the expander.
   */

  ret = cores3_i2c_write_reg(BOARD_IOEXP_ADDR, AW9523_REG_OUTPUT1, 0x8f);
  if (ret < 0)
    {
      return ret;
    }

  /* P0_4 (USB_OTG_EN) is an input, matching the official firmware: the
   * U18 enable net then sits low through the ME1502C internal pull-down,
   * which keeps the USB-C 5V -> AXP2101 VBUS charge path connected.
   * Driving it high (an earlier experiment) actively opened U18 and the
   * battery slowly drained to death while plugged in.  P0_3 (TF_SW)
   * stays an output low so the SD rail remains on.
   */

  ret = cores3_i2c_write_reg(BOARD_IOEXP_ADDR, AW9523_REG_CONFIG0, 0x10);
  if (ret < 0)
    {
      return ret;
    }

  /* P1_2 is the touch interrupt input; all other P1 pins are outputs. */

  ret = cores3_i2c_write_reg(BOARD_IOEXP_ADDR, AW9523_REG_CONFIG1, 0x0c);
  if (ret < 0)
    {
      return ret;
    }



  g_ioexp_initialized = true;
  return OK;
}

int board_cores3_ioexp_init(void)
{
  int ret = nxmutex_lock(&g_ioexp_lock);
  if (ret < 0) return ret;
  ret = board_cores3_ioexp_init_locked();
  nxmutex_unlock(&g_ioexp_lock);
  return ret;
}

static int board_cores3_ioexp_set_dir_locked(uint8_t pin, bool output)
{
  uint8_t reg;
  uint8_t val;
  int ret;

  if (pin > 15)
    {
      return -EINVAL;
    }

  reg = AW9523_REG_CONFIG0 + (pin >> 3);
  ret = cores3_i2c_read_reg(BOARD_IOEXP_ADDR, reg, &val);
  if (ret < 0)
    {
      return ret;
    }

  if (output)
    {
      val &= ~(1u << (pin & 7));
    }
  else
    {
      val |= 1u << (pin & 7);
    }

  return cores3_i2c_write_reg(BOARD_IOEXP_ADDR, reg, val);
}

int board_cores3_ioexp_set_dir(uint8_t pin, bool output)
{
  int ret = nxmutex_lock(&g_ioexp_lock);
  if (ret < 0) return ret;
  ret = board_cores3_ioexp_set_dir_locked(pin, output);
  nxmutex_unlock(&g_ioexp_lock);
  return ret;
}

static int board_cores3_ioexp_write_locked(uint8_t pin, bool value)
{
  uint8_t reg;
  uint8_t val;
  int ret;

  if (pin > 15)
    {
      return -EINVAL;
    }

  reg = AW9523_REG_OUTPUT0 + (pin >> 3);
  ret = cores3_i2c_read_reg(BOARD_IOEXP_ADDR, reg, &val);
  if (ret < 0)
    {
      return ret;
    }

  if (value)
    {
      val |= 1u << (pin & 7);
    }
  else
    {
      val &= ~(1u << (pin & 7));
    }

  return cores3_i2c_write_reg(BOARD_IOEXP_ADDR, reg, val);
}

int board_cores3_ioexp_write(uint8_t pin, bool value)
{
  int ret = nxmutex_lock(&g_ioexp_lock);
  if (ret < 0) return ret;
  ret = board_cores3_ioexp_write_locked(pin, value);
  nxmutex_unlock(&g_ioexp_lock);
  return ret;
}

int board_cores3_ioexp_read(uint8_t pin, FAR bool *value)
{
  uint8_t reg;
  uint8_t val;
  int ret;

  if (pin > 15 || value == NULL)
    {
      return -EINVAL;
    }

  reg = AW9523_REG_INPUT0 + (pin >> 3);
  ret = cores3_i2c_read_reg(BOARD_IOEXP_ADDR, reg, &val);
  if (ret < 0)
    {
      return ret;
    }

  *value = (val & (1u << (pin & 7))) != 0;
  return OK;
}

static uint8_t cores3_pmic_enable_bit(uint8_t reg)
{
  switch (reg)
    {
      case BOARD_PMIC_ALDO1: return 1u << 0;
      case BOARD_PMIC_ALDO2: return 1u << 1;
      case BOARD_PMIC_ALDO3: return (1u << 2) | (1u << 4) | (1u << 5);
      case BOARD_PMIC_ALDO4: return 1u << 3;
      case BOARD_PMIC_DLDO1: return 1u << 7;
      default:               return 0;
    }
}

int board_cores3_pmic_set_ldo(uint8_t reg, int millivolts)
{
  uint8_t bit;
  uint8_t next;
  int ret;

  if (reg < BOARD_PMIC_ALDO1 || reg > BOARD_PMIC_DLDO1 ||
      millivolts < 0 || millivolts > 3300)
    {
      return -EINVAL;
    }

  bit = cores3_pmic_enable_bit(reg);
  if (!g_pmic_en_valid)
    {
      ret = cores3_i2c_read_reg(BOARD_PMIC_ADDR, BOARD_PMIC_REG_EN,
                                &g_pmic_en);
      if (ret < 0)
        {
          g_pmic_en = 0;
        }
      g_pmic_en_valid = true;
    }

  ret = cores3_i2c_write_reg(BOARD_PMIC_ADDR, reg,
                             millivolts < 500 ? 0 :
                             (uint8_t)((millivolts - 500) / 100));
  if (ret < 0)
    {
      return ret;
    }

  next = millivolts > 0 ? (g_pmic_en | bit) : (g_pmic_en & ~bit);
  if (next != g_pmic_en)
    {
      ret = cores3_i2c_write_reg(BOARD_PMIC_ADDR, BOARD_PMIC_REG_EN, next);
      if (ret < 0)
        {
          return ret;
        }
      g_pmic_en = next;
    }

  return OK;
}

int board_cores3_backlight_set(int percent)
{
  int millivolts;

  if (percent < 0)
    {
      percent = 0;
    }
  else if (percent > 100)
    {
      percent = 100;
    }

  millivolts = percent == 0 ? 0 : 2500 + percent * 8;
  return board_cores3_pmic_set_ldo(BOARD_PMIC_DLDO1, millivolts);
}

int board_cores3_audio_power(bool enable)
{
  int ret;

  ret = board_cores3_pmic_set_ldo(BOARD_PMIC_ALDO1, enable ? 1800 : 0);
  ret |= board_cores3_pmic_set_ldo(BOARD_PMIC_ALDO2, enable ? 3300 : 0);
  ret |= board_cores3_ioexp_set_dir(BOARD_IOEXP_AUDIO_EN, true);
  ret |= board_cores3_ioexp_write(BOARD_IOEXP_AUDIO_EN, enable);
  return ret;
}

int board_cores3_camera_power(bool enable)
{
  int ret;

  ret = board_cores3_pmic_set_ldo(BOARD_PMIC_ALDO3, enable ? 3300 : 0);
  ret |= board_cores3_ioexp_set_dir(BOARD_IOEXP_CAM_RST, true);
  ret |= board_cores3_ioexp_write(BOARD_IOEXP_CAM_RST, false);
  if (enable)
    {
      nxsig_usleep(10 * 1000);
      ret |= board_cores3_ioexp_write(BOARD_IOEXP_CAM_RST, true);
      nxsig_usleep(20 * 1000);
    }

  return ret;
}

int gt911_diag_snapshot(FAR uint32_t out[8])
{
  if (out == NULL)
    {
      return -EINVAL;
    }

  memset(out, 0, 8 * sizeof(uint32_t));
  return OK;
}

int board_cores3_power_off(void)
{
  uint8_t value;int rc=board_cores3_pmic_read_reg(0x10,&value);
  if(rc<0)return rc;
  return cores3_i2c_write_reg(BOARD_PMIC_ADDR,0x10,value|1);
}
