/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-box/src/esp32s3_board_spi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <debug.h>

#include <nuttx/spi/spi.h>

#include "esp32s3_gpio.h"
#include "hardware/esp32s3_gpio_sigmap.h"
#include "esp32s3-cores3.h"


/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_spi2_status
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_SPI2

uint8_t esp32s3_spi2_status(struct spi_dev_s *dev, uint32_t devid)
{
  uint8_t status = 0;

  return status;
}

#endif

/****************************************************************************
 * Name: esp32s3_spi2_cmddata
 ****************************************************************************/

#if defined(CONFIG_ESP32S3_SPI2) && defined(CONFIG_SPI_CMDDATA)

int esp32s3_spi2_cmddata(struct spi_dev_s *dev, uint32_t devid, bool cmd)
{
  if (devid == SPIDEV_DISPLAY(0))
    {
      /*  This is the Data/Command control pad which determines whether the
       *  data bits are data or a command.
       */

      esp32s3_gpiowrite(DISPLAY_DC, !cmd);

      return OK;
    }

  spiinfo("devid: %" PRIu32 " CMD: %s\n", devid, cmd ? "command" :
          "data");

  return -ENODEV;
}

#endif

/****************************************************************************
 * Name: esp32s3_spi3_status
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_SPI3

uint8_t esp32s3_spi3_status(struct spi_dev_s *dev, uint32_t devid)
{
  uint8_t status = 0;

  /* CoreS3 has no SD card-detect pin; report the card always present so
   * mmcsd_open() does not reject every access with -ENODEV.
   */

  if (devid == SPIDEV_MMCSD(0))
    {
      status |= SPI_STATUS_PRESENT;
    }

  return status;
}

#endif

/****************************************************************************
 * Name: esp32s3_spi3_cmddata
 ****************************************************************************/

#if defined(CONFIG_ESP32S3_SPI3) && defined(CONFIG_SPI_CMDDATA)

int esp32s3_spi3_cmddata(struct spi_dev_s *dev, uint32_t devid, bool cmd)
{
  if (devid == SPIDEV_DISPLAY(0))
    {
      /*  This is the Data/Command control pad which determines whether the
       *  data bits are data or a command.
       */

      esp32s3_gpiowrite(DISPLAY_DC, !cmd);

      return OK;
    }

  spiinfo("devid: %" PRIu32 " CMD: %s\n", devid, cmd ? "command" :
          "data");

  return -ENODEV;
}

#endif

/****************************************************************************
 * Name: esp32s3_spi3_select
 *
 * Description:
 *   Select either the LCD (CS=GPIO3) or microSD (CS=GPIO4) on SPI3.
 *
 ****************************************************************************/

#if defined(CONFIG_ESP32S3_SPI3) && defined(CONFIG_ESP32S3_SPI_UDCS)

/* CoreS3 shares GPIO35 between LCD D/C (ESP32 output) and microSD MISO
 * (card output).  The pad direction must follow whoever currently owns the
 * bus, otherwise the ESP32 output driver fights the card.  LCD transfers
 * always reselect the display first, so restoring the output here keeps
 * both users consistent.
 */

static void cores3_g35_to_lcd_dc(void)
{
  esp32s3_configgpio(DISPLAY_DC, OUTPUT);
  esp32s3_gpio_matrix_out(DISPLAY_DC, SIG_GPIO_OUT_IDX, false, false);
}

static void cores3_g35_to_sd_miso(void)
{
  esp32s3_configgpio(DISPLAY_DC, INPUT | PULLUP);
  esp32s3_gpio_matrix_in(DISPLAY_DC, SPI3_Q_IN_IDX, 0);
}

void esp32s3_spi3_select(FAR struct spi_dev_s *dev, uint32_t devid,
                         bool selected)
{
  if (devid == SPIDEV_DISPLAY(0))
    {
      if (selected)
        {
          cores3_g35_to_lcd_dc();
        }

      esp32s3_gpiowrite(DISPLAY_CS, !selected);
    }
  else if (devid == SPIDEV_MMCSD(0))
    {
      if (selected)
        {
          cores3_g35_to_sd_miso();
        }
      else
        {
          cores3_g35_to_lcd_dc();
        }

      esp32s3_gpiowrite(SD_SPI_CS, !selected);
    }
}

#endif
