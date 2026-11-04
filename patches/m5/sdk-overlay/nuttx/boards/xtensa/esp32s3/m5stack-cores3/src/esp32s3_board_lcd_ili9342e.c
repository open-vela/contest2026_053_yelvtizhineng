/****************************************************************************
 * boards/xtensa/esp32s3/m5stack-cores3/src/esp32s3_board_lcd_ili9342e.c
 *
 * M5Stack CoreS3 ILI9342C/ILI9342E display.  The vendor init sequence is
 * taken from the official esp-bsp CoreS3 BSP.
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/lcd/ili9341.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/signal.h>
#include <nuttx/spi/spi.h>

#include <arch/board/board.h>

#include "esp32s3_gpio.h"
#include "esp32s3_spi.h"
#include "hardware/esp32s3_gpio_sigmap.h"

#include "esp32s3-cores3.h"

#define ILI9342E_FREQUENCY (40 * 1000 * 1000)

struct lcd_cmd_s
{
  uint8_t cmd;
  uint8_t len;
  uint8_t data[16];
  uint16_t delay_ms;
};

struct ili9342e_dev_s
{
  struct ili9341_lcd_s lower;
  struct spi_dev_s *spi;
  struct lcd_dev_s *lcd;
};

static const struct lcd_cmd_s g_init[] =
{
  {0xcf, 3, {0x00, 0xaa, 0xe0}, 0},
  {0xed, 4, {0x67, 0x03, 0x12, 0x81}, 0},
  {0xe8, 3, {0x8a, 0x01, 0x78}, 0},
  {0xcb, 5, {0x39, 0x2c, 0x00, 0x34, 0x02}, 0},
  {0xf7, 1, {0x20}, 0},
  {0xf7, 1, {0x20}, 0},
  {0xea, 2, {0x00, 0x00}, 0},
  {0xc0, 1, {0x23}, 0},
  {0xc1, 1, {0x11}, 0},
  {0xc5, 2, {0x43, 0x4c}, 0},
  {0xc7, 1, {0xa0}, 0},
  {0xb1, 2, {0x00, 0x1b}, 0},
  {0xf2, 1, {0x00}, 0},
  {0x26, 1, {0x01}, 0},
  {0xe0, 15, {0x1f, 0x36, 0x36, 0x3a, 0x0c, 0x05, 0x4f, 0x87,
                0x3c, 0x08, 0x11, 0x35, 0x19, 0x13, 0x00}, 0},
  {0xe1, 15, {0x00, 0x09, 0x09, 0x05, 0x13, 0x0a, 0x30, 0x78,
                0x43, 0x07, 0x0e, 0x0a, 0x26, 0x2c, 0x1f}, 0},
  {0xb7, 1, {0x07}, 0},
  {0xb6, 3, {0x08, 0x82, 0x27}, 0},
  {0xdd, 1, {0x01}, 0},
  {0x3a, 1, {0x55}, 0},
  {0x21, 0, {0}, 0},
  {0x36, 1, {0x08}, 0},
  {0xd5, 1, {0x00}, 0},
  {0xb1, 1, {0x22}, 0},
  {0xc8, 1, {0x38}, 0},
  {0xcb, 1, {0x1c}, 0},
  {0xc9, 1, {0x1a}, 0},
  {0xca, 1, {0x1a}, 0},
  {0xb7, 4, {0x5a, 0x41, 0x11, 0x19}, 0},
  {0xe4, 15, {0x04, 0x08, 0x11, 0x06, 0x12, 0x07, 0x3a, 0x76,
                0x47, 0x07, 0x0f, 0x0a, 0x11, 0x19, 0x05}, 0},
  {0xe5, 15, {0x02, 0x03, 0x07, 0x06, 0x12, 0x07, 0x36, 0x5f,
                0x48, 0x06, 0x10, 0x0c, 0x16, 0x14, 0x09}, 0},
  {0x11, 0, {0}, 120},
  {0x29, 0, {0}, 120},
};

/* CoreS3 revision 1 uses the ILI9342C controller.  The vendor sequence is
 * the ESP LCD ILI9341 default sequence followed by 16-bit pixels, the BGR
 * landscape MADCTL value and the normal display-on sequence.  The generic
 * ILI9341 driver resets the controller, so this is applied after its own
 * initialization just like the ILI9342E sequence above.
 */

