/****************************************************************************
 * boards/xtensa/esp32s3/m5stack-cores3/src/esp32s3_board_sdspi.c
 *
 * CoreS3 microSD on SPI3 (shares CLK/MOSI/MISO with the LCD).
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <nuttx/signal.h>
#include <stdbool.h>

#include <nuttx/arch.h>
#include <nuttx/mmcsd.h>
#include <nuttx/spi/spi.h>

#include "esp32s3_spi.h"
#include "esp32s3_gpio.h"
#include "hardware/esp32s3_gpio_sigmap.h"

#include "esp32s3-cores3.h"

int board_sdspi_initialize(void)
{
  FAR struct spi_dev_s *spi;
  int ret;

  /* Expander and PMIC rails must be up before the card can be probed. */

  ret = board_cores3_ioexp_init();
  if (ret < 0)
    {
      return ret;
    }

  /* SD chip select is a plain GPIO driven by esp32s3_spi3_select(); the
   * SPI bus setup never touches it, so configure it deselected here.
   */

  esp32s3_configgpio(SD_SPI_CS, OUTPUT);
  esp32s3_gpio_matrix_out(SD_SPI_CS, SIG_GPIO_OUT_IDX, false, false);
  esp32s3_gpiowrite(SD_SPI_CS, true);

  spi = esp32s3_spibus_initialize(DISPLAY_SPI);
  if (spi == NULL)
    {
      return -ENODEV;
    }

  /* A CPU-only reset can leave the powered card mid-command.  Cycle its
   * supply BEFORE binding the block driver: mmcsd_spislotinitialize()
   * immediately calls mmcsd_mediainitialize(), and caches card type/state.
   * Cycling power afterwards invalidates that cache; mmcsd_open() then
   * skips initialization because the cached card type is already known.
   */

  ret = board_cores3_pmic_set_ldo(BOARD_PMIC_ALDO4, 0);
  if (ret < 0)
    {
      return ret;
    }

  nxsig_usleep(200 * 1000);

  ret = board_cores3_pmic_set_ldo(BOARD_PMIC_ALDO4, 3300);
  if (ret < 0)
    {
      return ret;
    }

  nxsig_usleep(50 * 1000);

  return mmcsd_spislotinitialize(0, 0, spi);
}

/****************************************************************************
 * Name: board_cores3_sd_diag
 *
 * Description:
 *   Raw SPI-mode SD probe for bring-up debugging: clocks the card with CS
 *   high, issues CMD0 and reports the R1 response bytes so CS/MOSI/MISO/
 *   clock/card-power can be verified without the mmc driver stack.
 *
 ****************************************************************************/

static FAR struct spi_dev_s *g_diag_spi;

static void sd_diag_r1(FAR const char *tag)
{
  uint8_t rx[8];
  int i;

  memset(rx, 0xff, sizeof(rx));
  SPI_RECVBLOCK(g_diag_spi, rx, sizeof(rx));

  syslog(LOG_ERR, "[SdDiag] %s:", tag);
  for (i = 0; i < sizeof(rx); i++)
    {
      syslog(LOG_ERR, " %02x", rx[i]);
    }
  syslog(LOG_ERR, "%s", "\n");
}

