#pragma once

#include <nuttx/config.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOIL_SSID_MAX 32
#define SOIL_PASS_MAX 64
#define SOIL_MQTT_USER_MAX 64
#define SOIL_MQTT_PASS_MAX 96
#define SOIL_AP_SSID_MAX 32
#define SOIL_TOPIC_MAX 96

struct soil_settings_s
{
  char wifi_ssid[SOIL_SSID_MAX + 1];
  char wifi_password[SOIL_PASS_MAX + 1];
  uint8_t wifi_bssid[6];
  uint32_t wifi_channel;
  uint32_t interval_sec;
  char mqtt_user[SOIL_MQTT_USER_MAX + 1];
  char mqtt_password[SOIL_MQTT_PASS_MAX + 1];
  uint8_t modbus_addr;
  uint32_t modbus_baud;
  int16_t temp_offset_x10;
  int16_t ph_offset_x10;
  bool power_save;
  bool wifi_hint_valid;
  bool wifi_hint_changed;
};

struct soil_reading_s
{
  int16_t temp_x10;
  uint16_t moisture_x10;
  uint16_t ec;
  uint16_t ph_x10;
  uint16_t salt;
  uint16_t nitrogen;
  uint16_t phosphorus;
  uint16_t potassium;
};

struct soil_state_s
{
  uint32_t boot_count;
  uint32_t last_update;
  int last_rssi;
  bool last_sample_ok;
  bool last_publish_ok;
  bool has_reading;
  struct soil_reading_s last_reading;
};

void soil_defaults(struct soil_settings_s *settings);
int soil_settings_load(struct soil_settings_s *settings);
int soil_settings_save(const struct soil_settings_s *settings);
int soil_settings_clear(void);
int soil_settings_validate(const struct soil_settings_s *settings,
                           char *error, size_t error_size);

int soil_sensor_read(const struct soil_settings_s *settings,
                     struct soil_reading_s *reading);
void soil_sensor_print_raw(const uint8_t *data, size_t length);
void soil_sensor_set_line_mode(unsigned int mode);
void soil_reading_json(const struct soil_reading_s *reading,
                       char *buffer, size_t buffer_size);
void soil_reading_print(const struct soil_reading_s *reading);

void soil_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
