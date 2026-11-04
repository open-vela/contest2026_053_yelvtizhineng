#pragma once

#include <stdbool.h>

#include "soil_app.h"

int soil_mqtt_publish_json(const struct soil_settings_s *settings,
                           const char *payload);
int soil_mqtt_test_connection(const struct soil_settings_s *settings);