static const struct lcd_cmd_s g_init_c[] =
{
  {0xcf, 3, {0x00, 0xaa, 0xe0}, 0},
  {0xed, 4, {0x67, 0x03, 0x12, 0x81}, 0},
  {0xe8, 3, {0x8a, 0x01, 0x78}, 0},
  {0xcb, 5, {0x39, 0x2c, 0x00, 0x34, 0x02}, 0},
  {0xf7, 1, {0x20}, 0},
  {0xf7, 1, {0x20}, 0},
  {0xea, 2, {0x00, 0x00}, 0},
  {0xc0, 1, {0x23}, 0},
  {0xc1, 1, {0x11}, 0},
  {0xc5, 2, {0x43, 0x4c}, 0},
  {0xc7, 1, {0xa0}, 0},
  {0xb1, 2, {0x00, 0x1b}, 0},
  {0xf2, 1, {0x00}, 0},
  {0x26, 1, {0x01}, 0},
  {0xe0, 15, {0x1f, 0x36, 0x36, 0x3a, 0x0c, 0x05, 0x4f, 0x87,
                0x3c, 0x08, 0x11, 0x35, 0x19, 0x13, 0x00}, 0},
  {0xe1, 15, {0x00, 0x09, 0x09, 0x05, 0x13, 0x0a, 0x30, 0x78,
                0x43, 0x07, 0x0e, 0x0a, 0x26, 0x2c, 0x1f}, 0},
  {0xb7, 1, {0x07}, 0},
  {0xb6, 3, {0x08, 0x82, 0x27}, 0},
  {0x3a, 1, {0x55}, 0},
  {0x21, 0, {0}, 0},
  {0x36, 1, {0x08}, 0},
  {0x11, 0, {0}, 120},
  {0x29, 0, {0}, 120},
};

static struct ili9342e_dev_s g_lcd;

static bool g_panel_is_e;

static void ili9342e_select(FAR struct ili9341_lcd_s *lcd)
{
  FAR struct ili9342e_dev_s *priv = (FAR struct ili9342e_dev_s *)lcd;

  /* SPI3 is shared with the SD slot.  A failed or later SD probe can leave
   * the controller at its 400 kHz identification clock and mode 0.  Restore
   * the panel settings for every transaction after taking the bus lock;
   * otherwise a full 320x240 frame takes about 3 seconds. */

  SPI_LOCK(priv->spi, true);
  SPI_SETMODE(priv->spi, SPIDEV_MODE2);
  SPI_SETFREQUENCY(priv->spi, ILI9342E_FREQUENCY);
  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), true);
}

static void ili9342e_deselect(FAR struct ili9341_lcd_s *lcd)
{
  FAR struct ili9342e_dev_s *priv = (FAR struct ili9342e_dev_s *)lcd;
  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), false);
  SPI_LOCK(priv->spi, false);
}

static int ili9342e_sendcmd(FAR struct ili9341_lcd_s *lcd, const uint8_t cmd)
{
  FAR struct ili9342e_dev_s *priv = (FAR struct ili9342e_dev_s *)lcd;
  SPI_SETBITS(priv->spi, 8);
  SPI_CMDDATA(priv->spi, SPIDEV_DISPLAY(0), true);
  SPI_SEND(priv->spi, cmd);
  return OK;
}

