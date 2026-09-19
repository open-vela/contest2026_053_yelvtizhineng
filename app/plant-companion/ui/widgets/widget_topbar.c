/****************************************************************************
 * apps/plant-companion/ui/widgets/widget_topbar.c
 *
 * 状态栏：时间 | 🍃 | 电池 87% | 26° 晴
 * 布局：flex 横向，左时间，右电池+天气
 * 注：占位阶段用 LVGL 符号（montserrat_20 内置字形），emoji 素材后补
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>

#include "widget_topbar.h"
#include "../theme/theme_plant.h"

/* montserrat_20 内置 FontAwesome 字形（LVGL v9 符号表） */

#define TOPBAR_ICON_LEAF   LV_SYMBOL_TINT   /* 0xF043 水滴——占位"植物"图标 */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *widget_topbar_create(lv_obj_t *parent)
{
  lv_obj_t *bar = lv_obj_create(parent);

  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LV_PCT(100), TP_TOP_BAR_H);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(TP_CLR_TOPBAR_BG), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(bar, 12, 0);

  /* flex 横向布局：左时间，右电池+天气 */

  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(bar, 8, 0);

  /* 左：时间（阶段 B：默认 "--:--"，真实时间由 ui_task 定时器
   * 用 widget_topbar_set_time 刷新——服务器 GET /time，断网保持 --:--） */

  lv_obj_t *time_label = lv_label_create(bar);
  lv_label_set_text(time_label, "--:--");
  lv_obj_add_style(time_label, &theme_style_text_main, 0);
  lv_obj_set_style_text_font(time_label, TP_FONT_TITLE, 0);

  /* 植物图标（占位） */

  lv_obj_t *leaf_icon = lv_label_create(bar);
  lv_label_set_text(leaf_icon, TOPBAR_ICON_LEAF);
  lv_obj_set_style_text_font(leaf_icon, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(leaf_icon, lv_color_hex(TP_CLR_PRIMARY_GREEN_DARK), 0);

  /* 右：电池 + 天气（用 spacer 顶到右边） */

  lv_obj_t *spacer = lv_obj_create(bar);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_flex_grow(spacer, 1);

  lv_obj_t *bat_label = lv_label_create(bar);
  lv_label_set_text(bat_label, "电池 --%");   /* 占位；真实值由 ui_refresh 刷新 */
  lv_obj_add_style(bat_label, &theme_style_text_sub, 0);

  lv_obj_t *wx_label = lv_label_create(bar);
  lv_label_set_text(wx_label, "");
  lv_obj_add_style(wx_label, &theme_style_text_sub, 0);

  return bar;
}

void widget_topbar_set_battery(lv_obj_t *topbar, int pct)
{
  lv_obj_t *bat = (lv_obj_t *)lv_obj_get_child(topbar, 3);
  char buf[32];

  if (bat != NULL)
    {
      snprintf(buf, sizeof(buf), "电池 %d%%", pct);
      lv_label_set_text(bat, buf);
    }
}

void widget_topbar_set_time(lv_obj_t *topbar, const char *time_str)
{
  lv_obj_t *t = (lv_obj_t *)lv_obj_get_child(topbar, 0);

  if (t != NULL)
    {
      lv_label_set_text(t, time_str);
    }
}

void widget_topbar_set_weather(lv_obj_t *topbar, const char *weather)
{
  lv_obj_t *wx = (lv_obj_t *)lv_obj_get_child(topbar, 4);

  if (wx != NULL)
    {
      lv_label_set_text(wx, weather != NULL ? weather : "");
    }
}
