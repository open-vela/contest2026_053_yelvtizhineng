/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_serial.c
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

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#ifdef CONFIG_SERIAL_TERMIOS
#  include <termios.h>
#endif
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/spinlock.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/irq.h>
#include <nuttx/serial/serial.h>

#include "xtensa.h"
#include "esp32s3_config.h"
#include "esp32s3_irq.h"
#include "esp32s3_lowputc.h"
#include "esp32s3_gpio.h"
#include "hardware/esp32s3_uart.h"
#include "hardware/esp32s3_gpio.h"
#include "hardware/esp32s3_iomux.h"
#include "hardware/esp32s3_system.h"

#ifdef CONFIG_ESP32S3_USBSERIAL
#  include "esp32s3_usbserial.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The console is enabled, and it's not the syslog device, so, it should be a
 * serial device.
 */

#ifdef USE_SERIALDRIVER

/* Which UART with be tty0/console and which tty1? */

/* First pick the console and ttys0.
 * Console can be UART0 or UART1, but will always be ttys0.
 */

/* In case a UART was assigned to be the console and the corresponding
 * peripheral was also selected.
 */

#ifdef CONSOLE_UART
#  if defined(CONFIG_UART0_SERIAL_CONSOLE)
#    define CONSOLE_DEV         g_uart0_dev  /* UART0 is console */
#    define TTYS0_DEV           g_uart0_dev  /* UART0 is ttyS0 */
#    define UART0_ASSIGNED      1
#  elif defined(CONFIG_UART1_SERIAL_CONSOLE)
#    define CONSOLE_DEV         g_uart1_dev  /* UART1 is console */
#    define TTYS0_DEV           g_uart1_dev  /* UART1 is ttyS0 */
#    define UART1_ASSIGNED      1
#  elif defined(CONFIG_UART2_SERIAL_CONSOLE)
#    define CONSOLE_DEV         g_uart2_dev  /* UART2 is console */
#    define TTYS0_DEV           g_uart2_dev  /* UART2 is ttyS0 */
#    define UART2_ASSIGNED      1
#  endif /* CONFIG_UART0_SERIAL_CONSOLE */
#else /* No UART console */
#  undef  CONSOLE_DEV
#  if defined(CONFIG_ESP32S3_UART0)
#    define TTYS0_DEV           g_uart0_dev  /* UART0 is ttyS0 */
#    define UART0_ASSIGNED      1
#  elif defined(CONFIG_ESP32S3_UART1)
#    define TTYS0_DEV           g_uart1_dev  /* UART1 is ttyS0 */
#    define UART1_ASSIGNED      1
#  elif defined(CONFIG_ESP32S3_UART2)
#    define TTYS0_DEV           g_uart2_dev  /* UART2 is ttyS0 */
#    define UART2_ASSIGNED      1
#  endif
#endif /* CONSOLE_UART */

#ifdef CONFIG_ESP32S3_USBSERIAL
#  define CONSOLE_DEV           g_uart_usbserial
#  define TTYACM0_DEV           g_uart_usbserial
#endif

/* Pick ttyS1 */

#if defined(CONFIG_ESP32S3_UART0) && !defined(UART0_ASSIGNED)
#  define TTYS1_DEV           g_uart0_dev  /* UART0 is ttyS1 */
#  define UART0_ASSIGNED      1
#elif defined(CONFIG_ESP32S3_UART1) && !defined(UART1_ASSIGNED)
#  define TTYS1_DEV           g_uart1_dev  /* UART1 is ttyS1 */
#  define UART1_ASSIGNED      1
#elif defined(CONFIG_ESP32S3_UART2) && !defined(UART2_ASSIGNED)
#  define TTYS1_DEV           g_uart2_dev  /* UART2 is ttyS1 */
#  define UART2_ASSIGNED      1
#endif

/* Pick ttyS2 */

#if defined(CONFIG_ESP32S3_UART0) && !defined(UART0_ASSIGNED)
#  define TTYS2_DEV           g_uart0_dev  /* UART0 is ttyS2 */
#  define UART0_ASSIGNED      1
#elif defined(CONFIG_ESP32S3_UART1) && !defined(UART1_ASSIGNED)
#  define TTYS2_DEV           g_uart1_dev  /* UART1 is ttyS2 */
#  define UART1_ASSIGNED      1
#elif defined(CONFIG_ESP32S3_UART2) && !defined(UART2_ASSIGNED)
#  define TTYS2_DEV           g_uart2_dev  /* UART2 is ttyS2 */
#  define UART2_ASSIGNED      1
#endif

#ifdef HAVE_UART_DEVICE

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_UART

/* Serial driver methods */

static bool motor_txmirror_reserved(void);
static int  esp32s3_setup(struct uart_dev_s *dev);
static void esp32s3_shutdown(struct uart_dev_s *dev);
static int  esp32s3_attach(struct uart_dev_s *dev);
static void esp32s3_detach(struct uart_dev_s *dev);
static void esp32s3_txint(struct uart_dev_s *dev, bool enable);
static void esp32s3_rxint(struct uart_dev_s *dev, bool enable);
static bool esp32s3_rxavailable(struct uart_dev_s *dev);
static bool esp32s3_txready(struct uart_dev_s *dev);
static bool esp32s3_txempty(struct uart_dev_s *dev);
static void esp32s3_send(struct uart_dev_s *dev, int ch);
static int  esp32s3_receive(struct uart_dev_s *dev, unsigned int *status);
static int  esp32s3_ioctl(struct file *filep, int cmd, unsigned long arg);
#ifdef CONFIG_SERIAL_IFLOWCONTROL
static bool esp32s3_rxflowcontrol(struct uart_dev_s *dev,
                                  unsigned int nbuffered, bool upper);
#endif
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_UART

/* Operations */

static struct uart_ops_s g_uart_ops =
{
  .setup         = esp32s3_setup,
  .shutdown      = esp32s3_shutdown,
  .attach        = esp32s3_attach,
  .detach        = esp32s3_detach,
  .ioctl         = esp32s3_ioctl,
  .receive       = esp32s3_receive,
  .rxint         = esp32s3_rxint,
  .rxavailable   = esp32s3_rxavailable,
#ifdef CONFIG_SERIAL_IFLOWCONTROL
  .rxflowcontrol = esp32s3_rxflowcontrol,
#endif
  .send          = esp32s3_send,
  .txint         = esp32s3_txint,
  .txready       = esp32s3_txready,
  .txempty       = esp32s3_txempty
};

/* UART 0 */

#ifdef CONFIG_ESP32S3_UART0

static char g_uart0_rxbuffer[CONFIG_UART0_RXBUFSIZE];
static char g_uart0_txbuffer[CONFIG_UART0_TXBUFSIZE];

/* Fill only the requested fields */

static uart_dev_t g_uart0_dev =
{
#ifdef CONFIG_UART0_SERIAL_CONSOLE
  .isconsole = true,
#else
  .isconsole = false,
#endif
  .xmit =
  {
    .size   = CONFIG_UART0_TXBUFSIZE,
    .buffer = g_uart0_txbuffer,
  },
  .recv =
  {
    .size   = CONFIG_UART0_RXBUFSIZE,
    .buffer = g_uart0_rxbuffer,
  },

  .ops  = &g_uart_ops,
  .priv = &g_uart0_config
};