static int ili9342e_sendparam(FAR struct ili9341_lcd_s *lcd,
                              const uint8_t param)
{
  FAR struct ili9342e_dev_s *priv = (FAR struct ili9342e_dev_s *)lcd;
  SPI_SETBITS(priv->spi, 8);
  SPI_CMDDATA(priv->spi, SPIDEV_DISPLAY(0), false);
  SPI_SEND(priv->spi, param);
  return OK;
}

/* Camera GDMA and the CPU share PSRAM bandwidth. Direct GPSPI TX from
 * PSRAM can underflow its FIFO at 40 MHz, corrupting the displayed pixels
 * even though the source frame and transfer-complete interrupt are valid.
 * Stage eight rows in internal SRAM; SPI_SNDBLOCK completes synchronously
 * before this buffer is reused. The selected SPI bus serializes callers.
 * Keep the address window and RAMWR command open across chunks.
 */

#define ILI9342E_MAX_TX_WORDS 2560u
static uint16_t g_lcd_txbuffer[ILI9342E_MAX_TX_WORDS] __attribute__((aligned(64)));

static void ili9342e_sendgram_chunked(FAR struct ili9342e_dev_s *priv,
                                      FAR const uint16_t *words,
                                      uint32_t nwords, bool swap_bytes)
{
  while (nwords > 0)
    {
      uint32_t chunk = nwords > ILI9342E_MAX_TX_WORDS ?
                       ILI9342E_MAX_TX_WORDS : nwords;

      if (swap_bytes)
        {
          uint32_t i;
          for (i = 0; i < chunk; i++)
            {
              g_lcd_txbuffer[i] = swap16(words[i]);
            }
        }
      else
        {
          memcpy(g_lcd_txbuffer, words, chunk * sizeof(uint16_t));
        }

      SPI_SNDBLOCK(priv->spi, g_lcd_txbuffer, chunk);
      words += chunk;
      nwords -= chunk;
    }
}

static int ili9342e_sendgram(FAR struct ili9341_lcd_s *lcd,
                             FAR const uint16_t *wd, uint32_t nwords)
{
  FAR struct ili9342e_dev_s *priv = (FAR struct ili9342e_dev_s *)lcd;
  SPI_SETBITS(priv->spi, 16);
  SPI_CMDDATA(priv->spi, SPIDEV_DISPLAY(0), false);
  ili9342e_sendgram_chunked(priv, wd, nwords, true);
  return OK;
}

static int ili9342e_sendgram_raw(FAR struct ili9341_lcd_s *lcd,
                                 FAR const uint16_t *wd, uint32_t nwords)
{
  FAR struct ili9342e_dev_s *priv = (FAR struct ili9342e_dev_s *)lcd;

  SPI_SETBITS(priv->spi, 16);
  SPI_CMDDATA(priv->spi, SPIDEV_DISPLAY(0), false);
  ili9342e_sendgram_chunked(priv, wd, nwords, false);
  return OK;
}

static int ili9342e_backlight(FAR struct ili9341_lcd_s *lcd, int level)
{
  UNUSED(lcd);
  return board_cores3_backlight_set(level);
}

static void ili9342e_configure(FAR struct ili9341_lcd_s *lcd,
                               FAR const struct lcd_cmd_s *cmd)
{
  uint8_t i;

  lcd->select(lcd);
  lcd->sendcmd(lcd, cmd->cmd);
  for (i = 0; i < cmd->len; i++)
    {
      lcd->sendparam(lcd, cmd->data[i]);
    }
  lcd->deselect(lcd);

  if (cmd->delay_ms != 0)
    {
      nxsig_usleep(cmd->delay_ms * 1000);
    }
}

static void sw_lcd_bit(bool value)
{
  /* SPI mode 2: clock idles high and data is captured on the first edge. */

  esp32s3_gpiowrite(DISPLAY_MOSI, value);
  up_udelay(1);
  esp32s3_gpiowrite(DISPLAY_SCLK, false);
  up_udelay(1);
  esp32s3_gpiowrite(DISPLAY_SCLK, true);
  up_udelay(1);
}

