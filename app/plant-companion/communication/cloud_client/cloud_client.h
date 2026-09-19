/****************************************************************************
 * cloud_client.h — MQTT + HTTP REST 云端通信
 *
 * Phase 17: MQTT sensor upload + HTTP image upload + REST API
 ****************************************************************************/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int cloud_client_init(void);
int cloud_upload_sensor_data(const char *json);
int cloud_upload_image(const uint8_t *jpeg, size_t len);
int cloud_request_advice(const char *json, char *response, size_t max_len);

#ifdef __cplusplus
}
#endif
