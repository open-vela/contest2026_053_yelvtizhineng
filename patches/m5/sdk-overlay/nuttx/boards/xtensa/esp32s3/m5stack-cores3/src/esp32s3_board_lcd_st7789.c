/****************************************************************************
 * esp32s3_board_lcd_st7789.c
 *
 * Exact IDF ui_main_screen.c ST7796 init sequence + final RED fill.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include <debug.h>
#include <errno.h>

#include "xtensa.h"

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/signal.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/st7789.h>

#include <arch/board/board.h>

#include "esp32s3_gpio.h"
#include "esp32s3_spi.h"
#include "hardware/esp32s3_gpio_sigmap.h"
#include "esp32s3-box.h"

static struct spi_dev_s *g_spidev;
static struct lcd_dev_s *g_lcd;

/* ST7796 init commands from xiaozhi-esp32 — proven working on this panel */
static const struct {
  uint8_t cmd;
  const uint8_t *data;
  uint8_t len;
  uint16_t ms;
} g_st7796_init[] = {
  {0x11, NULL, 0, 120},
  {0x3A, (uint8_t[]){0x05}, 1, 0},
  {0xB0, (uint8_t[]){0x00, 0xF0}, 2, 0},  /* RAMCTRL: big endian data */
  {0xF0, (uint8_t[]){0xC3}, 1, 0},
  {0xF0, (uint8_t[]){0x96}, 1, 0},
  {0xB4, (uint8_t[]){0x01}, 1, 0},
  {0xB7, (uint8_t[]){0xC6}, 1, 0},
  {0xC0, (uint8_t[]){0x80, 0x45}, 2, 0},
  {0xC1, (uint8_t[]){0x13}, 1, 0},
  {0xC2, (uint8_t[]){0xA7}, 1, 0},
  {0xC5, (uint8_t[]){0x0A}, 1, 0},
  {0xE8, (uint8_t[]){0x40,0x8A,0x00,0x00,0x29,0x19,0xA5,0x33}, 8, 0},
  {0xE0, (uint8_t[]){0xD0,0x08,0x0F,0x06,0x06,0x33,0x30,0x33,0x47,0x17,0x13,0x13,0x2B,0x31}, 14, 0},
  {0xE1, (uint8_t[]){0xD0,0x0A,0x11,0x0B,0x09,0x07,0x2F,0x33,0x47,0x38,0x15,0x16,0x2C,0x32}, 14, 0},
  {0xF0, (uint8_t[]){0x3C}, 1, 0},
  {0xF0, (uint8_t[]){0x69}, 1, 120},
  {0x29, NULL, 0, 0},
};

int board_lcd_initialize(void)
{
  printf("lcd: ST7796 full init + RED fill\n");

  /* GPIO init */
  esp32s3_configgpio(DISPLAY_DC, OUTPUT);
  esp32s3_configgpio(DISPLAY_RST, OUTPUT);
  esp32s3_configgpio(DISPLAY_BCKL, OUTPUT);

  /* Hard reset */
  esp32s3_gpiowrite(DISPLAY_RST, false);
  nxsig_usleep(10 * 1000);
  esp32s3_gpiowrite(DISPLAY_RST, true);
  nxsig_usleep(10 * 1000);

  /* Backlight ON */
  esp32s3_gpiowrite(DISPLAY_BCKL, true);

  /* SPI init */
  g_spidev = esp32s3_spibus_initialize(DISPLAY_SPI);
  if (!g_spidev) { printf("lcd: SPI FAIL\n"); return -ENODEV; }

  /* Send all 17 ST7796 init commands */
  for (int i = 0; i < sizeof(g_st7796_init) / sizeof(g_st7796_init[0]); i++)
    {
      const uint8_t cmd = g_st7796_init[i].cmd;
      const uint8_t *data = g_st7796_init[i].data;
      const uint8_t len = g_st7796_init[i].len;
      const uint16_t ms = g_st7796_init[i].ms;

      SPI_LOCK(g_spidev, true);
      SPI_SETMODE(g_spidev, SPIDEV_MODE0);
      SPI_SETBITS(g_spidev, 8);
      SPI_SETFREQUENCY(g_spidev, CONFIG_LCD_ST7789_FREQUENCY);
      SPI_SELECT(g_spidev, SPIDEV_DISPLAY(0), true);

      SPI_CMDDATA(g_spidev, SPIDEV_DISPLAY(0), true);
      SPI_SEND(g_spidev, cmd);
      if (len > 0)
        {
          SPI_CMDDATA(g_spidev, SPIDEV_DISPLAY(0), false);
          SPI_SNDBLOCK(g_spidev, data, len);
        }

      SPI_SELECT(g_spidev, SPIDEV_DISPLAY(0), false);
      SPI_LOCK(g_spidev, false);

      if (ms > 0)
        nxsig_usleep(ms * 1000);
    }

  printf("lcd: ST7796 init complete\n");

  /* Now init ST7789 driver for LVGL.
   * The driver sends its own MADCTL (0x28 = MV+BGR, matching IDF swap_xy+BGR)
   * and does a test RED fill via st7789_lcdinitialize() → st7789_fill(). */

  g_lcd = st7789_lcdinitialize(g_spidev);
  if (!g_lcd) { printf("lcd: driver FAIL\n"); return -ENODEV; }

  printf("lcd: ready\n");
  return OK;
}

struct lcd_dev_s *board_lcd_getdev(int devno)
{
  return g_lcd;
}

void board_lcd_uninitialize(void)
{
  if (g_lcd) g_lcd->setpower(g_lcd, 0);
}
