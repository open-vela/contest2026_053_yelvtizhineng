/****************************************************************************
 * apps/plant-companion/ui/screens/screen_home.h
 *
 * ① 主界面 — 植物状态总览（UI_SPEC ①）
 ****************************************************************************/

#ifndef __PLANT_UI_SCREEN_HOME_H
#define __PLANT_UI_SCREEN_HOME_H

#include <lvgl/lvgl.h>
#include "../widgets/widget_bottomnav.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 创建首页屏幕（含状态栏 + 内容 + 底部导航，自包含）
 * @param cb  Tab 切换回调（透传给底部导航）
 * @param user_data 透传数据
 * @return 屏幕对象（挂到 lv_screen_active()）
 */
lv_obj_t *screen_home_create(plant_nav_cb_t cb, void *user_data);

/**
 * 刷新首页数据（3B 阶段由 sensor_service/plant_state 调用）
 * @param health 健康分 0-100（进度环）
 * @param moisture 水分 %
 * @param temp 温度 °C
 * @param ph_x10 真实 pH×10（如 65 = pH 6.5，保一位小数）
 * @param ec EC 值（uS/cm 整数，与数据页同口径）
 * @param valid 最近一次读取是否成功；0 = 四个读数一律显示 "--"
 */
void screen_home_refresh(int health, int moisture, int temp, int ph_x10, int ec,
                         int valid);

/**
 * 刷新首页顶栏状态（时间 / 日期 / 网络 / 电量）
 * @param time_str "HH:MM"；NULL 或空 = 还没校到时 → 显示 "--:--"
 * @param date_str "M月D日"；NULL = 不显示
 * @param online 非 0 = WiFi 已连接 → "已联网"，否则 "未联网"
 * @param battery_pct 电量 0-100；<0 = 读不到 → 显示 "电池 --%"
 */
void screen_home_set_status(const char *time_str, const char *date_str,
                            int online, int battery_pct);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_SCREEN_HOME_H */