#endif

/* UART 1 */

#ifdef CONFIG_ESP32S3_UART1

static char g_uart1_rxbuffer[CONFIG_UART1_RXBUFSIZE];
static char g_uart1_txbuffer[CONFIG_UART1_TXBUFSIZE];

/* Fill only the requested fields */

static uart_dev_t g_uart1_dev =
{
#ifdef CONFIG_UART1_SERIAL_CONSOLE
  .isconsole = true,
#else
  .isconsole = false,
#endif
  .xmit =
  {
    .size   = CONFIG_UART1_TXBUFSIZE,
    .buffer = g_uart1_txbuffer,
  },
  .recv =
  {
    .size   = CONFIG_UART1_RXBUFSIZE,
    .buffer = g_uart1_rxbuffer,
  },

  .ops  = &g_uart_ops,
  .priv = &g_uart1_config
};

#endif

/* UART 2 */

#ifdef CONFIG_ESP32S3_UART2

static char g_uart2_rxbuffer[CONFIG_UART2_RXBUFSIZE];
static char g_uart2_txbuffer[CONFIG_UART2_TXBUFSIZE];

/* Fill only the requested fields */

static uart_dev_t g_uart2_dev =
{
#ifdef CONFIG_UART2_SERIAL_CONSOLE
  .isconsole = true,
#else
  .isconsole = false,
#endif
  .xmit =
  {
    .size   = CONFIG_UART2_TXBUFSIZE,
    .buffer = g_uart2_txbuffer,
  },
  .recv =
  {
    .size   = CONFIG_UART2_RXBUFSIZE,
    .buffer = g_uart2_rxbuffer,
  },

  .ops  = &g_uart_ops,
  .priv = &g_uart2_config
};

#endif

#endif /* CONFIG_ESP32S3_UART */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_UART

/****************************************************************************
 * Name: uart_interrupt
 *
 * Description:
 *   This is the UART interrupt handler.  It will be invoked when an
 *   interrupt is received on the 'irq'.  It should call uart_xmitchars or
 *   uart_recvchars to perform the appropriate data transfers.  The
 *   interrupt handling logic must be able to map the 'irq' number into the
 *   appropriate uart_dev_s structure in order to call these functions.
 *
 ****************************************************************************/

#include "esp32s3_motor_txwait.h"
#include "esp32s3_motor_rx_policy.h"

/* Protected by the UART upper-half recursive lock. UART1 only. */
static bool g_uart1_tx_feed_enabled;

#include "esp32s3_motor_halfduplex.h"
#include "esp32s3_motor_faultpins.h"

static int uart_handler(int irq, void *context, void *arg)
{
  struct uart_dev_s *dev = (struct uart_dev_s *)arg;
  struct esp32s3_uart_s *priv = dev->priv;
  uint32_t tx_mask = UART_TXFIFO_EMPTY_INT_ST_M | UART_TX_DONE_INT_ST_M;
  uint32_t rx_mask = UART_RXFIFO_TOUT_INT_ST_M | UART_RXFIFO_FULL_INT_ST_M;
  uint32_t int_status;

  int_status = getreg32(UART_INT_ST_REG(priv->id));

#ifdef HAVE_RS485
  if ((int_status & UART_TX_BRK_IDLE_DONE_INT_ST_M) != 0 &&
      esp32s3_txempty(dev))
    {
      if (dev->xmit.tail == dev->xmit.head)
        {
          esp32s3_gpiowrite(priv->rs485_dir_gpio,
                            !priv->rs485_dir_polarity);
        }
    }
#endif

  /* Tx fifo empty interrupt or UART tx done int */

  if ((int_status & tx_mask) != 0)
    {
      /* UART1's wire waiter needs the DONE edge. Clear only the sampled
       * causes BEFORE filling FIFO, so a new DONE cannot be erased after
       * uart_xmitchars. UART0/2 retain their established handler. */
      if(priv->id==1)
        {
          /* A TX cause sampled before disable can survive on another CPU.
           * Serialize its disposition with txint(false). Once that returns,
           * no old ISR may consume the upper-half queue while uart_write()
           * is still populating the complete packet. uart_xmitchars() and
           * its nested txint(false) use this same recursive UART lock. */
          irqstate_t txflags=uart_spinlock(dev,true);
          bool feed=g_uart1_tx_feed_enabled &&
                    (getreg32(UART_INT_ENA_REG(1))&int_status&tx_mask);
          putreg32(int_status&tx_mask,UART_INT_CLR_REG(1));
          if(feed)uart_xmitchars(dev);
          uart_spinunlock(dev,true,txflags);
        }
      else
        {
          uart_xmitchars(dev);
          modifyreg32(UART_INT_CLR_REG(priv->id),tx_mask,tx_mask);
        }
    }

  /* Rx fifo timeout interrupt or rx fifo full interrupt */

  if ((int_status & rx_mask) != 0)
    {
      uart_recvchars(dev);
      modifyreg32(UART_INT_CLR_REG(priv->id), rx_mask, rx_mask);
    }

  motor_txwire_notify(priv->id,int_status);
  return OK;
}

/****************************************************************************
 * Name: esp32s3_setup
 *
 * Description:
 *      Configure the UART baud, bits, parity, fifos, etc. This method is
 *      called the first time that the serial port is opened.
 *      For the serial console, this will occur very early in initialization,
 *      for other serial ports this will occur when the port is first opened.
 *      This setup does not include attaching or enabling interrupts.
 *      That portion of the UART setup is performed when the attach() method
 *      is called.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *
 * Returned Values:
 *   Zero (OK) is returned.
 *
 ****************************************************************************/

extern int esp32s3_uart1_park_tx(void);
extern void esp32s3_uart1_resume_pins(void);

