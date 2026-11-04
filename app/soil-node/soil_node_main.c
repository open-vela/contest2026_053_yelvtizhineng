#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "soil_app.h"
#include "soil_cloud_config.h"
#include "soil_http.h"
#include "soil_mqtt.h"
#include "soil_power.h"
#include "soil_state.h"
#include "soil_time.h"
#include "soil_wifi.h"

static uint64_t soil_monotonic_us(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000000ULL +
         (uint64_t)now.tv_nsec / 1000ULL;
}

static void print_settings(const struct soil_settings_s *settings)
{
  soil_log("SETTINGS interval_s=%lu modbus_addr=%u modbus_baud=%lu "
           "temp_offset_x10=%d ph_offset_x10=%d power_save=%u\n",
           (unsigned long)settings->interval_sec,
           settings->modbus_addr,
           (unsigned long)settings->modbus_baud,
           settings->temp_offset_x10,
           settings->ph_offset_x10,
           settings->power_save ? 1 : 0);
}

static int sample_once(const struct soil_settings_s *settings)
{
  struct soil_reading_s reading;
  char json[256];
  int ret;

  memset(&reading, 0, sizeof(reading));
  memset(json, 0, sizeof(json));

  ret = soil_sensor_read(settings, &reading);
  if (ret < 0)
    {
      soil_log("SOIL_READ_FAILED ret=%d\n", ret);
      return ret;
    }

  soil_reading_print(&reading);
  soil_reading_json(&reading, json, sizeof(json));
  soil_log("SOIL_JSON=%s\n", json);
  return 0;
}

static void save_wifi_hint(struct soil_settings_s *settings)
{
  if (settings->wifi_hint_changed)
    {
      int ret = soil_settings_save(settings);
      soil_log("WIFI_HINT_SAVE=%s ret=%d\n",
               ret == 0 ? "OK" : "FAIL", ret);
      settings->wifi_hint_changed = false;
    }
}

static int sample_and_publish(const struct soil_settings_s *settings, int rssi)
{
  struct soil_reading_s reading;
  char json[256];
  int ret;

  memset(&reading, 0, sizeof(reading));
  memset(json, 0, sizeof(json));
  ret = soil_sensor_read(settings, &reading);
  if (ret < 0)
    {
      soil_log("SOIL_READ_FAILED ret=%d\n", ret);
      soil_state_record(NULL, false, false, rssi);
      return ret;
    }

  soil_reading_print(&reading);
  soil_reading_json(&reading, json, sizeof(json));
  soil_log("SOIL_JSON=%s\n", json);
  ret = soil_mqtt_publish_json(settings, json);
  soil_log("MQTT_PUBLISH=%s ret=%d TOPIC=%s\n",
           ret == 0 ? "OK" : "FAIL", ret, SOIL_MQTT_TOPIC);
  soil_state_record(&reading, true, ret == 0, rssi);
  return ret;
}

static int run_node(void)
{
  struct soil_settings_s settings;
  char ip[INET_ADDRSTRLEN];
  uint64_t cycle_started = soil_monotonic_us();
  int ret;

  ret = soil_settings_load(&settings);
  if (ret < 0)
    {
      soil_defaults(&settings);
    }

  if (ret < 0 || settings.wifi_ssid[0] == '\0')
    {
      /* A node may already have its cloud identity but no Wi-Fi settings.
       * Preserve those credentials when entering initial provisioning.
       */

      ret = soil_http_portal_run(&settings, 900000);
      if (ret <= 0)
        {
          soil_log("PROVISIONING_TIMEOUT_SLEEP_30MIN\n");
          if (soil_power_deep_sleep_us(30ULL * 60ULL * 1000000ULL) < 0)
            {
              sleep(30 * 60);
            }

          return 0;
        }
    }

  print_settings(&settings);
  soil_log("WIFI_CONNECTING SSID=%s\n", settings.wifi_ssid);
  ret = soil_wifi_station_connect(&settings, ip, sizeof(ip));
  if (ret < 0)
    {
      soil_log("WIFI_CONNECT_FAILED ret=%d\n", ret);
      ret = soil_http_portal_run(&settings, 900000);
      if (ret <= 0)
        {
          soil_log("PROVISIONING_TIMEOUT_SLEEP_30MIN\n");
          if (soil_power_deep_sleep_us(30ULL * 60ULL * 1000000ULL) < 0)
            {
              sleep(30 * 60);
            }

          return 0;
        }

      ret = soil_wifi_station_connect(&settings, ip, sizeof(ip));
      if (ret < 0)
        {
          return ret;
        }
    }

  soil_log("WIFI_CONNECTED IP=%s POWER_SAVE=%u\n", ip,
           settings.power_save ? 1 : 0);
  save_wifi_hint(&settings);

  ret = soil_time_sync(8000);
  if (ret < 0)
    {
      soil_log("TIME_SYNC=FAIL ret=%d\n", ret);
    }

  for (;;)
    {
      int rssi = 0;

      if (!soil_wifi_station_is_connected())
        {
          soil_log("WIFI_RECONNECTING\n");
          ret = soil_wifi_station_connect(&settings, ip, sizeof(ip));
          soil_log("WIFI_RECONNECT=%s ret=%d IP=%s\n",
                   ret == 0 ? "OK" : "FAIL", ret, ret == 0 ? ip : "");
          if (ret == 0)
            {
              save_wifi_hint(&settings);
            }
        }

      if (soil_wifi_station_is_connected())
        {
          if (soil_wifi_get_rssi(&rssi) == 0)
            {
              soil_log("WIFI_RSSI=%d\n", rssi);
            }
          else
            {
              soil_log("WIFI_RSSI_UNAVAILABLE\n");
            }

          sample_and_publish(&settings, rssi);
        }
      else
        {
          soil_state_record(NULL, false, false, 0);
        }

#ifdef CONFIG_SOIL_NODE_DEEP_SLEEP
      if (settings.power_save)
        {
          uint64_t interval_us = (uint64_t)settings.interval_sec * 1000000ULL;
          uint64_t active_us = soil_monotonic_us() - cycle_started;
          uint64_t duration_us = active_us < interval_us ?
                                 interval_us - active_us : 5000000ULL;

          soil_log("CYCLE_ACTIVE_MS=%llu\n",
                   (unsigned long long)(active_us / 1000ULL));
          if (soil_power_deep_sleep_us(duration_us) < 0)
            {
              soil_log("DEEP_SLEEP_FAILED falling back to sleep\n");
              sleep(settings.interval_sec);
            }
        }
      else
#endif
        {
          sleep(settings.interval_sec);
        }

      cycle_started = soil_monotonic_us();
    }

  return 0;
}