static void sw_lcd_byte(uint8_t value)
{
  int bit;

  for (bit = 7; bit >= 0; bit--)
    {
      sw_lcd_bit((value & (1u << bit)) != 0);
    }
}

static void sw_lcd_configure(FAR const struct lcd_cmd_s *cmd)
{
  unsigned int i;

  esp32s3_gpiowrite(DISPLAY_CS, false);
  esp32s3_gpiowrite(DISPLAY_DC, false);
  sw_lcd_byte(cmd->cmd);
  esp32s3_gpiowrite(DISPLAY_DC, true);

  for (i = 0; i < cmd->len; i++)
    {
      sw_lcd_byte(cmd->data[i]);
    }

  esp32s3_gpiowrite(DISPLAY_CS, true);

  if (cmd->delay_ms != 0)
    {
      nxsig_usleep(cmd->delay_ms * 1000);
    }
}

int board_cores3_sw_lcd_test(uint16_t color)
{
  static const struct lcd_cmd_s window_col =
    {0x2a, 4, {0x00, 0x00, 0x01, 0x3f}, 0};
  static const struct lcd_cmd_s window_row =
    {0x2b, 4, {0x00, 0x00, 0x00, 0xef}, 0};
  uint8_t hi = color >> 8;
  uint8_t lo = color & 0xff;
  unsigned int i;
  uint32_t pixel;

  /* Detach SPI3 from its GPIO-matrix outputs and drive the panel through a
   * deliberately simple software SPI path.  This isolates the LCD itself
   * and its wiring from the NuttX SPI controller.
   */

  esp32s3_configgpio(DISPLAY_CS, OUTPUT);
  esp32s3_configgpio(DISPLAY_DC, OUTPUT);
  esp32s3_configgpio(DISPLAY_SCLK, OUTPUT);
  esp32s3_configgpio(DISPLAY_MOSI, OUTPUT);

  esp32s3_gpio_matrix_out(DISPLAY_CS, SIG_GPIO_OUT_IDX, false, false);
  esp32s3_gpio_matrix_out(DISPLAY_DC, SIG_GPIO_OUT_IDX, false, false);
  esp32s3_gpio_matrix_out(DISPLAY_SCLK, SIG_GPIO_OUT_IDX, false, false);
  esp32s3_gpio_matrix_out(DISPLAY_MOSI, SIG_GPIO_OUT_IDX, false, false);

  esp32s3_gpiowrite(DISPLAY_CS, true);
  esp32s3_gpiowrite(DISPLAY_SCLK, true);
  esp32s3_gpiowrite(DISPLAY_DC, false);

  for (i = 0; i < sizeof(g_init_c) / sizeof(g_init_c[0]); i++)
    {
      sw_lcd_configure(&g_init_c[i]);
    }

  sw_lcd_configure(&window_col);
  sw_lcd_configure(&window_row);

  esp32s3_gpiowrite(DISPLAY_CS, false);
  esp32s3_gpiowrite(DISPLAY_DC, false);
  sw_lcd_byte(0x2c);
  esp32s3_gpiowrite(DISPLAY_DC, true);

  for (pixel = 0; pixel < 320u * 240u; pixel++)
    {
      sw_lcd_byte(hi);
      sw_lcd_byte(lo);
    }

  esp32s3_gpiowrite(DISPLAY_CS, true);
  return OK;
}