static int esp32s3_setup(struct uart_dev_s *dev)
{
  struct esp32s3_uart_s *priv = dev->priv;

  if(priv->id==2 && motor_txmirror_reserved())return -EBUSY;
  if(priv->id==1)esp32s3_uart1_resume_pins();
  else if(priv->id==2)esp32s3_lowputc_config_pins(priv);
  int wire_init=motor_txwire_init(priv->id);if(wire_init<0)return wire_init;

  /* Initialize UART module */

  /* Keep corrupt RX frames rejected. The unmodified official StackChan
   * application was observed with UART1 RAM clock gating enabled. Match
   * that one bit only; preserve generic UART0/2 policy and all framing,
   * interrupt thresholds, deadlines and motor-fault protections. This
   * controlled comparison does not yet establish a motor repair.
   */

  uint32_t motor_mem_gate = priv->id == 1 ? UART_MEM_CLK_EN_M : 0;
  modifyreg32(UART_CONF0_REG(priv->id), UART_ERR_WR_MASK_M |
              UART_MEM_CLK_EN_M, (priv->id == 1 ? 0 : UART_ERR_WR_MASK_M) | motor_mem_gate);

  /* Define 0 as the threshold that means TX FIFO buffer is empty. */

  /* Official IDF UART uses96: queue the next fragment while FIFO still
   * has headroom. Threshold0 can starve a split frame under ISR latency.
   * UART0/2 keep their prior value; RX policy and physical deadlines stay. */
  modifyreg32(UART_CONF1_REG(priv->id), UART_TXFIFO_EMPTY_THRHD_M,
              priv->id==1 ? (96u<<UART_TXFIFO_EMPTY_THRHD_S) : 0);

  /* Define a threshold to trigger an RX FIFO FULL interrupt.
   * Define just one byte to read data immediately.
   */

  modifyreg32(UART_CONF1_REG(priv->id), UART_RXFIFO_FULL_THRHD_M,
              1 << UART_RXFIFO_FULL_THRHD_S);

  /* Define the maximum FIFO size for RX and TX FIFO.
   * That means, 1 block = 128 bytes.
   * As a consequence, software serial FIFO can unload the bytes and
   * not wait too much on polling activity.
   */

  modifyreg32(UART_MEM_CONF_REG(priv->id), UART_TX_SIZE_M | UART_RX_SIZE_M,
              (1 << UART_TX_SIZE_S) | (1 << UART_RX_SIZE_S));

  motor_uart1_rx_policy(priv->id);

  /* The official StackChan IDF driver explicitly selects UART_SCLK_DEFAULT
   * (=APB on S3). Do not inherit an earlier bootloader's UART1 clock source.
   * USB console and other UART controllers retain their current clocks. */
  if(priv->id==1)esp32s3_lowputc_set_sclk(priv, (enum uart_sclk)1);

  /* Configure the UART Baud Rate */

  esp32s3_lowputc_baud(priv);

  /* Set a mode */

  esp32s3_lowputc_normal_mode(priv);

  /* Parity */

  esp32s3_lowputc_parity(priv);

  /* Data Frame size */

  esp32s3_lowputc_data_length(priv);

  /* Stop bit */

  esp32s3_lowputc_stop_length(priv);

#ifdef CONFIG_SERIAL_IFLOWCONTROL
  /* Configure the input flow control */

  if (priv->iflow)
    {
      /* Enable input flow control and set the RX FIFO threshold
       * to assert the RTS line to half the RX FIFO buffer.
       * It will then save some space on the hardware fifo to
       * remaining bytes that may arrive after RTS be asserted
       * and before the transmitter stops sending data.
       */

      esp32s3_lowputc_set_iflow(priv, (uint8_t)(UART_RX_FIFO_SIZE / 2),
                                true);
    }
  else
    {
      /* Just disable input flow control, threshold parameter
       * will be discarded.
       */

      esp32s3_lowputc_set_iflow(priv, 0 , false);
    }

#endif
#ifdef CONFIG_SERIAL_OFLOWCONTROL
  /* Configure the ouput flow control */

  if (priv->oflow)
    {
      esp32s3_lowputc_set_oflow(priv, true);
    }
  else
    {
      esp32s3_lowputc_set_oflow(priv, false);
    }
#endif

#ifdef HAVE_RS485
  if (priv->rs485_dir_gpio != 0)
    {
      esp32s3_lowputc_set_tx_idle_time(priv, 1);
    }
  else
#endif
    {
      /* No Tx idle interval */

      esp32s3_lowputc_set_tx_idle_time(priv, 0);
    }

  /* Enable cores */

  esp32s3_lowputc_enable_sclk(priv);

  /* Clear FIFOs */

  esp32s3_lowputc_rst_txfifo(priv);
  esp32s3_lowputc_rst_rxfifo(priv);

  return OK;
}

/****************************************************************************
 * Name: esp32s3_shutdown
 *
 * Description:
 * Disable the UART.  This method is called when the serial port is closed.
 * This method reverses the operation the setup method.  NOTE that the serial
 * console is never shutdown.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *
 ****************************************************************************/

static void esp32s3_shutdown(struct uart_dev_s *dev)
{
  struct esp32s3_uart_s *priv = dev->priv;

  /* Disable ints */

  esp32s3_lowputc_disable_all_uart_int(priv, NULL);
  if(priv->id==1)esp32s3_uart1_park_tx();
}

/****************************************************************************
 * Name: esp32s3_attach
 *
 * Description:
 *   Configure the UART to operation in interrupt driven mode.  This method
 *   is called when the serial port is opened.  Normally, this is just after
 *   the the setup() method is called, however, the serial console may
 *   operate in a non-interrupt driven mode during the boot phase.
 *
 *   RX and TX interrupts are not enabled when by the attach method (unless
 *   the hardware supports multiple levels of interrupt enabling).  The RX
 *   and TX interrupts are not enabled until the txint() and rxint() methods
 *   are called.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *
 * Returned Values:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of any failure.
 *
 ****************************************************************************/

static int esp32s3_attach(struct uart_dev_s *dev)
{
  struct esp32s3_uart_s *priv = dev->priv;
  int ret;

  DEBUGASSERT(priv->cpuint == -ENOMEM);

  /* Set up to receive peripheral interrupts on the current CPU */

  priv->cpu = this_cpu();
  priv->cpuint = esp32s3_setup_irq(priv->cpu, priv->periph, priv->int_pri,
                                   ESP32S3_CPUINT_LEVEL);
  if (priv->cpuint < 0)
    {
      /* Failed to allocate a CPU interrupt of this type */

      return priv->cpuint;
    }

  /* Attach and enable the IRQ */

  ret = irq_attach(priv->irq, uart_handler, dev);
  if (ret == OK)
    {
      /* Enable the CPU interrupt (RX and TX interrupts are still disabled
       * in the UART
       */

      up_enable_irq(priv->irq);
    }

  return ret;
}

/****************************************************************************
 * Name: esp32s3_detach
 *
 * Description:
 *   Detach UART interrupts.  This method is called when the serial port is
 *   closed normally just before the shutdown method is called.  The
 *   exception is the serial console which is never shutdown.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *
 ****************************************************************************/

static void esp32s3_detach(struct uart_dev_s *dev)
{
  struct esp32s3_uart_s *priv = dev->priv;

  DEBUGASSERT(priv->cpuint != -ENOMEM);

  /* Disable and detach the CPU interrupt */

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);

  /* Disassociate the peripheral interrupt from the CPU interrupt */

  esp32s3_teardown_irq(priv->cpu, priv->periph, priv->cpuint);
  priv->cpuint = -ENOMEM;
}

/****************************************************************************
 * Name: esp32s3_txint
 *
 * Description:
 *    Enable or disable TX interrupts.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *   enable     -  If true enables the TX interrupt, if false disables it.
 *
 ****************************************************************************/

