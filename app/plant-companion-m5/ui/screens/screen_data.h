/****************************************************************************
 * apps/plant-companion/ui/screens/screen_data.h
 *
 * ④ 传感器数据 — 实时监测（2026-09-11 重写）
 *   2列×4行 八张真实参数卡：温度/水分/EC/盐分/氮/磷/钾/pH（无趋势图）
 *   没有有效读数时全部显示 "--"，不编造数值。
 ****************************************************************************/

#ifndef __PLANT_UI_SCREEN_DATA_H
#define __PLANT_UI_SCREEN_DATA_H

#include <lvgl/lvgl.h>
#include "../widgets/widget_bottomnav.h"
#include "../../services/sensor_service.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建数据页屏幕（含状态栏 + 内容 + 底部导航，自包含）
 * @param cb  Tab 切换回调（透传给底部导航）
 * @param user_data 透传数据
 * @return 屏幕对象（挂到 lv_screen_active()）
 */
lv_obj_t *screen_data_create(plant_nav_cb_t cb, void *user_data);

/** 刷新数据页（由 sensor_service 回调/UI 定时器驱动） */
void screen_data_refresh(const struct sensor_view_s *view,
                         const struct sensor_history_s *hist);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_DATA_H */
