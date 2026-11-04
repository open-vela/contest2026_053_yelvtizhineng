#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <nuttx/arch.h>

#include "soil_app.h"

#define SOIL_RESPONSE_SIZE 21
#define SOIL_RAW_BUFFER_SIZE 64
#define SOIL_READ_TIMEOUT_MS 650
#define SOIL_RS485_DIR_GPIO 5
#define SOIL_UART1_FIFO_REG 0x60010000UL
#define SOIL_UART1_DR_FIFO_REG 0x3ff50000UL
#define SOIL_UART1_STATUS_REG 0x3ff5001cUL
#define SOIL_UART1_INT_ENA_REG 0x3ff5000cUL
#define SOIL_UART1_INT_CLR_REG 0x3ff50010UL

#ifdef CONFIG_ARCH_CHIP_ESP32
#  include "esp32_gpio.h"
#  include "hardware/esp32_gpio_sigmap.h"
#endif

static unsigned int g_line_mode;

static uint16_t modbus_crc(const uint8_t *data, size_t length)
{
  uint16_t crc = 0xffff;
  size_t i;

  for (i = 0; i < length; i++)
    {
      int bit;
      crc ^= data[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xa001) :
                            (uint16_t)(crc >> 1);
        }
    }

  return crc;
}

static uint16_t read_u16(const uint8_t *data)
{
  return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static speed_t baud_constant(uint32_t baud)
{
  switch (baud)
    {
      case 4800:
        return B4800;
      case 19200:
        return B19200;
      case 38400:
        return B38400;
      case 9600:
      default:
        return B9600;
    }
}

#ifndef CONFIG_ARCH_CHIP_ESP32
static int write_all(int fd, const uint8_t *data, size_t length)
{
  size_t written = 0;

  while (written < length)
    {
      ssize_t ret = write(fd, data + written, length - written);
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (ret == 0)
        {
          return -EIO;
        }

      written += (size_t)ret;
    }

  return 0;
}
#endif

static int configure_uart(int fd, uint32_t baud)
{
  struct termios term;

  if (tcgetattr(fd, &term) < 0)
    {
      return -errno;
    }

  term.c_iflag = 0;
  term.c_oflag = 0;
  term.c_lflag = 0;
  term.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
  term.c_cflag |= CS8 | CLOCAL | CREAD;
  term.c_cc[VMIN] = 0;
  term.c_cc[VTIME] = 0;

  if (cfsetspeed(&term, baud_constant(baud)) < 0 ||
      tcsetattr(fd, TCSANOW, &term) < 0)
    {
      return -errno;
    }

  tcflush(fd, TCIOFLUSH);
  return 0;
}

void soil_sensor_set_line_mode(unsigned int mode)
{
  g_line_mode = mode;
}

static void configure_line_mode(void)
{
#ifdef CONFIG_ARCH_CHIP_ESP32
  bool swap = (g_line_mode & 1) != 0;
  bool invert = (g_line_mode & 2) != 0;
  bool auto_direction = (g_line_mode & 4) != 0;
  int rxpin = swap ? 18 : 19;
  int txpin = swap ? 19 : 18;

  if (swap || invert)
    {
      esp32_configgpio(txpin, OUTPUT_FUNCTION_3);
      esp32_gpio_matrix_out(txpin, U1TXD_OUT_IDX, invert, 0);
      esp32_configgpio(rxpin, INPUT_FUNCTION_3);
      esp32_gpio_matrix_in(rxpin, U1RXD_IN_IDX, invert);
    }

  if (auto_direction)
    {
      esp32_configgpio(SOIL_RS485_DIR_GPIO, INPUT_FUNCTION_3 | PULLUP);
    }
  else
    {
      esp32_configgpio(SOIL_RS485_DIR_GPIO, OUTPUT_FUNCTION_3);
      esp32_gpio_matrix_out(SOIL_RS485_DIR_GPIO, SIG_GPIO_OUT_IDX, 0, 0);
      esp32_gpiowrite(SOIL_RS485_DIR_GPIO, false);
    }
#endif
}

static bool parse_reading(const uint8_t *response, size_t length,
                          const struct soil_settings_s *settings,
                          struct soil_reading_s *reading)
{
  size_t start;

  for (start = 0; start + SOIL_RESPONSE_SIZE <= length; start++)
    {
      const uint8_t *frame = response + start;
      uint16_t expected;
      uint16_t received;

      if (frame[0] == 0x03 && frame[1] == 0x10 &&
          start + SOIL_RESPONSE_SIZE - 1 <= length)
        {
          uint8_t repaired[SOIL_RESPONSE_SIZE];

          repaired[0] = settings->modbus_addr;
          memcpy(&repaired[1], frame, SOIL_RESPONSE_SIZE - 1);
          return parse_reading(repaired, sizeof(repaired), settings, reading);
        }

      if (frame[0] != settings->modbus_addr || frame[1] != 0x03 ||
          frame[2] != 16)
        {
          continue;
        }

      expected = modbus_crc(frame, SOIL_RESPONSE_SIZE - 2);
      received = (uint16_t)(frame[SOIL_RESPONSE_SIZE - 2] |
                            ((uint16_t)frame[SOIL_RESPONSE_SIZE - 1] << 8));
      if (expected != received)
        {
          continue;
        }

      reading->temp_x10 = (int16_t)read_u16(&frame[3]);
      reading->moisture_x10 = read_u16(&frame[5]);
      reading->ec = read_u16(&frame[7]);
      reading->salt = read_u16(&frame[9]);
      reading->nitrogen = read_u16(&frame[11]);
      reading->phosphorus = read_u16(&frame[13]);
      reading->potassium = read_u16(&frame[15]);
      reading->ph_x10 = read_u16(&frame[17]);
      reading->temp_x10 = (int16_t)(reading->temp_x10 +
                                    settings->temp_offset_x10);
      reading->ph_x10 = (uint16_t)(reading->ph_x10 +
                                   settings->ph_offset_x10);
      return true;
    }

  soil_log("SOIL_FRAME_NOT_FOUND LEN=%u\n", (unsigned int)length);
  return false;
}

void soil_sensor_print_raw(const uint8_t *data, size_t length)
{
  static const char hex[] = "0123456789ABCDEF";
  char output[SOIL_RAW_BUFFER_SIZE * 2 + 1];
  size_t i;

  if (length > SOIL_RAW_BUFFER_SIZE)
    {
      length = SOIL_RAW_BUFFER_SIZE;
    }

  for (i = 0; i < length; i++)
    {
      output[i * 2] = hex[data[i] >> 4];
      output[i * 2 + 1] = hex[data[i] & 0x0f];
    }

  output[length * 2] = '\0';
  soil_log("SOIL_RAW=%s LEN=%u\n", output, (unsigned int)length);
}

int soil_sensor_read(const struct soil_settings_s *settings,
                     struct soil_reading_s *reading)
{
  uint8_t request[8] =
    {
      settings->modbus_addr, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00
    };
  uint8_t response[SOIL_RAW_BUFFER_SIZE];
  uint16_t crc;
  size_t length = 0;
  int fd;
  int ret;
  int elapsed = 0;
#ifdef CONFIG_ARCH_CHIP_ESP32
  uint32_t saved_int_ena = 0;
#endif

  crc = modbus_crc(request, 6);
  request[6] = (uint8_t)(crc & 0xff);
  request[7] = (uint8_t)(crc >> 8);

  fd = open(CONFIG_SOIL_NODE_UART_DEV, O_RDWR | O_NOCTTY);
  if (fd < 0)
    {
      return -errno;
    }

  ret = configure_uart(fd, settings->modbus_baud);
  if (ret < 0)
    {
      close(fd);
      return ret;
    }

  configure_line_mode();

#ifdef CONFIG_ARCH_CHIP_ESP32
  saved_int_ena = *(volatile uint32_t *)SOIL_UART1_INT_ENA_REG;
  *(volatile uint32_t *)SOIL_UART1_INT_ENA_REG = 0;

  if ((g_line_mode & 4) == 0)
    {
      esp32_gpiowrite(SOIL_RS485_DIR_GPIO, true);
      up_udelay(1000);
    }
#endif

#ifdef CONFIG_ARCH_CHIP_ESP32
  {
    uint32_t status = 0;
    size_t i;

    for (i = 0; i < sizeof(request); i++)
      {
        *(volatile uint32_t *)SOIL_UART1_FIFO_REG = request[i];
      }

    for (i = 0; i < 1000; i++)
      {
        status = *(volatile uint32_t *)SOIL_UART1_STATUS_REG;
        if ((status & 0x00ff0000UL) != 0 ||
            (status & 0x0f000000UL) != 0)
          {
            break;
          }

        usleep(100);
      }

    for (i = 0; i < 10000; i++)
      {
        status = *(volatile uint32_t *)SOIL_UART1_STATUS_REG;
        if ((status & 0x00ff0000UL) == 0 &&
            (status & 0x0f000000UL) == 0)
          {
            break;
          }

        usleep(100);
      }

    up_udelay(1000);

    if ((g_line_mode & 4) == 0)
      {
        esp32_gpiowrite(SOIL_RS485_DIR_GPIO, false);
      }

    usleep(500);
    ret = 0;
  }
#else
  ret = write_all(fd, request, sizeof(request));
  if (ret == 0)
    {
      tcdrain(fd);
      usleep(2000);
    }
#endif

#ifdef CONFIG_ARCH_CHIP_ESP32
  while (ret == 0 && elapsed < SOIL_READ_TIMEOUT_MS &&
         length < sizeof(response))
    {
      uint32_t status = *(volatile uint32_t *)SOIL_UART1_STATUS_REG;
      unsigned int available = status & 0xff;

      while (available-- > 0 && length < sizeof(response))
        {
          response[length++] =
            (uint8_t)*(volatile uint32_t *)SOIL_UART1_DR_FIFO_REG;
        }

      if (length >= SOIL_RESPONSE_SIZE + sizeof(request))
        {
          break;
        }

      usleep(1000);
      elapsed += 10;
    }
#else
  while (ret == 0 && elapsed < SOIL_READ_TIMEOUT_MS &&
         length < sizeof(response))
    {
      struct pollfd pfd;
      ssize_t got;

      pfd.fd = fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      ret = poll(&pfd, 1, 20);
      elapsed += 20;
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              ret = 0;
              continue;
            }

          ret = -errno;
          break;
        }

      if (ret == 0 || (pfd.revents & POLLIN) == 0)
        {
          ret = 0;
          if (length >= SOIL_RESPONSE_SIZE)
            {
              break;
            }

          continue;
        }

      got = read(fd, response + length, sizeof(response) - length);
      if (got < 0)
        {
          if (errno == EINTR || errno == EAGAIN)
            {
              ret = 0;
              continue;
            }

          ret = -errno;
          break;
        }

      length += (size_t)got;
      if (length >= SOIL_RESPONSE_SIZE)
        {
          usleep(15000);
          got = read(fd, response + length, sizeof(response) - length);
          if (got > 0)
            {
              length += (size_t)got;
            }

          break;
        }
    }