static void esp32s3_txint(struct uart_dev_s *dev, bool enable)
{
  struct esp32s3_uart_s *priv = dev->priv;
  uint32_t ints_mask = UART_TXFIFO_EMPTY_INT_ENA_M | UART_TX_DONE_INT_ENA_M;
  bool nopreempt=up_interrupt_context();
  irqstate_t txflags=0;
  if(priv->id==1)txflags=uart_spinlock(dev,nopreempt);
  irqstate_t flags = spin_lock_irqsave(&priv->lock);
  if(priv->id==1)g_uart1_tx_feed_enabled=enable;

  if (enable)
    {
      /* After all bytes physically transmitted in the RS485 bus
       * the TX_BRK_IDLE will indicate we can disable the TX pin.
       */

#ifdef HAVE_RS485
      if (priv->rs485_dir_gpio != 0)
        {
          modifyreg32(UART_INT_ENA_REG(priv->id),
                      0, UART_TX_BRK_IDLE_DONE_INT_ENA);
        }
#endif

      /* Set to receive an interrupt when the TX holding register register
       * is empty
       */

#ifndef CONFIG_SUPPRESS_SERIAL_INTS
      modifyreg32(UART_INT_ENA_REG(priv->id), ints_mask, ints_mask);
#endif
    }
  else
    {
      /* Disable the TX interrupt */

      modifyreg32(UART_INT_ENA_REG(priv->id), ints_mask, motor_txwire_keep(priv->id));
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  if(priv->id==1)uart_spinunlock(dev,nopreempt,txflags);
}

/****************************************************************************
 * Name: esp32s3_rxint
 *
 * Description:
 *   Enable or disable RX interrupts.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *   enable     -  If true enables the RX interrupt, if false disables it.
 *
 ****************************************************************************/

/* Explicit rail-OFF diagnostic selection; default remains interrupt RX. */
static bool g_uart1_rxpoll;
static unsigned g_uart1_rxpoll_remaining;
static uint32_t g_uart1_rxpoll_calls,g_uart1_rxpoll_bytes;

static void esp32s3_rxint(struct uart_dev_s *dev, bool enable)
{
  struct esp32s3_uart_s *priv = dev->priv;
  uint32_t ints_mask = UART_RXFIFO_TOUT_INT_ENA_M |
                       UART_RXFIFO_FULL_INT_ENA_M;
  irqstate_t flags = spin_lock_irqsave(&priv->lock);

  if (priv->id == 1 && g_uart1_rxpoll) enable = false;

  if (enable)
    {
      /* Receive an interrupt when there is anything in the RX data register
       * (or an RX timeout occurs).
       * NOTE: RX timeout feature needs to be enabled.
       */
#ifndef CONFIG_SUPPRESS_SERIAL_INTS
      modifyreg32(UART_CONF1_REG(priv->id), UART_RX_TOUT_EN_M,
                  UART_RX_TOUT_EN_M);
      modifyreg32(UART_INT_ENA_REG(priv->id), ints_mask, ints_mask);
#endif
    }
  else
    {
      modifyreg32(UART_CONF1_REG(priv->id), UART_RX_TOUT_EN_M, 0);

      /* Disable the RX interrupts */

      modifyreg32(UART_INT_ENA_REG(priv->id), ints_mask, 0);
    }

  spin_unlock_irqrestore(&priv->lock, flags);
}

/****************************************************************************
 * Name: esp32s3_rxavailable
 *
 * Description:
 *   Check if there is any data available to be read.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *
 * Returned Values:
 *   Return true if the RX FIFO is not empty and false if RX FIFO is empty.
 *
 ****************************************************************************/

static bool esp32s3_rxavailable(struct uart_dev_s *dev)
{
  struct esp32s3_uart_s *priv = dev->priv;
  uint32_t status_reg;
  uint32_t bytes;

  if(priv->id==1 && g_uart1_rxpoll && !g_uart1_rxpoll_remaining)return false;

  status_reg = getreg32(UART_STATUS_REG(priv->id));
  bytes = REG_MASK(status_reg, UART_RXFIFO_CNT);

  return bytes > 0;
}

/****************************************************************************
 * Name: esp32s3_txready
 *
 * Description:
 *    Check if the transmit hardware is ready to send another byte.
 *    This is used to determine if send() method can be called.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *
 * Returned Values:
 *   Return true if the transmit hardware is ready to send another byte,
 *   false otherwise.
 *
 ****************************************************************************/

static bool esp32s3_txready(struct uart_dev_s *dev)
{
  return !esp32s3_lowputc_is_tx_fifo_full(dev->priv);
}

/****************************************************************************
 * Name: esp32s3_txempty
 *
 * Description:
 *    Verify if all characters have been sent. If for example, the UART
 *    hardware implements FIFOs, then this would mean the transmit FIFO is
 *    empty. This method is called when the driver needs to make sure that
 *    all characters are "drained" from the TX hardware.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *
 * Returned Values:
 *   Return true if the TX FIFO is empty, false if it is not.
 *
 ****************************************************************************/

static bool esp32s3_txempty(struct uart_dev_s *dev)
{
  uint32_t reg;
  struct esp32s3_uart_s *priv = dev->priv;

  reg = getreg32(UART_INT_RAW_REG(priv->id));
  reg = REG_MASK(reg, UART_TX_DONE_INT_RAW);

  return reg > 0;
}

/****************************************************************************
 * Name: esp32s3_send
 *
 * Description:
 *    Send a unique character
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *   ch         -  Byte to be sent.
 *
 ****************************************************************************/

static void esp32s3_send(struct uart_dev_s *dev, int ch)
{
  struct esp32s3_uart_s *priv = dev->priv;

#ifdef HAVE_RS485
  if (priv->rs485_dir_gpio != 0)
    {
      esp32s3_gpiowrite(priv->rs485_dir_gpio, priv->rs485_dir_polarity);
    }
#endif

  esp32s3_lowputc_send_byte(priv, (char)ch);
}

/****************************************************************************
 * Name: esp32s3_receive
 *
 * Description:
 *   Called (usually) from the interrupt level to receive one
 *   character from the UART.  Error bits associated with the
 *   receipt are provided in the return 'status'.
 *
 * Parameters:
 *   dev        -  Pointer to the serial driver struct.
 *   status     -  Pointer to a variable to store eventual error bits.
 *
 * Returned Values:
 *   Return the byte read from the RX FIFO.
 *
 ****************************************************************************/

/* Explicit UART1 diagnostic API. No pin, baud or power reconfiguration.
 * Snapshot reads do not consume FIFO data or acknowledge interrupts.
 */
#ifdef CONFIG_ESP32S3_UART1
/* Read-only queue/IRQ observation for the explicit body feedback test.
 * No interrupt acknowledge, FIFO access, transmission or pin changes.
 */
int esp32s3_uart1_diag_queue(uint32_t *v,unsigned count)
{
  if(!v || count!=10)return -EINVAL;
  irqstate_t flags=spin_lock_irqsave(&g_uart1_config.lock);
  v[0]=g_uart1_dev.xmit.head;v[1]=g_uart1_dev.xmit.tail;
  v[2]=g_uart1_dev.recv.head;v[3]=g_uart1_dev.recv.tail;
  v[4]=getreg32(UART_CONF1_REG(1));v[5]=getreg32(UART_INT_ENA_REG(1));
  v[6]=getreg32(UART_INT_ST_REG(1));v[7]=getreg32(UART_INT_RAW_REG(1));
  v[8]=getreg32(UART_STATUS_REG(1));v[9]=getreg32(UART_FSM_STATUS_REG(1));
  spin_unlock_irqrestore(&g_uart1_config.lock,flags);return 0;
}

int esp32s3_uart1_diag_snapshot(uint32_t *v, unsigned count)
{
  if (!v || count != 14) return -EINVAL;
  const unsigned tx=CONFIG_ESP32S3_UART1_TXPIN;
  const unsigned rx=CONFIG_ESP32S3_UART1_RXPIN;
  irqstate_t flags=spin_lock_irqsave(&g_uart1_config.lock);
  v[0]=getreg32(GPIO_FUNC0_OUT_SEL_CFG_REG+4*tx);
  v[1]=getreg32(GPIO_FUNC0_IN_SEL_CFG_REG+4*g_uart1_config.rxsig);
  v[2]=getreg32(REG_IO_MUX_BASE+4*(tx+1));
  v[3]=getreg32(REG_IO_MUX_BASE+4*(rx+1));
  uint32_t enables=getreg32(GPIO_ENABLE_REG),levels=getreg32(GPIO_IN_REG);
  v[4]=((enables>>tx)&1)|(((enables>>rx)&1)<<1);
  v[5]=((levels>>tx)&1)|(((levels>>rx)&1)<<1);
  v[6]=getreg32(UART_CONF0_REG(1));
  v[7]=getreg32(UART_CLK_CONF_REG(1));
  v[8]=getreg32(UART_CLKDIV_REG(1));
  v[9]=getreg32(UART_STATUS_REG(1));
  v[10]=getreg32(UART_INT_RAW_REG(1));
  v[11]=getreg32(UART_ID_REG(1));
  v[12]=getreg32(UART_FSM_STATUS_REG(1));
  v[13]=esp32s3_lowputc_get_sclk(&g_uart1_config);
  spin_unlock_irqrestore(&g_uart1_config.lock,flags);
  return 0;
}

/* Sample the external TX pad by enabling only its input buffer for 2us.
 * Preserve its mux, drive, pull and output configuration exactly. CoreS3's
 * guarded UP configuration and UART1 ownership serialize this short probe.
 * GPIO_IN without the input buffer cannot establish the TX pad level.
 */
int esp32s3_uart1_diag_pad_sample(uint32_t *levels)
{
  if(!levels)return -EINVAL;
  const unsigned tx=CONFIG_ESP32S3_UART1_TXPIN;
  const unsigned rx=CONFIG_ESP32S3_UART1_RXPIN;
  const uint32_t address=REG_IO_MUX_BASE+4*(tx+1);
  irqstate_t flags=spin_lock_irqsave(&g_uart1_config.lock);
  uint32_t saved=getreg32(address);
  putreg32(saved|FUN_IE,address);
  bool input_enabled=(getreg32(address)&FUN_IE)!=0;
  up_udelay(2);
  uint32_t input=getreg32(GPIO_IN_REG);
  putreg32(saved,address);
  bool restored=getreg32(address)==saved;
  spin_unlock_irqrestore(&g_uart1_config.lock,flags);
  if(!input_enabled || !restored)return -EIO;
  *levels=((input>>tx)&1)|(((input>>rx)&1)<<1);
  return 0;
}

/* Explicit rail-OFF caller only. Observe the external TX pad during a
 * fixed header-free pattern, with UART internal loopback OFF. This is not a
 * servo instruction. Only TX input-enable is borrowed and restored exactly.
 * No UART pin/clock/baud/mux/loopback change or interrupt acknowledgement.
 * The bounded critical section is 256 one-microsecond samples plus MMIO.
 */
int esp32s3_uart1_diag_pad_wave(uint32_t *v,unsigned count)
{
  static const uint8_t pattern[8]={0,255,0,255,0,255,0,255};
  if(!v || count!=8)return -EINVAL;
  memset(v,0,8*sizeof(*v));
  const unsigned tx=CONFIG_ESP32S3_UART1_TXPIN;
  const uint32_t address=REG_IO_MUX_BASE+4*(tx+1);
  irqstate_t flags=spin_lock_irqsave(&g_uart1_config.lock);
  int rc=0;
  if(g_uart1_config.baud!=1000000 ||
     !(getreg32(UART_ID_REG(1))&UART_HIGH_SPEED_M))
    {rc=-ENOTSUP;goto unlock;}
  if((getreg32(UART_CONF0_REG(1))&UART_LOOPBACK_M) ||
     g_uart1_dev.xmit.head!=g_uart1_dev.xmit.tail ||
     (getreg32(UART_STATUS_REG(1))&UART_TXFIFO_CNT_M) ||
     (getreg32(UART_FSM_STATUS_REG(1))&UART_ST_UTX_OUT_M))
    {rc=-EBUSY;goto unlock;}
  uint32_t saved=getreg32(address);
  putreg32(saved|FUN_IE,address);
  if(!(getreg32(address)&FUN_IE)){rc=-EIO;goto restore;}
  up_udelay(2);
  v[3]=(getreg32(GPIO_IN_REG)>>tx)&1;
  for(unsigned i=0;i<sizeof(pattern);i++)putreg32(pattern[i],UART_FIFO_REG(1));
  unsigned previous=v[3];
  for(unsigned i=0;i<256;i++)
    {
      unsigned level=(getreg32(GPIO_IN_REG)>>tx)&1;
      if(level)v[0]++;else v[1]++;
      if(level!=previous)v[2]++;
      previous=level;up_udelay(1);
    }
  v[4]=previous;
  v[5]=(getreg32(UART_STATUS_REG(1))&UART_TXFIFO_CNT_M)>>UART_TXFIFO_CNT_S;
  v[6]=(getreg32(UART_FSM_STATUS_REG(1))&UART_ST_UTX_OUT_M)>>UART_ST_UTX_OUT_S;
  if(v[5] || v[6])rc=-ETIMEDOUT;
restore:
  putreg32(saved,address);v[7]=getreg32(address)==saved;
  if(!v[7])rc=-EIO;
unlock:
  spin_unlock_irqrestore(&g_uart1_config.lock,flags);return rc;
}

/* Explicit diagnostic only. Fixed SCSCL READ(position) packet, IDs1/2.
 * No WRITE/torque/target parameter. Observe RX pad independently of UART
 * decoding while its FIFO holds the short reply. IRQ masking is bounded to
 * 2048 one-us samples plus MMIO; caller guarantees rail cleanup/ownership.
 */
int esp32s3_uart1_diag_receive(uint8_t id,uint32_t *v,unsigned count)
{
  if(!v || count!=14 || (id!=1 && id!=2))return -EINVAL;
  memset(v,0,14*sizeof(*v));
  uint8_t packet[8]={255,255,id,4,2,56,2,0};
  packet[7]=(uint8_t)~(id+4+2+56+2);
  const unsigned tx=CONFIG_ESP32S3_UART1_TXPIN,rx=CONFIG_ESP32S3_UART1_RXPIN;
  const uint32_t address=REG_IO_MUX_BASE+4*(tx+1);
  irqstate_t flags=spin_lock_irqsave(&g_uart1_config.lock);int rc=0;
  if(g_uart1_config.baud!=1000000 || !(getreg32(UART_ID_REG(1))&UART_HIGH_SPEED_M))
    {rc=-ENOTSUP;goto unlock;}
  if((getreg32(UART_CONF0_REG(1))&UART_LOOPBACK_M) ||
     g_uart1_dev.xmit.head!=g_uart1_dev.xmit.tail ||
     (getreg32(UART_STATUS_REG(1))&(UART_TXFIFO_CNT_M|UART_RXFIFO_CNT_M)) ||
     (getreg32(UART_FSM_STATUS_REG(1))&UART_ST_UTX_OUT_M))
    {rc=-EBUSY;goto unlock;}
  if(!(getreg32(REG_IO_MUX_BASE+4*(rx+1))&FUN_IE)){rc=-ENOTSUP;goto unlock;}
  uint32_t saved=getreg32(address);putreg32(saved|FUN_IE,address);
  if(!(getreg32(address)&FUN_IE)){rc=-EIO;goto restore;}
  up_udelay(2);uint32_t pins=getreg32(GPIO_IN_REG);
  v[6]=(pins>>tx)&1;v[7]=(pins>>rx)&1;unsigned previous_tx=v[6],previous_rx=v[7];
  for(unsigned i=0;i<8;i++)putreg32(packet[i],UART_FIFO_REG(1));
  for(unsigned i=0;i<2048;i++)
    {
      pins=getreg32(GPIO_IN_REG);unsigned a=(pins>>tx)&1,b=(pins>>rx)&1;
      if(a)v[0]++;else v[1]++;if(a!=previous_tx)v[2]++;
      if(b)v[3]++;else v[4]++;if(b!=previous_rx)v[5]++;
      previous_tx=a;previous_rx=b;up_udelay(1);
    }
  v[8]=previous_tx;v[9]=previous_rx;
  uint32_t status=getreg32(UART_STATUS_REG(1));
  v[10]=(status&UART_TXFIFO_CNT_M)>>UART_TXFIFO_CNT_S;
  v[11]=(status&UART_RXFIFO_CNT_M)>>UART_RXFIFO_CNT_S;
  v[12]=getreg32(UART_INT_RAW_REG(1))&(UART_FRM_ERR_INT_RAW_M|UART_PARITY_ERR_INT_RAW_M|UART_RXFIFO_OVF_INT_RAW_M);
  if(v[10])rc=-ETIMEDOUT;
restore:
  putreg32(saved,address);v[13]=getreg32(address)==saved;if(!v[13])rc=-EIO;
unlock:
  spin_unlock_irqrestore(&g_uart1_config.lock,flags);return rc;
}

int esp32s3_uart1_diag_loopback(bool enabled)
{
  irqstate_t flags=spin_lock_irqsave(&g_uart1_config.lock);
  /* Do not silently manipulate a different asynchronous clock mode.
   * Expose it in the snapshot and leave it for a separate diagnosis.
   */
  if (!(getreg32(UART_ID_REG(1))&UART_HIGH_SPEED_M))
    {spin_unlock_irqrestore(&g_uart1_config.lock,flags);return -ENOTSUP;}
  uint32_t before=getreg32(UART_CONF0_REG(1));
  modifyreg32(UART_CONF0_REG(1),UART_LOOPBACK_M,enabled?UART_LOOPBACK_M:0);
  bool observed=(getreg32(UART_CONF0_REG(1))&UART_LOOPBACK_M)!=0;
  spin_unlock_irqrestore(&g_uart1_config.lock,flags);
  if(observed!=enabled)return -EIO;
  return (before&UART_LOOPBACK_M)!=0;
}
#endif

/* Per-transaction error observation: clear only sticky RX error latches.
 * Does not reset RX/TX state, clear FIFO bytes or change interrupt enables.
 * The normal worker owns UART1. Existing RX deadlines remain unchanged. */
uint32_t esp32s3_uart1_rx_error_begin(void)
{
  const uint32_t mask=UART_PARITY_ERR_INT_RAW_M | UART_FRM_ERR_INT_RAW_M |
                      UART_RXFIFO_OVF_INT_RAW_M | UART_BRK_DET_INT_RAW_M |
                      UART_GLITCH_DET_INT_RAW_M;
  uint32_t prior=getreg32(UART_INT_RAW_REG(1))&mask;
  putreg32(mask,UART_INT_CLR_REG(1));
  return prior;
}

#include "esp32s3_motor_wire_counter.h"
#include "esp32s3_motor_txmirror.h"
#include "esp32s3_motor_timing.h"

/* UART1 RX service timestamps. These conservatively bound byte arrival:
 * interrupt latency may reject an on-time byte, never make a late byte early.
 * Only UART1 pays the clock/lock cost; console and soil UART are unchanged.
 * Coherent snapshots work even if SMP is enabled in a future configuration.
 */
static spinlock_t g_uart1_rx_stamp_lock = SP_UNLOCKED;
static uint32_t g_uart1_rx_ms;
static uint64_t g_uart1_rx_us;
static uint32_t g_uart1_rx_sequence;

void esp32s3_uart1_rx_snapshot(uint32_t *received_ms, uint32_t *sequence)
{
  irqstate_t flags = spin_lock_irqsave(&g_uart1_rx_stamp_lock);
  *received_ms = g_uart1_rx_ms;
  *sequence = g_uart1_rx_sequence;
  spin_unlock_irqrestore(&g_uart1_rx_stamp_lock, flags);
}

void esp32s3_uart1_rx_usec_snapshot(uint64_t *received)
{
 irqstate_t flags=spin_lock_irqsave(&g_uart1_rx_stamp_lock);
 *received=g_uart1_rx_us;
 spin_unlock_irqrestore(&g_uart1_rx_stamp_lock,flags);
}

static int esp32s3_receive(struct uart_dev_s *dev, unsigned int *status)
{
  uint32_t rx_fifo;
  struct esp32s3_uart_s *priv = dev->priv;

  rx_fifo = REG_MASK(getreg32(UART_FIFO_REG(priv->id)), UART_RXFIFO_RD_BYTE);
  if(priv->id==1 && g_uart1_rxpoll && g_uart1_rxpoll_remaining)g_uart1_rxpoll_remaining--;

  if (priv->id == 1)
    {
      irqstate_t flags = spin_lock_irqsave(&g_uart1_rx_stamp_lock);
      g_uart1_rx_ms = (uint32_t)TICK2MSEC(clock_systime_ticks());
      g_uart1_rx_us=esp32s3_rt_timer_time_us();
      g_uart1_rx_sequence++;
      spin_unlock_irqrestore(&g_uart1_rx_stamp_lock, flags);
    }


  /* Since we don't have error bits associated with receipt, we set zero */

  *status = 0;

  return (int)rx_fifo;
}


/* Diagnostic differential: same FIFO decoder and software ring, serviced by
 * the owning task instead of the UART RX interrupt. No TX/baud/pin/reset
 * writes, retries or deadline change. Caller selects only with motor rail OFF.
 * Each service is bounded by the ring free space, not a receive wait loop. */
int esp32s3_uart1_rxpoll_select(bool enabled)
{
  irqstate_t flags=spin_lock_irqsave(&g_uart1_config.lock);
  uint32_t status=getreg32(UART_STATUS_REG(1));
  if(g_uart1_dev.xmit.head!=g_uart1_dev.xmit.tail ||
     g_uart1_dev.recv.head!=g_uart1_dev.recv.tail ||
     (status&(UART_TXFIFO_CNT_M|UART_RXFIFO_CNT_M)) ||
     (getreg32(UART_FSM_STATUS_REG(1))&(UART_ST_UTX_OUT_M|UART_ST_URX_OUT_M)))
    {spin_unlock_irqrestore(&g_uart1_config.lock,flags);return -EBUSY;}
  g_uart1_rxpoll=enabled;
  bool opened=g_uart1_dev.open_count!=0;
  spin_unlock_irqrestore(&g_uart1_config.lock,flags);
  if(opened)esp32s3_rxint(&g_uart1_dev,!enabled);
  return 0;
}
int esp32s3_uart1_rxpoll_service(void)
{
  if(!g_uart1_rxpoll)return 0;
  irqstate_t flags=spin_lock_irqsave(&g_uart1_config.lock);
  if(g_uart1_dev.open_count!=1)
    {spin_unlock_irqrestore(&g_uart1_config.lock,flags);return -ENODEV;}
  uint32_t errors=getreg32(UART_INT_RAW_REG(1))&
    (UART_PARITY_ERR_INT_RAW_M|UART_FRM_ERR_INT_RAW_M|UART_RXFIFO_OVF_INT_RAW_M);
  if(errors){spin_unlock_irqrestore(&g_uart1_config.lock,flags);return -EIO;}
  uint32_t before=g_uart1_rx_sequence;
  unsigned available=REG_MASK(getreg32(UART_STATUS_REG(1)),UART_RXFIFO_CNT);
  g_uart1_rxpoll_remaining=available<64?available:64;
  uart_recvchars(&g_uart1_dev);
  g_uart1_rxpoll_remaining=0;
  g_uart1_rxpoll_calls++;g_uart1_rxpoll_bytes+=g_uart1_rx_sequence-before;
  putreg32(UART_RXFIFO_TOUT_INT_CLR_M|UART_RXFIFO_FULL_INT_CLR_M,UART_INT_CLR_REG(1));
  spin_unlock_irqrestore(&g_uart1_config.lock,flags);
  return 1; /* Task must pump without a blocking poll while RX IRQ is masked. */
}
int esp32s3_uart1_rxpoll_metrics(uint32_t v[3])
{
  if(!v)return -EINVAL;
  irqstate_t flags=spin_lock_irqsave(&g_uart1_config.lock);
  v[0]=g_uart1_rxpoll;v[1]=g_uart1_rxpoll_calls;v[2]=g_uart1_rxpoll_bytes;
  spin_unlock_irqrestore(&g_uart1_config.lock,flags);return 0;
}

/****************************************************************************
 * Name: esp32s3_ioctl
 *
 * Description:
 *   All ioctl calls will be routed through this method.
 *   Here it's employed to implement the TERMIOS ioctls and TIOCSERGSTRUCT.
 *
 * Parameters:
 *   filep    Pointer to a file structure instance.
 *   cmd      The ioctl command.
 *   arg      The argument of the ioctl cmd.
 *
 * Returned Value:
 *   Returns a non-negative number on success;  A negated errno value is
 *   returned on any failure (see comments ioctl() for a list of appropriate
 *   errno values).
 *
 ****************************************************************************/

static int esp32s3_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  /* Get access to the internal instance of the driver through the file
   *  pointer.
   */

#if defined(CONFIG_SERIAL_TERMIOS) || defined(CONFIG_SERIAL_TIOCSERGSTRUCT)
  struct inode      *inode = filep->f_inode;
  struct uart_dev_s *dev   = inode->i_private;
#endif
  int ret = OK;

  /* Run the requested ioctl command. */

  switch (cmd)
    {
#ifdef CONFIG_SERIAL_TIOCSERGSTRUCT

    /* Get the internal driver data structure for debug purposes. */

    case TIOCSERGSTRUCT:
      {
         struct esp32s3_uart_s *user = (struct esp32s3_uart_s *)arg;
         if (user == NULL)
           {
             ret = -EINVAL;
           }
         else
           {
             memcpy(user, dev->priv, sizeof(struct esp32s3_uart_s));
           }
       }
       break;
#endif

#ifdef CONFIG_SERIAL_TERMIOS

    /* Fill a termios structure with the required information. */

    case TCGETS:
      {
        struct termios  *termiosp   = (struct termios *)arg;
        struct esp32s3_uart_s *priv = (struct esp32s3_uart_s *)dev->priv;
        if (termiosp == NULL)
          {
            ret = -EINVAL;
            break;
          }

        /* Return parity (0 = no parity, 1 = odd parity, 2 = even parity). */

        termiosp->c_cflag = (priv->parity != 0 ? PARENB : 0) |
                            (priv->parity == 1 ? PARODD : 0);

        /* Return stop bits */

        termiosp->c_cflag |= priv->stop_b2 != 0 ? CSTOPB : 0;

#ifdef CONFIG_SERIAL_OFLOWCONTROL
        termiosp->c_cflag |=  priv->oflow != 0 ? CCTS_OFLOW : 0;
#endif
#ifdef CONFIG_SERIAL_IFLOWCONTROL
        termiosp->c_cflag |=  priv->iflow != 0 ? CRTS_IFLOW : 0;
#endif

        /* Set the baud rate in ther termiosp using the
         * cfsetispeed interface.
         */

        cfsetispeed(termiosp, priv->baud);

        /* Return number of bits. */

        switch (priv->bits)
          {
          case 5:
            termiosp->c_cflag |= CS5;
            break;

          case 6:
            termiosp->c_cflag |= CS6;
            break;

          case 7:
            termiosp->c_cflag |= CS7;
            break;

          default:
          case 8:
            termiosp->c_cflag |= CS8;
            break;
          }
      }
      break;

    case TCSETS:
      {
        struct termios  *termiosp   = (struct termios *)arg;
        struct esp32s3_uart_s *priv = (struct esp32s3_uart_s *)dev->priv;
        uint32_t baud;
        uint32_t current_int_sts;
        uint8_t  parity;
        uint8_t  bits;
        uint8_t  stop2;
#ifdef CONFIG_SERIAL_IFLOWCONTROL
        bool iflow;
#endif
#ifdef CONFIG_SERIAL_OFLOWCONTROL
        bool oflow;
#endif

        if (termiosp == NULL)
          {
            ret = -EINVAL;
            break;
          }

        /* Get the target baud rate to change. */

        baud = cfgetispeed(termiosp);

        /* Decode number of bits. */

        switch (termiosp->c_cflag & CSIZE)
          {
          case CS5:
            bits = 5;
            break;

          case CS6:
            bits = 6;
            break;

          case CS7:
            bits = 7;
            break;

          case CS8:
            bits = 8;
            break;

          default:
            ret = -EINVAL;
            break;
          }

        /* Decode parity. */

        if ((termiosp->c_cflag & PARENB) != 0)
          {
            parity = (termiosp->c_cflag & PARODD) != 0 ? 1 : 2;
          }
        else
          {
            parity = 0;
          }

        /* Decode stop bits. */

        stop2 = (termiosp->c_cflag & CSTOPB) != 0 ? 1 : 0;

#ifdef CONFIG_SERIAL_IFLOWCONTROL
        iflow = (termiosp->c_cflag & CRTS_IFLOW) != 0;
#endif
#ifdef CONFIG_SERIAL_OFLOWCONTROL
        oflow = (termiosp->c_cflag & CCTS_OFLOW) != 0;
#endif

        /* Verify that all settings are valid before
         * performing the changes.
         */

        if (ret == OK)
          {
            /* Fill the private struct fields. */

            priv->baud      = baud;
            priv->parity    = parity;
            priv->bits      = bits;
            priv->stop_b2   = stop2;
#ifdef CONFIG_SERIAL_IFLOWCONTROL
            priv->iflow     = iflow;
#endif
#ifdef CONFIG_SERIAL_OFLOWCONTROL
            priv->oflow     = oflow;
#endif

            /* Effect the changes immediately - note that we do not
             * implement TCSADRAIN or TCSAFLUSH, only TCSANOW option.
             * See nuttx/libs/libc/termios/lib_tcsetattr.c
             */

            esp32s3_lowputc_disable_all_uart_int(priv, &current_int_sts);
            ret = esp32s3_setup(dev);

            /* Restore the interrupt state */

            esp32s3_lowputc_restore_all_uart_int(priv, &current_int_sts);
          }
      }
      break;
#endif /* CONFIG_SERIAL_TERMIOS */

    default:
      ret = -ENOTTY;
      break;
    }

  return ret;
}

