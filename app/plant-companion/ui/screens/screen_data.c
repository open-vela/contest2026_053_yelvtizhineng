/****************************************************************************
 * apps/plant-companion/ui/screens/screen_data.c
 *
 * ④ 传感器数据 —— 实时监测（2026-09-11 重写为 8 个真实参数）
 *   顶栏 32 + 2列×4行 八张真实参数卡（226）+ 底部 Tab 52 = 310
 *   顺序：温度 / 水分 / EC / 盐分 / 氮 / 磷 / 钾 / pH
 *
 * 数据来源：sensor_service（Modbus 轮询缓存，全部为真实读数）
 * 无有效读数时一律显示 "--" + 中性灰圆点，不编造任何数值。
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>

#include "screen_data.h"
#include "../theme/theme_plant.h"
#include "../widgets/widget_bottomnav.h"

#define IC_CHART   "\xF0\x9F\x93\x8A"          /* U+1F4CA */

/* 8 张真实参数卡 */

#define DATA_CARD_NUM 8

enum
{
  DATA_IDX_TEMP = 0,      /* 温度 °C */
  DATA_IDX_MOISTURE,      /* 水分 % */
  DATA_IDX_EC,            /* EC uS/cm */
  DATA_IDX_SALT,          /* 盐分 */
  DATA_IDX_NITROGEN,      /* 氮 mg/kg */
  DATA_IDX_PHOSPHORUS,    /* 磷 mg/kg */
  DATA_IDX_POTASSIUM,     /* 钾 mg/kg */
  DATA_IDX_PH             /* pH */
};

static lv_obj_t *s_val[DATA_CARD_NUM];   /* 大数值 label */
static lv_obj_t *s_dot[DATA_CARD_NUM];   /* 状态圆点 */

/* 中性灰：没有阈值规则的参数（盐分/N/P/K/pH）圆点用 */

#define CLR_DOT_NEUTRAL 0x94a3b8

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static lv_color_t status_color(int status)
{
  switch (status)
    {
      case SENSOR_STATUS_BAD:
        return lv_color_hex(TP_CLR_STATE_BAD);

      case SENSOR_STATUS_WARN:
        return lv_color_hex(TP_CLR_STATE_WARN);

      default:
        return lv_color_hex(TP_CLR_STATE_OK);
    }
}

/* 数据卡：名称（上）+ 大数值含单位（下），右上角状态圆点 */

static lv_obj_t *data_card(lv_obj_t *parent, const char *name, int idx)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_t *l;

  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &theme_style_card_small, 0);
  lv_obj_set_size(card, 223, 52);
  lv_obj_set_style_pad_all(card, 0, 0);

  l = lv_label_create(card);
  lv_label_set_text(l, name);
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_TEXT_SUB), 0);
  lv_obj_set_pos(l, 12, 5);

  l = lv_label_create(card);
  lv_label_set_text(l, "--");
  lv_obj_set_style_text_font(l, TP_FONT_TITLE, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_TEXT_MAIN), 0);
  lv_obj_set_pos(l, 12, 22);
  s_val[idx] = l;

  l = lv_obj_create(card);
  lv_obj_remove_style_all(l);
  lv_obj_set_size(l, 10, 10);
  lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(l, lv_color_hex(CLR_DOT_NEUTRAL), 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -12, 16);
  s_dot[idx] = l;

  return card;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_data_create(plant_nav_cb_t cb, void *user_data)
{
  static const char *const names[DATA_CARD_NUM] =
  {
    "温度", "水分", "EC", "盐分",
    "氮", "磷", "钾", "pH"
  };
  lv_obj_t *scr = lv_obj_create(lv_screen_active());
  lv_obj_t *top;
  lv_obj_t *grid;
  lv_obj_t *l;
  int i;

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 320);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  theme_page_bg(scr, TP_CLR_DATA_BG_A, TP_CLR_DATA_BG_B);

  /* ── 顶栏（32） ─────────────────────────────────────────────── */

  top = lv_obj_create(scr);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, 480, 32);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_style_pad_hor(top, 14, 0);
  lv_obj_set_style_pad_column(top, 6, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  l = lv_label_create(top);
  lv_label_set_text(l, IC_CHART " 小绿的数据");
  lv_obj_set_style_text_font(l, TP_FONT_TITLE, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_TEXT_MAIN), 0);

  l = lv_label_create(top);
  lv_label_set_text(l, "· 实时");
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_TEXT_MUTED), 0);

  /* ── 2 列 × 4 行 八张真实参数卡（226） ──────────────────────── */

  grid = lv_obj_create(scr);
  lv_obj_remove_style_all(grid);
  lv_obj_set_size(grid, 480, 226);
  lv_obj_set_pos(grid, 0, 38);
  lv_obj_set_style_pad_hor(grid, 12, 0);
  lv_obj_set_style_pad_row(grid, 6, 0);
  lv_obj_set_style_pad_column(grid, 10, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  for (i = 0; i < DATA_CARD_NUM; i++)
    {
      data_card(grid, names[i], i);
    }

  /* ── 底部 Tab（52） ─────────────────────────────────────────── */

  widget_bottomnav_create(scr, cb, user_data, PLANT_TAB_DATA,
                          PLANT_NAV_BLUE, false);

  return scr;
}

