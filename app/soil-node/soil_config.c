#include <nuttx/config.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "soil_app.h"

#define SOIL_CONFIG_PATH "/data/soil.cfg"
#define SOIL_CONFIG_MAGIC "SOILCFG1"

static void copy_text(char *dest, size_t size, const char *src)
{
  if (size == 0)
    {
      return;
    }

  if (src == NULL)
    {
      dest[0] = '\0';
      return;
    }

  strncpy(dest, src, size - 1);
  dest[size - 1] = '\0';
}

static void trim_line(char *line)
{
  size_t length = strlen(line);

  while (length > 0 &&
         (line[length - 1] == '\n' || line[length - 1] == '\r'))
    {
      line[--length] = '\0';
    }
}

void soil_log(const char *format, ...)
{
  va_list ap;

  va_start(ap, format);
  vprintf(format, ap);
  va_end(ap);
  fflush(stdout);
}

void soil_defaults(struct soil_settings_s *settings)
{
  memset(settings, 0, sizeof(*settings));
  settings->interval_sec = 60;
  settings->modbus_addr = 2;
  settings->modbus_baud = 9600;
  settings->power_save = true;
}

int soil_settings_validate(const struct soil_settings_s *settings,
                           char *error, size_t error_size)
{
  if (settings->wifi_ssid[0] == '\0')
    {
      snprintf(error, error_size, "Wi-Fi name is required");
      return -EINVAL;
    }

  if (settings->interval_sec < 10 || settings->interval_sec > 3600)
    {
      snprintf(error, error_size, "interval must be 10..3600 seconds");
      return -EINVAL;
    }

  if (settings->modbus_addr < 1 || settings->modbus_addr > 247)
    {
      snprintf(error, error_size, "Modbus address must be 1..247");
      return -EINVAL;
    }

  if (settings->modbus_baud != 4800 && settings->modbus_baud != 9600 &&
      settings->modbus_baud != 19200 && settings->modbus_baud != 38400)
    {
      snprintf(error, error_size, "unsupported Modbus baud rate");
      return -EINVAL;
    }

  if (settings->temp_offset_x10 < -200 ||
      settings->temp_offset_x10 > 200)
    {
      snprintf(error, error_size, "temperature offset must be -20.0..20.0");
      return -EINVAL;
    }

  if (settings->ph_offset_x10 < -50 || settings->ph_offset_x10 > 50)
    {
      snprintf(error, error_size, "pH offset must be -5.0..5.0");
      return -EINVAL;
    }

  return 0;
}