/****************************************************************************
 * Name: esp32s3_rxflowcontrol
 *
 * Description:
 *   Called when upper half RX buffer is full (or exceeds configured
 *   watermark levels if CONFIG_SERIAL_IFLOWCONTROL_WATERMARKS is defined).
 *   Return true if UART activated RX flow control to block more incoming
 *   data.
 *   NOTE: ESP32-S3 has a hardware RX FIFO threshold mechanism to control
 *   RTS line and to stop receiving data. This is very similar to the concept
 *   behind upper watermark level. The hardware threshold is used here
 *   to control the RTS line. When setting the threshold to zero, RTS will
 *   immediately be asserted. If nbuffered = 0 or the lower watermark is
 *   crossed and the serial driver decides to disable RX flow control, the
 *   threshold will be changed to UART_RX_FLOW_THRHD_VALUE, which is almost
 *   half the HW RX FIFO capacity. It keeps some space to keep the data
 *   received between the RTS assertion and the stop by the sender.
 *
 * Input Parameters:
 *   dev       - UART device instance
 *   nbuffered - the number of characters currently buffered
 *               (if CONFIG_SERIAL_IFLOWCONTROL_WATERMARKS is
 *               not defined the value will be 0 for an empty buffer or the
 *               defined buffer size for a full buffer)
 *   upper     - true indicates the upper watermark was crossed where
 *               false indicates the lower watermark has been crossed
 *
 * Returned Value:
 *   true if RX flow control activated.
 *
 ****************************************************************************/

