/****************************************************************************
 * apps/plant-companion/ui/screens/screen_diary.h
 *
 * ⑦ 成长日记 — 时光记录（UI_SPEC ⑦）
 *   时间线：颜色圆点 + 时间 + 标题 + 详情
 ****************************************************************************/

#ifndef __PLANT_UI_SCREEN_DIARY_H
#define __PLANT_UI_SCREEN_DIARY_H

#include <lvgl/lvgl.h>
#include "../widgets/widget_bottomnav.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建日记页屏幕（含状态栏 + 内容 + 底部导航，自包含）
 * @param cb  Tab 切换回调（透传给底部导航）
 * @param user_data 透传数据
 * @return 屏幕对象（挂到 lv_screen_active()）
 */
lv_obj_t *screen_diary_create(plant_nav_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_DIARY_H */
