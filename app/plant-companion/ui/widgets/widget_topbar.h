/****************************************************************************
 * apps/plant-companion/ui/widgets/widget_topbar.h
 *
 * 状态栏：时间 | 🌵 信号 | 电池 xx% | ☀️ 天气（UI_SPEC ①④⑤⑦）
 ****************************************************************************/

#ifndef __PLANT_UI_WIDGET_TOPBAR_H
#define __PLANT_UI_WIDGET_TOPBAR_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建状态栏（挂到屏幕对象上，置顶 40px）
 * @param parent 父对象（屏幕）
 * @return 状态栏容器
 */
lv_obj_t *widget_topbar_create(lv_obj_t *parent);

/** 更新电池百分比文本（如 "87%"） */
void widget_topbar_set_battery(lv_obj_t *topbar, int pct);

/** 更新时间文本（如 "9:41"） */
void widget_topbar_set_time(lv_obj_t *topbar, const char *time_str);

/** 更新天气文本（如 "26° 晴"），空则隐藏 */
void widget_topbar_set_weather(lv_obj_t *topbar, const char *weather);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_WIDGET_TOPBAR_H */