int soil_settings_load(struct soil_settings_s *settings)
{
  FILE *file;
  char line[160];
  bool magic_ok = false;

  soil_defaults(settings);
  file = fopen(SOIL_CONFIG_PATH, "r");
  if (file == NULL)
    {
      return -errno;
    }

  while (fgets(line, sizeof(line), file) != NULL)
    {
      char *value;

      trim_line(line);
      value = strchr(line, '=');
      if (value == NULL)
        {
          continue;
        }

      *value++ = '\0';

      if (strcmp(line, "magic") == 0)
        {
          magic_ok = strcmp(value, SOIL_CONFIG_MAGIC) == 0;
        }
      else if (strcmp(line, "wifi_ssid") == 0)
        {
          copy_text(settings->wifi_ssid, sizeof(settings->wifi_ssid), value);
        }
      else if (strcmp(line, "wifi_password") == 0)
        {
          copy_text(settings->wifi_password,
                    sizeof(settings->wifi_password), value);
        }
      else if (strcmp(line, "wifi_channel") == 0)
        {
          settings->wifi_channel = strtoul(value, NULL, 10);
        }
      else if (strcmp(line, "wifi_bssid") == 0)
        {
          unsigned int a[6];
          if (sscanf(value, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2],
                     &a[3], &a[4], &a[5]) == 6)
            {
              size_t i;
              for (i = 0; i < 6; i++)
                {
                  settings->wifi_bssid[i] = (uint8_t)a[i];
                }

            }
        }
      else if (strcmp(line, "interval_sec") == 0)
        {
          settings->interval_sec = strtoul(value, NULL, 10);
        }
      else if (strcmp(line, "mqtt_user") == 0)
        {
          copy_text(settings->mqtt_user, sizeof(settings->mqtt_user), value);
        }
      else if (strcmp(line, "mqtt_password") == 0)
        {
          copy_text(settings->mqtt_password,
                    sizeof(settings->mqtt_password), value);
        }
      else if (strcmp(line, "modbus_addr") == 0)
        {
          settings->modbus_addr = (uint8_t)strtoul(value, NULL, 10);
        }
      else if (strcmp(line, "modbus_baud") == 0)
        {
          settings->modbus_baud = strtoul(value, NULL, 10);
        }
      else if (strcmp(line, "temp_offset_x10") == 0)
        {
          settings->temp_offset_x10 = (int16_t)strtol(value, NULL, 10);
        }
      else if (strcmp(line, "ph_offset_x10") == 0)
        {
          settings->ph_offset_x10 = (int16_t)strtol(value, NULL, 10);
        }
      else if (strcmp(line, "power_save") == 0)
        {
          settings->power_save = strtoul(value, NULL, 10) != 0;
        }
    }

  fclose(file);
  settings->wifi_hint_valid = settings->wifi_channel > 0 &&
                              settings->wifi_channel <= 14;
  if (settings->wifi_hint_valid)
    {
      size_t i;
      bool nonzero = false;

      for (i = 0; i < 6; i++)
        {
          if (settings->wifi_bssid[i] != 0)
            {
              nonzero = true;
              break;
            }
        }

      settings->wifi_hint_valid = nonzero &&
                                  (settings->wifi_bssid[0] & 1) == 0;
    }

  return magic_ok ? 0 : -EINVAL;
}

int soil_settings_save(const struct soil_settings_s *settings)
{
  FILE *file;
  char temp_path[] = SOIL_CONFIG_PATH ".tmp";

  file = fopen(temp_path, "w");
  if (file == NULL)
    {
      return -errno;
    }

  fprintf(file, "magic=%s\n", SOIL_CONFIG_MAGIC);
  fprintf(file, "wifi_ssid=%s\n", settings->wifi_ssid);
  fprintf(file, "wifi_password=%s\n", settings->wifi_password);
  fprintf(file, "wifi_channel=%lu\n",
          (unsigned long)settings->wifi_channel);
  fprintf(file, "wifi_bssid=%02X:%02X:%02X:%02X:%02X:%02X\n",
          settings->wifi_bssid[0], settings->wifi_bssid[1],
          settings->wifi_bssid[2], settings->wifi_bssid[3],
          settings->wifi_bssid[4], settings->wifi_bssid[5]);
  fprintf(file, "interval_sec=%lu\n",
          (unsigned long)settings->interval_sec);
  fprintf(file, "mqtt_user=%s\n", settings->mqtt_user);
  fprintf(file, "mqtt_password=%s\n", settings->mqtt_password);
  fprintf(file, "modbus_addr=%u\n", settings->modbus_addr);
  fprintf(file, "modbus_baud=%lu\n",
          (unsigned long)settings->modbus_baud);
  fprintf(file, "temp_offset_x10=%d\n", settings->temp_offset_x10);
  fprintf(file, "ph_offset_x10=%d\n", settings->ph_offset_x10);
  fprintf(file, "power_save=%u\n", settings->power_save ? 1 : 0);

  if (fflush(file) != 0 || fsync(fileno(file)) != 0)
    {
      int ret = -errno;
      fclose(file);
      unlink(temp_path);
      return ret;
    }

  fclose(file);
  if (rename(temp_path, SOIL_CONFIG_PATH) != 0)
    {
      int ret = -errno;
      unlink(temp_path);
      return ret;
    }

  return 0;
}

int soil_settings_clear(void)
{
  if (unlink(SOIL_CONFIG_PATH) != 0 && errno != ENOENT)
    {
      return -errno;
    }

  return 0;
}