int board_lcd_initialize(void)
{
  FAR struct ili9341_lcd_s *lower = &g_lcd.lower;
  FAR const struct lcd_cmd_s *init_cmds;
  size_t init_count;
  uint8_t touch_id = 0;
  unsigned int i;
  int ret;

  /* DISPLAY_DC is a plain GPIO that the SPI peripheral does not control.
   * Nothing else configures it on the boot path, so set it up here before
   * the first transfer.  sw_lcd_test() does the same for its bit-bang path.
   */

  esp32s3_configgpio(DISPLAY_DC, OUTPUT);
  esp32s3_gpio_matrix_out(DISPLAY_DC, SIG_GPIO_OUT_IDX, false, false);
  esp32s3_gpiowrite(DISPLAY_DC, false);

  ret = board_cores3_ioexp_init();
  if (ret < 0)
    {
      lcderr("ERROR: CoreS3 I/O expander init failed: %d\n", ret);
      return ret;
    }

  ret = board_cores3_ioexp_set_dir(BOARD_IOEXP_LCD_RST, true);
  ret |= board_cores3_ioexp_write(BOARD_IOEXP_LCD_RST, false);
  nxsig_usleep(10 * 1000);
  ret |= board_cores3_ioexp_write(BOARD_IOEXP_LCD_RST, true);
  nxsig_usleep(50 * 1000);
  if (ret < 0)
    {
      return ret;
    }

  if (board_cores3_touch_firmware_id(&touch_id) == OK && touch_id == 0x12)
    {
      g_panel_is_e = true;
    }
  else
    {
      g_panel_is_e = false;
    }

  g_lcd.spi = esp32s3_spibus_initialize(DISPLAY_SPI);
  if (g_lcd.spi == NULL)
    {
      lcderr("ERROR: Failed to initialize SPI%d\n", DISPLAY_SPI);
      return -ENODEV;
    }

  /* StackChan's official CoreS3 board code configures the panel IO with
   * spi_mode = 2.  Mode 0 accepts the transfers but the panel does not
   * latch them, which presents as an entirely black display.
   */

  SPI_SETMODE(g_lcd.spi, SPIDEV_MODE2);
  SPI_SETBITS(g_lcd.spi, 8);
  SPI_SETFREQUENCY(g_lcd.spi, ILI9342E_FREQUENCY);

  lower->select    = ili9342e_select;
  lower->deselect  = ili9342e_deselect;
  lower->sendcmd   = ili9342e_sendcmd;
  lower->sendparam = ili9342e_sendparam;
  lower->recvparam = NULL;
  lower->sendgram  = ili9342e_sendgram;
  lower->sendgram_raw = ili9342e_sendgram_raw;
  lower->recvgram  = NULL;
  lower->backlight = ili9342e_backlight;

  g_lcd.lcd = ili9341_initialize(lower, 0);
  if (g_lcd.lcd == NULL)
    {
      lcderr("ERROR: ili9341_initialize() failed\n");
      return -ENODEV;
    }

  /* The generic ILI9341 driver performs a software reset as part of its
   * own initialization.  Apply the matching CoreS3 vendor sequence after
   * the generic driver has finished so it is the final panel configuration.
   */

  if (g_panel_is_e)
    {
      init_cmds = g_init;
      init_count = sizeof(g_init) / sizeof(g_init[0]);
    }
  else
    {
      init_cmds = g_init_c;
      init_count = sizeof(g_init_c) / sizeof(g_init_c[0]);
    }

  for (i = 0; i < init_count; i++)
    {
      ili9342e_configure(lower, &init_cmds[i]);
    }

  /* The generic driver selects a rotated landscape MADCTL for its
   * 240x320 default panel. CoreS3 is a native 320x240 panel, so restore
   * the vendor orientation after the generic driver has initialised.
   */

  lower->select(lower);
  lower->sendcmd(lower, ILI9341_MEMORY_ACCESS_CONTROL);
  lower->sendparam(lower, 0x08);
  lower->deselect(lower);

  g_lcd.lcd->setpower(g_lcd.lcd, 1);
  return board_cores3_backlight_set(100);
}

FAR struct lcd_dev_s *board_lcd_getdev(int devno)
{
  UNUSED(devno);
  return g_lcd.lcd;
}

void board_lcd_uninitialize(void)
{
  if (g_lcd.lcd != NULL)
    {
      g_lcd.lcd->setpower(g_lcd.lcd, 0);
    }
}
