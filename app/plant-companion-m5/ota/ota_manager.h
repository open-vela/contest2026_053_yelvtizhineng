/****************************************************************************
 * ota_manager.h — OTA 固件升级
 *
 * Phase 17: version check / download / verify / switch
 ****************************************************************************/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int ota_check_update(const char *url, char *new_version, size_t len);
int ota_download_and_apply(const char *url);

#ifdef __cplusplus
}
#endif
