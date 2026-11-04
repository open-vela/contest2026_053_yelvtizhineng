/****************************************************************************
 * apps/plant-companion/ui/screens/screen_wifi.h
 *
 * 网络设置页（2026-09-11）：
 *   - screen_wifi_create()：附近网络列表（后台扫描 + 点选）；
 *   - screen_wifi_pwd_create(ssid)：密码输入（lv_textarea + lv_keyboard）
 *     → 连接 → 成功写 SD 配置并自动退回首页。
 *
 * 两页都是全屏二级页，用 ui_app_push_screen() 推入：
 *   首页 → [网络] → 列表页 → 点某项 → 密码页 → 连接成功 → 自动回首页。
 ****************************************************************************/

#ifndef __PLANT_UI_SCREENS_SCREEN_WIFI_H
#define __PLANT_UI_SCREENS_SCREEN_WIFI_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建网络设置页（附近网络列表 + 重新扫描）。
 * @return 页面对象（ui_app_push_screen 推入）
 */

lv_obj_t *screen_wifi_create(void);

/**
 * 创建密码输入页（连接 ssid）。
 * @param ssid 目标 SSID（内部会拷贝一份，调用方无需保活）
 * @return 页面对象；ssid 为 NULL 时返回 NULL
 */

lv_obj_t *screen_wifi_pwd_create(const char *ssid);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREENS_SCREEN_WIFI_H */