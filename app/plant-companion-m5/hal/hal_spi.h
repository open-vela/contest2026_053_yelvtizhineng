/****************************************************************************
 * apps/plant-companion/hal/hal_spi.h
 *
 * Thin abstraction over NuttX SPI API (SPI_LOCK/SELECT/SETMODE/SEND/...).
 * All SPI LCD and sensor code uses these macros — no direct SPI driver calls.
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_HAL_HAL_SPI_H
#define __APPS_PLANT_COMPANION_HAL_HAL_SPI_H

#include <nuttx/config.h>
#include <nuttx/spi/spi.h>
#include <stdbool.h>

/* Re-export NuttX SPI macros with HAL_ prefix for component portability */
#define hal_spi_lock(spi, lk)          SPI_LOCK(spi, lk)
#define hal_spi_select(spi, devid, en) SPI_SELECT(spi, devid, en)
#define hal_spi_setmode(spi, mode)     SPI_SETMODE(spi, mode)
#define hal_spi_setbits(spi, nbits)    SPI_SETBITS(spi, nbits)
#define hal_spi_setfreq(spi, hz)       SPI_SETFREQUENCY(spi, hz)
#define hal_spi_send(spi, word)        SPI_SEND(spi, word)
#define hal_spi_sndblock(spi, buf, nw) SPI_SNDBLOCK(spi, buf, nw)
#define hal_spi_cmddata(spi, devid, cmd) SPI_CMDDATA(spi, devid, cmd)

/* SPI mode constants */
#define HAL_SPI_MODE0  SPIDEV_MODE0
#define HAL_SPI_MODE3  SPIDEV_MODE3

/* Display device ID — must match board SPI cmddata implementation */
#define HAL_SPI_DISPLAY_DEV   SPIDEV_DISPLAY(0)

#endif /* __APPS_PLANT_COMPANION_HAL_HAL_SPI_H */
