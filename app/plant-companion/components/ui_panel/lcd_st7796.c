/****************************************************************************
 * apps/plant-companion/components/ui_panel/lcd_st7796.c
 *
 * ST7796 LCD component — runtime control.
 * Hardware init (GPIO, reset, backlight, ST7796 commands) lives in
 * board-level esp32s3_board_lcd_st7789.c.
 * Touch controller (GT911) is initialized by board bringup.
 * This component provides runtime LCD and touch control APIs.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <errno.h>
#include <nuttx/board.h>
#include <nuttx/lcd/lcd.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * lcd_init - Initialize LCD component
 *
 * LCD hardware is initialized by board_lcd_initialize() during bringup.
 * Touch controller is initialized by board bringup (board_touchscreen_initialize).
 * This function verifies the LCD device is available.
 *
 * Return: 0 on success, negative errno on failure
 */

int lcd_init(void)
{
  struct lcd_dev_s *dev;

  /* Get LCD device from board */

  dev = board_lcd_getdev(0);
  if (!dev)
    {
      return -ENODEV;
    }

  return 0;
}
