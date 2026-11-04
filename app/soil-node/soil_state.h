#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "soil_app.h"

int soil_state_init(void);
void soil_state_record(const struct soil_reading_s *reading,
                       bool sample_ok, bool publish_ok, int rssi);
int soil_state_json(char *buffer, size_t buffer_size);
