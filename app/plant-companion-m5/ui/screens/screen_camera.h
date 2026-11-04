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

/**
 * 若拍照页处于激活状态则正常弹出（停预览 + 关摄像头）。
 * 供 CLI 导航/切 Tab 时调用，避免预览线程继续在新页面上刷画面。
 */
void screen_camera_leave_if_active(void);
bool screen_camera_is_active(void);
/* UI thread only. */
bool screen_camera_is_visible(void);
int screen_camera_request_capture(void);
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
/* Thread-safe requests; timer changes are applied only by the UI thread. */
int screen_camera_request_frame_period(unsigned milliseconds);
unsigned screen_camera_frame_period_requested(void);
#endif
bool screen_camera_capture_busy(void); /* UI thread only. */
void screen_camera_set_lvgl_mode(bool on);
bool screen_camera_get_lvgl_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_CAMERA_H */
