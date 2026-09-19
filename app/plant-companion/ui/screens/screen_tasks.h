/****************************************************************************
 * apps/plant-companion/ui/screens/screen_tasks.h
 *
 * ⑤ 今日任务 — 养护打卡（UI_SPEC ⑤）
 *   日期 + 任务列表（✅/⬜ + 标题 + 时间窗）+ 点击完成
 ****************************************************************************/

#ifndef __PLANT_UI_SCREEN_TASKS_H
#define __PLANT_UI_SCREEN_TASKS_H

#include <lvgl/lvgl.h>
#include "../widgets/widget_bottomnav.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建任务页屏幕（含状态栏 + 内容 + 底部导航，自包含）
 * @param cb  Tab 切换回调（透传给底部导航）
 * @param user_data 透传数据
 * @return 屏幕对象（挂到 lv_screen_active()）
 */
lv_obj_t *screen_tasks_create(plant_nav_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_TASKS_H */
