/****************************************************************************
 * apps/plant-companion/ui/widgets/widget_bottomnav.h
 *
 * 底部 Tab 栏（V3 大字版）：高度 52px，图标 20px + 文字 12px，四页同色系
 *   首页(green) / 数据(blue) / 任务(amber) / 日记(green)
 *   语音页用 voice 变体：第 4 项显示「说话」并高亮粉色。
 ****************************************************************************/

#ifndef __PLANT_UI_WIDGET_BOTTOMNAV_H
#define __PLANT_UI_WIDGET_BOTTOMNAV_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PLANT_TAB_HOME     0
#define PLANT_TAB_DATA     1
#define PLANT_TAB_TASKS    2
#define PLANT_TAB_DIARY    3
#define PLANT_TAB_COUNT    4

/* 每页主题色（决定选中色 + Tab 栏淡底色） */

typedef enum
{
  PLANT_NAV_GREEN = 0,
  PLANT_NAV_BLUE,
  PLANT_NAV_AMBER,
  PLANT_NAV_PINK
} plant_nav_theme_t;

typedef void (*plant_nav_cb_t)(void *user_data, int tab_id);

/**
 * 创建底部 Tab 栏（挂到屏幕对象上，底部 TP_NAV_BAR_H）
 * @param parent   父对象（屏幕）
 * @param cb       Tab 切换回调（可为 NULL）
 * @param user_data 透传给回调
 * @param active   初始选中 Tab（0..3）
 * @param theme    主题色（选中高亮 + 淡底）
 * @param voice     true = 第 4 项显示「🎤 说话」（语音页）
 * @return 导航栏容器
 */
lv_obj_t *widget_bottomnav_create(lv_obj_t *parent, plant_nav_cb_t cb,
                                  void *user_data, int active,
                                  plant_nav_theme_t theme, bool voice);

/** 设置当前选中 Tab（高亮切换） */
void widget_bottomnav_set_active(lv_obj_t *nav, int tab_id);

/** 获取当前选中 Tab */
int widget_bottomnav_get_active(lv_obj_t *nav);

/** 调试：在给定页面上定位底部导航栏（几何特征：480xTP_NAV_BAR_H、底边贴屏底）。 */
lv_obj_t *widget_bottomnav_find(lv_obj_t *screen);

/** 调试：取某 Tab 按钮的屏幕绝对中心坐标（模拟点按用）。 */
bool widget_bottomnav_tab_center(lv_obj_t *nav, int tab_id, int *cx, int *cy);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_UI_WIDGET_BOTTOMNAV_H */