int board_cores3_sd_diag(void)
{
  FAR struct spi_dev_s *spi;
  uint8_t cmd0[6]  = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };
  uint8_t cmd8[6]  = { 0x48, 0x00, 0x00, 0x01, 0xaa, 0x87 };
  uint8_t cmd55[6] = { 0x77, 0x00, 0x00, 0x00, 0x00, 0xff };
  uint8_t acmd41[6]= { 0x69, 0x40, 0x00, 0x00, 0x00, 0xff };
  uint8_t cmd58[6] = { 0x7a, 0x00, 0x00, 0x00, 0x00, 0xff };
  uint8_t cmd17[6] = { 0x51, 0x00, 0x00, 0x00, 0x00, 0xff };
  uint8_t idle[16];
  uint8_t blk[64];
  int i;

  spi = esp32s3_spibus_initialize(DISPLAY_SPI);
  if (spi == NULL)
    {
      return -ENODEV;
    }

  g_diag_spi = spi;
  memset(idle, 0xff, sizeof(idle));

  /* Clean card power cycle: the in-app probe storm has been hammering the
   * card with CMD0 since boot, which can leave it stuck mid-init.
   */

  board_cores3_pmic_set_ldo(BOARD_PMIC_ALDO4, 0);
  nxsig_usleep(200 * 1000);
  board_cores3_pmic_set_ldo(BOARD_PMIC_ALDO4, 3300);
  nxsig_usleep(50 * 1000);

  SPI_LOCK(spi, true);
  SPI_SETMODE(spi, SPIDEV_MODE0);
  SPI_SETBITS(spi, 8);
  SPI_SETFREQUENCY(spi, 400000);

  esp32s3_spi3_select(spi, SPIDEV_MMCSD(0), false);
  SPI_SNDBLOCK(spi, idle, 10);

  esp32s3_spi3_select(spi, SPIDEV_MMCSD(0), true);

  SPI_SNDBLOCK(spi, cmd0, sizeof(cmd0));
  sd_diag_r1("CMD0 expect 01");

  SPI_SNDBLOCK(spi, cmd8, sizeof(cmd8));
  sd_diag_r1("CMD8 expect 01 000001aa");

  {
    int n_idle = 0;
    int n_none = 0;

    for (i = 0; i < 250; i++)
      {
        uint8_t r1[2];

        SPI_SNDBLOCK(spi, cmd55, sizeof(cmd55));
        SPI_RECVBLOCK(spi, r1, 2);
        SPI_SNDBLOCK(spi, acmd41, sizeof(acmd41));

        memset(blk, 0xff, 8);
        SPI_RECVBLOCK(spi, blk, 8);
        if (blk[0] == 0x00 || blk[1] == 0x00 || blk[2] == 0x00 ||
            blk[3] == 0x00 || blk[4] == 0x00 || blk[5] == 0x00)
          {
            syslog(LOG_ERR, "[SdDiag] ACMD41 ready after %d tries\n", i + 1);
            break;
          }

        if (blk[1] == 0x01)
          {
            n_idle++;
          }
        else if (blk[1] == 0xff)
          {
            n_none++;
          }

        if ((i % 25) == 24)
          {
            syslog(LOG_ERR,
                   "[SdDiag] ACMD41 try %d: last r1=%02x %02x %02x %02x "
                   "(idle=%d none=%d)\n",
                   i + 1, blk[0], blk[1], blk[2], blk[3], n_idle, n_none);
          }

        nxsig_usleep(20 * 1000);
      }

    if (i >= 250)
      {
        syslog(LOG_ERR, "[SdDiag] ACMD41 NEVER READY (idle=%d none=%d)\n",
               n_idle, n_none);
      }
  }

  SPI_SNDBLOCK(spi, cmd58, sizeof(cmd58));
  sd_diag_r1("CMD58 R1+OCR");

  SPI_SNDBLOCK(spi, cmd17, sizeof(cmd17));
  sd_diag_r1("CMD17 R1 then FE");

  for (i = 0; i < 8; i++)
    {
      memset(blk, 0, sizeof(blk));
      SPI_RECVBLOCK(spi, blk, sizeof(blk));
      syslog(LOG_ERR, "[SdDiag] blk%d: %02x %02x %02x %02x %02x %02x %02x %02x\n",
             i, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7]);
    }

  /* Full sector 0 read at 400kHz x3: check data stability and the
   * MBR signature at offset 510.
   */

  {
    static uint8_t sect[512];
    int r;

    for (r = 0; r < 3; r++)
      {
        SPI_SETFREQUENCY(spi, 400000);
        SPI_SNDBLOCK(spi, cmd17, sizeof(cmd17));
        memset(blk, 0, sizeof(blk));
        SPI_RECVBLOCK(spi, blk, 4);          /* R1 + wait + FE token */
        memset(sect, 0, sizeof(sect));
        SPI_RECVBLOCK(spi, sect, sizeof(sect));
        memset(blk, 0, sizeof(blk));
        SPI_RECVBLOCK(spi, blk, 2);          /* trailing CRC bytes */
        syslog(LOG_ERR,
               "[SdDiag] sec0 try%d: r1=%02x %02x %02x %02x head=%02x %02x %02x %02x sig=%02x %02x\n",
               r, blk[0], blk[1], blk[2], blk[3],
               sect[0], sect[1], sect[2], sect[3],
               sect[510], sect[511]);
      }
  }

  /* Reliability sweep: CMD9 (SEND_CSD) at increasing frequencies */

  {
    static const uint32_t freqs[] =
      {
        400000, 1000000, 2000000, 4000000,
        8000000, 10000000, 16000000, 20000000
      };
    uint8_t cmd9[6] = { 0x49, 0x00, 0x00, 0x00, 0x00, 0xff };
    int f;
    int t;

    for (f = 0; f < 8; f++)
      {
        SPI_SETFREQUENCY(spi, freqs[f]);
        for (t = 0; t < 4; t++)
          {
            uint8_t r[8];

            SPI_SNDBLOCK(spi, cmd9, sizeof(cmd9));
            memset(r, 0, sizeof(r));
            SPI_RECVBLOCK(spi, r, sizeof(r));
            syslog(LOG_ERR,
                   "[SdDiag] CMD9@%lu #%d: %02x %02x %02x %02x %02x %02x\n",
                   (unsigned long)freqs[f], t,
                   r[0], r[1], r[2], r[3], r[4], r[5]);
          }
      }
  }

  /* Byte-wise CMD9 at 10MHz, exactly like mmcsd_sendcmd does it
   * (SPI_SEND per byte).  The block-based sweep above works at all
   * frequencies; this isolates the single-byte poll path.
   */

  {
    int t;

    SPI_SETFREQUENCY(spi, 10000000);
    for (t = 0; t < 4; t++)
      {
        uint8_t resp = 0xff;

        SPI_SEND(spi, 0xff);
        SPI_SEND(spi, 0x49);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0xff);
        for (i = 0; i < 9; i++)
          {
            resp = SPI_SEND(spi, 0xff);
            if ((resp & 0x80) == 0)
              {
                break;
              }
          }

        syslog(LOG_ERR, "[SdDiag] cmd9-poll #%d: i=%d R1=%02x\n", t, i, resp);
      }
  }

  /* Exact mmcsd sequence: per-command select/deselect (toggles G35),
   * flush, waitready, 6 command bytes via SPI_SEND, response loop.
   */

  {
    int t;

    SPI_SETFREQUENCY(spi, 10000000);
    for (t = 0; t < 6; t++)
      {
        uint8_t resp = 0xff;
        uint8_t wr;

        esp32s3_spi3_select(spi, SPIDEV_MMCSD(0), true);
        SPI_SEND(spi, 0xff);
        wr = SPI_SEND(spi, 0xff);   /* waitready sample */
        SPI_SEND(spi, 0x49);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0xff);
        for (i = 0; i < 9; i++)
          {
            resp = SPI_SEND(spi, 0xff);
            if ((resp & 0x80) == 0)
              {
                break;
              }
          }

        /* drain the CSD data phase so the next command starts clean */

        if (resp == 0x00)
          {
            int k;
            uint8_t tok = 0xff;

            for (k = 0; k < 10; k++)
              {
                tok = SPI_SEND(spi, 0xff);
                if (tok == 0xfe)
                  {
                    break;
                  }
              }

            for (k = 0; k < 18; k++)
              {
                SPI_SEND(spi, 0xff);
              }
          }

        esp32s3_spi3_select(spi, SPIDEV_MMCSD(0), false);
        SPI_SEND(spi, 0xff);
        syslog(LOG_ERR, "[SdDiag] cmd9-seq #%d: wr=%02x i=%d R1=%02x\n",
               t, wr, i, resp);
        nxsig_usleep(1000);
      }
  }

  /* Test B: release the SPI lock between commands so the LCD runs its
   * normal 25Hz refresh in the gaps - replicates the mount-time failure.
   */

  {
    int t;

    SPI_LOCK(spi, false);
    for (t = 0; t < 6; t++)
      {
        uint8_t resp = 0xff;
        uint8_t wr;

        nxsig_usleep(50 * 1000);   /* let the LCD run */
        SPI_LOCK(spi, true);
        SPI_SETMODE(spi, SPIDEV_MODE0);
        SPI_SETBITS(spi, 8);
        SPI_SETFREQUENCY(spi, 10000000);
        esp32s3_spi3_select(spi, SPIDEV_MMCSD(0), true);
        SPI_SEND(spi, 0xff);
        wr = SPI_SEND(spi, 0xff);
        SPI_SEND(spi, 0x49);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0x00);
        SPI_SEND(spi, 0xff);
        for (i = 0; i < 9; i++)
          {
            resp = SPI_SEND(spi, 0xff);
            if ((resp & 0x80) == 0)
              {
                break;
              }
          }

        if (resp == 0x00)
          {
            int k;
            uint8_t tok = 0xff;

            for (k = 0; k < 10; k++)
              {
                tok = SPI_SEND(spi, 0xff);
                if (tok == 0xfe)
                  {
                    break;
                  }
              }

            for (k = 0; k < 18; k++)
              {
                SPI_SEND(spi, 0xff);
              }
          }

        esp32s3_spi3_select(spi, SPIDEV_MMCSD(0), false);
        SPI_SEND(spi, 0xff);
        syslog(LOG_ERR, "[SdDiag] cmd9-lcd #%d: wr=%02x i=%d R1=%02x\n",
               t, wr, i, resp);
        SPI_LOCK(spi, false);
      }

    return OK;
  }
}
