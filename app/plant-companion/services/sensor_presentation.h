#ifndef PLANT_SENSOR_PRESENTATION_H
#define PLANT_SENSOR_PRESENTATION_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sensor_service.h"
enum sp_metric { SP_WATER, SP_TEMP, SP_EC, SP_SALT, SP_PH, SP_N, SP_P, SP_K, SP_COUNT };
enum sp_freshness { SP_NONE, SP_FRESH, SP_STALE, SP_TIME_UNKNOWN };
#define SP_ALL_FIELDS ((1u << SP_COUNT) - 1)
struct sensor_presentation_s {
  float value[SP_COUNT];
  uint16_t fields;
  enum sp_freshness freshness;
  uint32_t version, age_s, expires_ms;
  uint8_t source;
  bool moisture_rule, dry;
  char measured_at[24], source_id[40];
};
void sensor_present(const struct sensor_view_s *view, uint32_t mono_ms,
                    int64_t unix_s, struct sensor_presentation_s *out);
uint16_t sensor_valid_fields(const float values[SP_COUNT]);
const char *sensor_metric_name(int metric);
const char *sensor_metric_unit(int metric);
const char *sensor_metric_explanation(int metric);
const char *sensor_source_name(int source);
const char *sensor_freshness_name(const struct sensor_presentation_s *view);
void sensor_metric_format(const struct sensor_presentation_s *view, int metric,
                          char *buffer, size_t size);
void sensor_time_note(const struct sensor_presentation_s *view, char *buffer, size_t size);
void sensor_spoken_summary(const struct sensor_presentation_s *view, char *buffer, size_t size);
/* UI-only controlled inputs. Never affect raw cache, telemetry or actuators. */
int sensor_present_fixture_set(int mode);
int sensor_present_fixture_get(struct sensor_view_s *out);
#endif
