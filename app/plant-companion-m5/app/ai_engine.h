/****************************************************************************
 * ai_engine.h — AI 养护建议调度
 *
 * Phase 18: device state → cloud AI → care advice dispatch
 ****************************************************************************/
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int ai_engine_diagnose(const char *sensor_json, const uint8_t *jpeg, size_t jpeg_len);
#ifdef __cplusplus
}
#endif
