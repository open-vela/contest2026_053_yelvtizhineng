/****************************************************************************
 * apps/plant-companion/communication/wifi_manager/wifi_manager.h
 *
 * WiFi Manager — NuttX WiFi connection management.
 *
 * Uses NuttX wapi tool or ioctl to connect to WiFi networks.
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_WIFI_MANAGER_H
#define __APPS_PLANT_COMPANION_WIFI_MANAGER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef struct
{
  char ssid[33];      /* WiFi SSID (max 32 chars + null) */
  char password[65];  /* WiFi password (max 64 chars + null) */
  bool connected;     /* true if connected */
  int  rssi;          /* Signal strength (dBm) */
  char ip[16];        /* IP address string */
} wifi_status_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * wifi_manager_init - Initialize WiFi manager
 *
 * Return: 0 on success, negative errno on failure
 */

int wifi_manager_init(void);

/****************************************************************************
 * 扫描（2026-09-11：网络设置页用）
 ****************************************************************************/

#define WIFI_AP_MAX 24

typedef struct
{
  char ssid[33];      /* SSID（UTF-8，最多 32 字节） */
  int  rssi;          /* 信号强度 dBm（负值，越大越强） */
  bool secure;        /* true = 加密（连接需密码） */
} wifi_ap_t;

/**
 * wifi_manager_scan - 扫描附近 WiFi（阻塞，实测量级 5~8s）
 *
 * 内部跑 `wapi scan wlan0`，stdout 重定向到 /mnt/sd/wifi_scan.txt 后解析
 * （制表符分隔的 bssid/frequency/signal/encode/ssid 表）。同一 SSID 只留
 * 信号最强的一条，隐藏 SSID 丢弃，结果按信号从强到弱排序。
 *
 * @param aps 输出数组；@param max 数组容量
 * @return 写入条数（>=0）；负 errno 失败
 */

int wifi_manager_scan(wifi_ap_t *aps, int max);

/**
 * wifi_manager_has_ip - wlan0 是否真的拿到非 0 IPv4 地址
 *
 * wifi_manager_is_connected() 只看内部乐观标志（wapi 命令跑完即 true，
 * DHCP 失败也是 true）；判断"真的连上了"用本函数。
 *
 * @return true = 有 IP
 */

bool wifi_manager_has_ip(void);

/**
 * wifi_manager_link_up - wlan0 是否真的关联上了（IFF_RUNNING）
 *
 * @return true 已关联；false 断开 / 未关联
 */

bool wifi_manager_link_up(void);

/**
 * wifi_manager_get_essid - 读 wlan0 当前关联的 SSID
 *
 * @param buf 输出缓冲；@param buflen 缓冲长度
 * @return 0 成功；负 errno 失败（未关联/查询失败）
 */

int wifi_manager_get_essid(char *buf, size_t buflen);

/**
 * wifi_manager_connect - Connect to WiFi network
 *
 * ssid     - WiFi SSID
 * password - WiFi password
 *
 * Return: 0 on success, negative errno on failure
 */

int wifi_manager_connect(const char *ssid, const char *password);

/**
 * wifi_manager_reconnect_saved - 用最近一次连接的 SSID/密码重连一次
 *
 * 用途：网络"看着还连着、其实已经不通"时的自愈（由心跳连续失败触发）。
 * 没有任何可用凭据时返回 -ENOENT，不做任何动作。
 *
 * Return: 0 成功；负 errno 失败
 */
/**
 * wifi_manager_disconnect - Disconnect from WiFi network
 *
 * Return: 0 on success, negative errno on failure
 */

int wifi_manager_disconnect(void);

/**
 * wifi_manager_get_status - Get current WiFi status
 *
 * status - Pointer to wifi_status_t structure to receive status
 *
 * Return: 0 on success, negative errno on failure
 */

int wifi_manager_get_status(wifi_status_t *status);

/**
 * wifi_manager_is_connected - Check if WiFi is connected
 *
 * Return: true if connected, false otherwise
 */

bool wifi_manager_is_connected(void);

#endif /* __APPS_PLANT_COMPANION_WIFI_MANAGER_H */