#ifdef CONFIG_SERIAL_IFLOWCONTROL
static bool esp32s3_rxflowcontrol(struct uart_dev_s *dev,
                                  unsigned int nbuffered, bool upper)
{
  bool ret = false;
  struct esp32s3_uart_s *priv = dev->priv;
  if (priv->iflow)
    {
      if (nbuffered == 0 || upper == false)
        {
          /* Empty buffer, RTS should be de-asserted and logic in above
           * layers should re-enable RX interrupt.
           */

          esp32s3_lowputc_set_iflow(priv, (uint8_t)(UART_RX_FIFO_SIZE / 2),
                                    true);
          esp32s3_rxint(dev, true);
          ret = false;
        }
      else
        {
          /* If the RX buffer is not zero and watermarks are not enabled,
           * then this function is called to announce RX buffer is full.
           * The first thing it should do is to immediately assert RTS.
           * Software RX FIFO is full, so besides asserting RTS, it's
           * necessary to disable RX interrupts to prevent remaining bytes
           * (that arrive after asserting RTS) to be pushed to the
           * SW RX FIFO.
           */

          esp32s3_lowputc_set_iflow(priv, 0 , true);
          esp32s3_rxint(dev, false);
          ret = true;
        }
    }

  return ret;
}
#endif
#endif /* CONFIG_ESP32S3_UART */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef USE_EARLYSERIALINIT

