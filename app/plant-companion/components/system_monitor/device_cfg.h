/****************************************************************************
 * apps/plant-companion/components/system_monitor/device_cfg.h
 *
 * 开机配置持久化（2026-09-09）：
 *   最近一次成功的 WiFi + 服务器地址写入 SD 卡 plant.cfg，重启后由
 *   开机自动恢复线程读回重连。解决每次重启 WiFi/服务器地址丢失后
 *   拍一拍/语音请求 -107（未配置服务器）的反复出现。
 *
 * 文件格式（/mnt/sd/plant.cfg，每行 key=value，# 开头为注释）：
 *   wifi_ssid=H3C_1209
 *   wifi_password=YOUR_WIFI_PASSWORD
 *   server_host=192.168.3.12
 *   server_port=8011
 ****************************************************************************/

#ifndef __PLANT_DEVICE_CFG_H
#define __PLANT_DEVICE_CFG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>
#include <stddef.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* 把最近成功的 WiFi/服务器配置写入 SD（挂载失败/无卡时返回负 errno）。
 * ssid/password/host 可为 NULL 或空串（跳过对应键）。 */

int device_cfg_save(const char *ssid, const char *password,
                    const char *host, uint16_t port);

/* 读取 plant.cfg。各输出参数在文件缺失时被清零；返回 0 表示读到文件，
 * 负 errno 表示挂载/读取失败（常见 -ENOENT：SD 上还没有配置文件）。
 * 调用方凭 ssid[0]/host[0] 判断是否有对应配置。 */

int device_cfg_load(char *ssid, size_t ssid_size,
                    char *password, size_t password_size,
                    char *host, size_t host_size,
                    uint16_t *port);

/* 删除 plant.cfg（无文件时返回 0） */

int device_cfg_clear(void);

#endif /* __PLANT_DEVICE_CFG_H */
