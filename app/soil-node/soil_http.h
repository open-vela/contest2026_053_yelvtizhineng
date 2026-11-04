#pragma once

#include <stdint.h>

#include "soil_app.h"

int soil_http_portal_run(struct soil_settings_s *settings,
                         uint32_t timeout_ms);