/****************************************************************************
 * Name: xtensa_earlyserialinit
 *
 * Description:
 *   Performs the low level UART initialization early in debug so that the
 *   serial console will be available during bootup.  This must be called
 *   before xtensa_serialinit.  NOTE:  This function depends on GPIO pin
 *   configuration performed in xtensa_consoleinit() and main clock
 *   initialization performed in up_clkinitialize().
 *
 ****************************************************************************/

void xtensa_earlyserialinit(void)
{
  /* NOTE:  All GPIO configuration for the UARTs was performed in
   * esp32s3_lowsetup
   */

  /* Disable all UARTS interrupts */

#ifdef TTYS0_DEV
  esp32s3_lowputc_disable_all_uart_int(TTYS0_DEV.priv, NULL);
#endif

#ifdef TTYS1_DEV
  esp32s3_lowputc_disable_all_uart_int(TTYS1_DEV.priv, NULL);
#endif

#ifdef TTYS2_DEV
  esp32s3_lowputc_disable_all_uart_int(TTYS2_DEV.priv, NULL);
#endif

  /* Configure console in early step.
   * Setup for other serials will be perfomed when the serial driver is
   * open.
   */

#ifdef CONSOLE_UART
  esp32s3_setup(&CONSOLE_DEV);
#endif
}

