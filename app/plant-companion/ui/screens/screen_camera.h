/****************************************************************************
 * apps/plant-companion/ui/screens/screen_camera.h
 *
 * ② AI拍照 — 植物识别（UI_SPEC ②）
 *   取景框 + 扫描线动画 + 「把植物放在框框里哦~」提示
 ****************************************************************************/

#ifndef __PLANT_UI_SCREEN_CAMERA_H
#define __PLANT_UI_SCREEN_CAMERA_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建拍照页（全屏二级页）
 * @return 屏幕对象
 */
lv_obj_t *screen_camera_create(void);
int screen_camera_voice_shoot(void);
int screen_camera_voice_busy(void);
/* UI-owner query: an underlying camera hidden by diagnosis is not a view. */
bool screen_camera_voice_visible(void);
bool screen_camera_power_active(void);
void screen_camera_voice_speech_pause(bool speaking);

/**
 * 若拍照页处于激活状态则正常弹出（停预览 + 关摄像头）。
 * 供 CLI 导航/切 Tab 时调用，避免预览线程继续在新页面上刷画面。
 */
void screen_camera_leave_if_active(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_CAMERA_H */