#endif

#ifdef CONFIG_ARCH_CHIP_ESP32
  *(volatile uint32_t *)SOIL_UART1_INT_CLR_REG = 0xffffffffUL;
  *(volatile uint32_t *)SOIL_UART1_INT_ENA_REG = saved_int_ena;
#endif

  close(fd);
  soil_sensor_print_raw(response, length);

  if (ret < 0)
    {
      return ret;
    }

  if (length < SOIL_RESPONSE_SIZE)
    {
      return -ETIMEDOUT;
    }

  if (!parse_reading(response, length, settings, reading))
    {
      return -EBADMSG;
    }

  return 0;
}

void soil_reading_print(const struct soil_reading_s *reading)
{
  soil_log("SOIL_OK moisture=%d.%u temp=%d.%u ec=%u ph=%u.%u "
           "salt=%u n=%u p=%u k=%u\n",
           reading->moisture_x10 / 10, reading->moisture_x10 % 10,
           reading->temp_x10 / 10,
           (unsigned int)(reading->temp_x10 < 0 ? -reading->temp_x10 % 10 :
                          reading->temp_x10 % 10),
           reading->ec, reading->ph_x10 / 10, reading->ph_x10 % 10,
           reading->salt, reading->nitrogen, reading->phosphorus,
           reading->potassium);
}

void soil_reading_json(const struct soil_reading_s *reading,
                       char *buffer, size_t buffer_size)
{
  snprintf(buffer, buffer_size,
           "{\"moisture\":%u.%u,\"temp\":%d.%u,\"ec\":%u,"
           "\"ph\":%u.%u,\"salt\":%u,\"nitrogen\":%u,"
           "\"phosphorus\":%u,\"potassium\":%u}",
           reading->moisture_x10 / 10, reading->moisture_x10 % 10,
           reading->temp_x10 / 10,
           (unsigned int)(reading->temp_x10 < 0 ? -reading->temp_x10 % 10 :
                          reading->temp_x10 % 10),
           reading->ec, reading->ph_x10 / 10, reading->ph_x10 % 10,
           reading->salt, reading->nitrogen, reading->phosphorus,
           reading->potassium);
}