#endif /* USE_EARLYSERIALINIT */

/****************************************************************************
 * Name: xtensa_serialinit
 *
 * Description:
 *   Register serial console and serial ports.  This assumes
 *   that xtensa_earlyserialinit was called previously.
 *
 ****************************************************************************/

void xtensa_serialinit(void)
{
#ifdef CONSOLE_DEV
  uart_register("/dev/console", &CONSOLE_DEV);
#endif

#ifdef TTYS0_DEV
  uart_register("/dev/ttyS0", &TTYS0_DEV);
#endif

#ifdef TTYS1_DEV
  uart_register("/dev/ttyS1", &TTYS1_DEV);
#endif

#ifdef TTYS2_DEV
  uart_register("/dev/ttyS2", &TTYS2_DEV);
#endif

#ifdef CONFIG_ESP32S3_USBSERIAL
  uart_register("/dev/ttyACM0", &TTYACM0_DEV);
#endif
}

/****************************************************************************
 * Name: up_putc
 *
 * Description:
 *   Provide priority, low-level access to support OS debug  writes
 *
 ****************************************************************************/

void up_putc(int ch)
{
#ifdef CONSOLE_UART
  uint32_t int_status;

  esp32s3_lowputc_disable_all_uart_int(CONSOLE_DEV.priv, &int_status);
#endif

  xtensa_lowputc((char)ch);

#ifdef CONSOLE_UART
  esp32s3_lowputc_restore_all_uart_int(CONSOLE_DEV.priv, &int_status);
#endif
}

#else

void xtensa_earlyserialinit(void)
{
}

void xtensa_serialinit(void)
{
}

void up_putc(int ch)
{
}

#endif /* HAVE_UART_DEVICE */

#else /* USE_SERIALDRIVER */

/****************************************************************************
 * Name: up_putc
 *
 * Description:
 *   Provide priority, low-level access to support OS debug writes
 *
 ****************************************************************************/

void up_putc(int ch)
{
#ifdef CONSOLE_UART
  uint32_t int_status;

  esp32s3_lowputc_disable_all_uart_int(CONSOLE_DEV.priv, &int_status);
#endif

  xtensa_lowputc(ch);

#ifdef CONSOLE_UART
  esp32s3_lowputc_restore_all_uart_int(CONSOLE_DEV.priv, &int_status);
#endif
}

#endif /* USE_SERIALDRIVER */
