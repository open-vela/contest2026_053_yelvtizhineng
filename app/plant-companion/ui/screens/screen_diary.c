/****************************************************************************
 * apps/plant-companion/ui/screens/screen_diary.c
 *
 * (7) 成长日记 —— 对齐《嵌入式 UI V3 大字版》
 *   顶栏 32 + 成长 Vlog 条 46 + 时间轴 166 = 244 + 底部 Tab 52 = 296
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>

#include "screen_diary.h"
#include "../theme/theme_plant.h"
#include "../../services/record_service.h"
#include "../../services/plant_state.h"
#include "../widgets/widget_bottomnav.h"

#define IC_BOOK    "\xF0\x9F\x93\x96"          /* U+1F4D6 */
#define IC_CLAPPER "\xF0\x9F\x8E\xAC"          /* U+1F3AC */
#define IC_PLAY    "\xE2\x96\xB6"              /* U+25B6  */
#define IC_PHOTO   "\xF0\x9F\x93\xB7"          /* U+1F4F7 */
#define IC_WATER   "\xF0\x9F\x92\xA7"          /* U+1F4A7 */
#define IC_LEAF    "\xF0\x9F\x8C\xBF"          /* U+1F33F */
#define IC_SPARK   "\xE2\x9C\xA8"              /* U+2728  */
#define IC_TROPHY  "\xF0\x9F\x8F\x86"          /* U+1F3C6 */

/* 时间显示映射（按 day_offset） */

static const char *const diary_time_label(int offset)
{
  switch (offset)
    {
      case 0:  return "今天";
      case 1:  return "昨天";
      case 3:  return "3天前";
      case 7:  return "7天前";
      default: return "";
    }
}

/* 时间轴圆点图标：按条目主题色挑一个（配色 -> 图标） */

static const char *diary_dot_icon(uint32_t color)
{
  switch (color)
    {
      case 0x3b82f6:
      case 0x2196f3:
        return IC_WATER;

      case 0x22c55e:
      case 0x16a34a:
        return IC_LEAF;

      case 0xa78bfa:
      case 0x7c3aed:
        return IC_SPARK;

      case 0xfbbf24:
      case 0xf59e0b:
        return IC_PHOTO;

      default:
        return IC_LEAF;
    }
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 时间线条目：圆点 + 卡片（标题 / 时间） */

static lv_obj_t *diary_entry_create(lv_obj_t *parent, int idx)
{
  struct record_diary_s d;
  lv_obj_t *row;
  lv_obj_t *dot;
  lv_obj_t *card;
  lv_obj_t *body;
  lv_obj_t *l;

  if (!record_diary_get(idx, &d))
    {
      return NULL;
    }

  row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 42);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  dot = lv_obj_create(row);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 22, 22);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(d.color), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  l = lv_label_create(dot);
  lv_label_set_text(l, diary_dot_icon(d.color));
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_center(l);

  card = lv_obj_create(row);
  lv_obj_remove_style_all(card);
  lv_obj_set_height(card, 42);
  lv_obj_set_flex_grow(card, 1);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_hor(card, 10, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  body = lv_obj_create(card);
  lv_obj_remove_style_all(body);
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  l = lv_label_create(body);
  lv_label_set_text(l, d.title);
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_TEXT_MAIN), 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, LV_PCT(100));

  l = lv_label_create(body);
  lv_label_set_text(l, diary_time_label((int)d.day_offset));
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_TEXT_MUTED), 0);

  if (d.detail[0] != '\0')
    {
      l = lv_label_create(card);
      lv_label_set_text(l, d.detail);
      lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_TEXT_SUB), 0);
    }

  return row;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_diary_create(plant_nav_cb_t cb, void *user_data)
{
  lv_obj_t *scr = lv_obj_create(lv_screen_active());
  lv_obj_t *top;
  lv_obj_t *vlog;
  lv_obj_t *list;
  lv_obj_t *l;
  struct plant_state_s ps;
  char buf[64];
  int i;

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 320);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  theme_page_bg(scr, TP_CLR_DIARY_BG_A, TP_CLR_DIARY_BG_B);

  plant_state_get(&ps);

  /* ── 顶栏（32） ─────────────────────────────────────────────── */

  top = lv_obj_create(scr);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, 480, 32);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_style_pad_hor(top, 12, 0);
  lv_obj_set_style_pad_column(top, 8, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  l = lv_label_create(top);
  snprintf(buf, sizeof(buf), IC_BOOK " %s的日记",
           ps.name[0] != '\0' ? ps.name : "小绿绿");
  lv_label_set_text(l, buf);
  lv_obj_set_style_text_font(l, TP_FONT_TITLE, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_GREEN_DEEP), 0);

  {
    lv_obj_t *sp = lv_obj_create(top);

    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 4, 4);
    lv_obj_set_flex_grow(sp, 1);
  }

  l = lv_label_create(top);
  snprintf(buf, sizeof(buf), "第%d天", ps.plant_day > 0 ? ps.plant_day : 1);
  lv_label_set_text(l, buf);
  lv_obj_set_style_bg_color(l, lv_color_hex(0xd1fae5), 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(l, 7, 0);
  lv_obj_set_style_pad_hor(l, 9, 0);
  lv_obj_set_style_pad_ver(l, 2, 0);
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0x059669), 0);

  /* ── 成长 Vlog 条（46） ─────────────────────────────────────── */

  vlog = lv_obj_create(scr);
  lv_obj_remove_style_all(vlog);
  lv_obj_set_size(vlog, 456, 46);
  lv_obj_set_pos(vlog, 12, 34);
  lv_obj_set_style_bg_color(vlog, lv_color_hex(0x166534), 0);
  lv_obj_set_style_bg_opa(vlog, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(vlog, lv_color_hex(0x15803d), 0);
  lv_obj_set_style_bg_grad_dir(vlog, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_radius(vlog, 12, 0);
  lv_obj_set_style_border_width(vlog, 0, 0);
  lv_obj_set_style_pad_hor(vlog, 12, 0);
  lv_obj_set_style_pad_column(vlog, 10, 0);
  lv_obj_set_flex_flow(vlog, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(vlog, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  {
    lv_obj_t *play = lv_obj_create(vlog);

    lv_obj_remove_style_all(play);
    lv_obj_set_size(play, 28, 28);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_30, 0);
    l = lv_label_create(play);
    lv_label_set_text(l, IC_PLAY);
    lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_center(l);
  }

  {
    lv_obj_t *info = lv_obj_create(vlog);

    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    l = lv_label_create(info);
    snprintf(buf, sizeof(buf), IC_CLAPPER " %d天成长Vlog",
             ps.plant_day > 0 ? ps.plant_day : 1);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_font(l, TP_FONT_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);

    l = lv_label_create(info);
    lv_label_set_text(l, "记录小绿的每一天");
    lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xd1fae5), 0);
  }

  /* ── 时间轴（166，可滚动） ──────────────────────────────────── */

  list = lv_obj_create(scr);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, 456, 166);
  lv_obj_set_pos(list, 12, 86);
  lv_obj_set_style_pad_row(list, 4, 0);
  lv_obj_set_style_pad_hor(list, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

  for (i = 0; i < record_diary_get_count(); i++)
    {
      diary_entry_create(list, i);
    }

  /* ── 底部 Tab（52） ─────────────────────────────────────────── */

  widget_bottomnav_create(scr, cb, user_data, PLANT_TAB_DIARY,
                          PLANT_NAV_GREEN, false);

  return scr;
}
