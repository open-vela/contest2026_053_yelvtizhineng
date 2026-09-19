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

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_CAMERA_H */
