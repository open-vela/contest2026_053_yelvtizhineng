/****************************************************************************
 * apps/plant-companion/ui/screens/screen_ota.h
 *
 * OTA 升级页（阶段 A）：
 *   - 普通模式（screen_ota_create(false)）：首页「检查更新」进入。
 *     自动起 worker 查服务器 → 已最新 / 发现新版本（可立即升级）/
 *     失败（可重试）；升级时显示下载进度，完成后设备自动重启。
 *   - 升级后模式（screen_ota_create(true)）：OTA 重启后当前槽处于
 *     PENDING_VERIFY 时开机自动弹出——显示当前/上一版本 + 回退警告，
 *     手动「确认新版本」（标 VALID）或「稍后再说」（下次重启自动回退）。
 ****************************************************************************/

#ifndef __PLANT_UI_SCREENS_SCREEN_OTA_H
#define __PLANT_UI_SCREENS_SCREEN_OTA_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建 OTA 升级页（全屏二级页，ui_app_push_screen 推入）。
 * @param post_reboot true = 升级后确认页（PENDING_VERIFY 开机弹）；
 *                    false = 常规检查/升级页
 * @return 页面对象
 */
lv_obj_t *screen_ota_create(bool post_reboot);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREENS_SCREEN_OTA_H */