/* 刷新：把 sensor_service 缓存的 8 个真实参数上屏 */

void screen_data_refresh(const struct sensor_view_s *view,
                         const struct sensor_history_s *hist)
{
  char buf[32];
  int i;

  (void)hist;

  if (view == NULL)
    {
      return;
    }

  if (!view->valid)
    {
      /* 没有有效读数：全部 "--"，圆点中性灰 */

      for (i = 0; i < DATA_CARD_NUM; i++)
        {
          if (s_val[i] != NULL)
            {
              lv_label_set_text(s_val[i], "--");
            }

          if (s_dot[i] != NULL)
            {
              lv_obj_set_style_bg_color(s_dot[i],
                                        lv_color_hex(CLR_DOT_NEUTRAL), 0);
            }
        }

      return;
    }

  if (s_val[DATA_IDX_TEMP] != NULL)
    {
      snprintf(buf, sizeof(buf), "%.1f°C", (double)view->temp);
      lv_label_set_text(s_val[DATA_IDX_TEMP], buf);
    }

  if (s_dot[DATA_IDX_TEMP] != NULL)
    {
      lv_obj_set_style_bg_color(s_dot[DATA_IDX_TEMP],
                                status_color(view->temp_status), 0);
    }

  if (s_val[DATA_IDX_MOISTURE] != NULL)
    {
      snprintf(buf, sizeof(buf), "%.1f%%", (double)view->moisture);
      lv_label_set_text(s_val[DATA_IDX_MOISTURE], buf);
    }

  if (s_dot[DATA_IDX_MOISTURE] != NULL)
    {
      lv_obj_set_style_bg_color(s_dot[DATA_IDX_MOISTURE],
                                status_color(view->moisture_status), 0);
    }

  if (s_val[DATA_IDX_EC] != NULL)
    {
      snprintf(buf, sizeof(buf), "%.0f uS/cm", (double)view->ec);
      lv_label_set_text(s_val[DATA_IDX_EC], buf);
    }

  if (s_dot[DATA_IDX_EC] != NULL)
    {
      lv_obj_set_style_bg_color(s_dot[DATA_IDX_EC],
                                status_color(view->ec_status), 0);
    }

  if (s_val[DATA_IDX_SALT] != NULL)
    {
      snprintf(buf, sizeof(buf), "%.0f mg/kg", (double)view->salt);
      lv_label_set_text(s_val[DATA_IDX_SALT], buf);
    }

  if (s_val[DATA_IDX_NITROGEN] != NULL)
    {
      snprintf(buf, sizeof(buf), "%.0f mg/kg", (double)view->nitrogen);
      lv_label_set_text(s_val[DATA_IDX_NITROGEN], buf);
    }

  if (s_val[DATA_IDX_PHOSPHORUS] != NULL)
    {
      snprintf(buf, sizeof(buf), "%.0f mg/kg", (double)view->phosphorus);
      lv_label_set_text(s_val[DATA_IDX_PHOSPHORUS], buf);
    }

  if (s_val[DATA_IDX_POTASSIUM] != NULL)
    {
      snprintf(buf, sizeof(buf), "%.0f mg/kg", (double)view->potassium);
      lv_label_set_text(s_val[DATA_IDX_POTASSIUM], buf);
    }

  if (s_val[DATA_IDX_PH] != NULL)
    {
      snprintf(buf, sizeof(buf), "%.1f", (double)view->ph);
      lv_label_set_text(s_val[DATA_IDX_PH], buf);
    }
}