int main(int argc, char *argv[])
{
  struct soil_settings_s settings;
  int ret;

  soil_log("SOIL_MQTT_NODE_READY RS485_PORT=1 UART1 RX=19 TX=18 DE=5\n");
  soil_state_init();

  ret = soil_settings_load(&settings);
  if (ret < 0)
    {
      soil_log("SETTINGS_LOAD_FAILED ret=%d using defaults\n", ret);
      soil_defaults(&settings);
    }

  print_settings(&settings);

  if (argc > 1 && strcmp(argv[1], "defaults") == 0)
    {
      soil_defaults(&settings);
      print_settings(&settings);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "sample") == 0)
    {
      unsigned long value;

      if (argc > 2)
        {
          value = strtoul(argv[2], NULL, 10);
          if (value >= 1 && value <= 247)
            {
              settings.modbus_addr = (uint8_t)value;
            }
        }

      if (argc > 3)
        {
          value = strtoul(argv[3], NULL, 10);
          if (value == 4800 || value == 9600 || value == 19200 ||
              value == 38400)
            {
              settings.modbus_baud = (uint32_t)value;
            }
        }

      if (argc > 4)
        {
          soil_sensor_set_line_mode((unsigned int)strtoul(argv[4], NULL, 0));
          soil_log("RS485_LINE_MODE=%u\n",
                   (unsigned int)strtoul(argv[4], NULL, 0));
        }

      print_settings(&settings);
      return sample_once(&settings);
    }

  if (argc > 3 && strcmp(argv[1], "wifi") == 0)
    {
      char ip[INET_ADDRSTRLEN];

      snprintf(settings.wifi_ssid, sizeof(settings.wifi_ssid), "%s", argv[2]);
      snprintf(settings.wifi_password, sizeof(settings.wifi_password), "%s",
               argv[3]);
      soil_log("WIFI_CONNECTING SSID=%s\n", settings.wifi_ssid);
      ret = soil_wifi_station_connect(&settings, ip, sizeof(ip));
      soil_log("WIFI_CONNECT_RESULT=%s ret=%d IP=%s\n",
               ret == 0 ? "OK" : "FAIL", ret, ret == 0 ? ip : "");
      return ret;
    }

  if (argc > 3 && strcmp(argv[1], "configure") == 0)
    {
      char ip[INET_ADDRSTRLEN];
      unsigned long value;

      snprintf(settings.wifi_ssid, sizeof(settings.wifi_ssid), "%s", argv[2]);
      snprintf(settings.wifi_password, sizeof(settings.wifi_password), "%s",
               argv[3]);

      if (argc > 4)
        {
          value = strtoul(argv[4], NULL, 10);
          if (value >= 1 && value <= 247)
            {
              settings.modbus_addr = (uint8_t)value;
            }
        }

      if (argc > 5)
        {
          value = strtoul(argv[5], NULL, 10);
          if (value == 4800 || value == 9600 || value == 19200 ||
              value == 38400)
            {
              settings.modbus_baud = (uint32_t)value;
            }
        }

      if (argc > 6)
        {
          value = strtoul(argv[6], NULL, 10);
          if (value >= 10 && value <= 3600)
            {
              settings.interval_sec = (uint32_t)value;
            }
        }

      ret = soil_wifi_station_connect(&settings, ip, sizeof(ip));
      soil_log("WIFI_CONNECT_RESULT=%s ret=%d IP=%s\n",
               ret == 0 ? "OK" : "FAIL", ret, ret == 0 ? ip : "");
      if (ret < 0)
        {
          return ret;
        }

      ret = soil_mqtt_test_connection(&settings);
      soil_log("MQTT_TEST=%s ret=%d\n", ret == 0 ? "OK" : "FAIL", ret);
      if (ret < 0)
        {
          return ret;
        }

      ret = soil_settings_save(&settings);
      soil_log("SETTINGS_SAVE=%s ret=%d\n", ret == 0 ? "OK" : "FAIL", ret);
      return ret;
    }

  if (argc > 1 && strcmp(argv[1], "wifi-status") == 0)
    {
      soil_log("WIFI_CONNECTED=%u\n",
               soil_wifi_station_is_connected() ? 1 : 0);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "wifi-rssi") == 0)
    {
      int rssi = 0;

      ret = soil_wifi_get_rssi(&rssi);
      if (ret < 0)
        {
          soil_log("WIFI_RSSI_UNAVAILABLE ret=%d\n", ret);
          return ret;
        }

      soil_log("WIFI_RSSI=%d\n", rssi);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "ap") == 0)
    {
      char ssid[SOIL_AP_SSID_MAX + 1];

      ret = soil_wifi_make_ap_ssid(ssid, sizeof(ssid));
      if (ret < 0)
        {
          soil_log("AP_SSID_FAILED ret=%d\n", ret);
          return ret;
        }

      soil_log("PROVISIONING_AP SSID=%s URL=http://192.168.4.1\n", ssid);
      ret = soil_wifi_start_ap(ssid);
      soil_log("PROVISIONING_AP_RESULT=%s ret=%d\n",
               ret == 0 ? "OK" : "FAIL", ret);
      return ret;
    }

  if (argc > 1 && strcmp(argv[1], "scan") == 0)
    {
      char json[4096];

      ret = soil_wifi_scan_json(json, sizeof(json));
      soil_log("WIFI_SCAN_RESULT=%s ret=%d\n",
               ret == 0 ? "OK" : "FAIL", ret);
      if (ret == 0)
        {
          soil_log("WIFI_SCAN_JSON=%s\n", json);
        }

      return ret;
    }

  if (argc > 1 && strcmp(argv[1], "mqtt-test") == 0)
    {
      ret = soil_mqtt_test_connection(&settings);
      soil_log("MQTT_TEST=%s ret=%d\n", ret == 0 ? "OK" : "FAIL", ret);
      return ret;
    }

  if (argc > 1 && strcmp(argv[1], "mqtt-publish") == 0)
    {
      static const char payload[] =
        "{\"moisture\":24.3,\"temp\":21.4,\"ec\":44,\"ph\":6.7,"
        "\"salt\":22,\"nitrogen\":2,\"phosphorus\":3,\"potassium\":9}";

      ret = soil_mqtt_publish_json(&settings, payload);
      soil_log("MQTT_PUBLISH=%s ret=%d\n",
               ret == 0 ? "OK" : "FAIL", ret);
      return ret;
    }

  if (argc > 1 && strcmp(argv[1], "report-once") == 0)
    {
      char ip[INET_ADDRSTRLEN];

      if (!soil_wifi_station_is_connected())
        {
          ret = soil_wifi_station_connect(&settings, ip, sizeof(ip));
          soil_log("WIFI_CONNECT_RESULT=%s ret=%d IP=%s\n",
                   ret == 0 ? "OK" : "FAIL", ret, ret == 0 ? ip : "");
          if (ret < 0)
            {
              return ret;
            }
        }

      return sample_and_publish(&settings, 0);
    }

  if (argc > 1 && strcmp(argv[1], "portal") == 0)
    {
      ret = soil_http_portal_run(&settings, 900000);
      soil_log("PORTAL_RESULT=%d\n", ret);
      return ret;
    }

  if (argc > 2 && strcmp(argv[1], "sleep-test") == 0)
    {
      unsigned long seconds = strtoul(argv[2], NULL, 10);

      if (seconds < 2 || seconds > 3600)
        {
          return -EINVAL;
        }

      soil_log("DEEP_SLEEP_TEST seconds=%lu\n", seconds);
      ret = soil_power_deep_sleep_us((uint64_t)seconds * 1000000ULL);
      soil_log("DEEP_SLEEP_TEST_FAILED ret=%d\n", ret);
      return ret;
    }

  if (argc > 1 && strcmp(argv[1], "save-defaults-test") == 0)
    {
      ret = soil_settings_save(&settings);
      soil_log("SETTINGS_SAVE=%s ret=%d\n",
               ret == 0 ? "OK" : "FAIL", ret);
      return ret;
    }

  return run_node();
}
