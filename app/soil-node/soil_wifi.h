#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "soil_app.h"

int soil_wifi_station_connect(struct soil_settings_s *settings,
                              char *ip, size_t ip_size);
bool soil_wifi_station_is_connected(void);
int soil_wifi_get_rssi(int *rssi);
int soil_wifi_make_ap_ssid(char *buffer, size_t buffer_size);
int soil_wifi_start_ap(const char *ssid);
int soil_wifi_stop_ap(void);
int soil_wifi_scan_json(char *buffer, size_t buffer_size);
