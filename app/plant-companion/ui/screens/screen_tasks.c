/****************************************************************************
 * apps/plant-companion/ui/screens/screen_tasks.c
 *
 * (5) 今日任务 —— 对齐《嵌入式 UI V3 大字版》
 *   顶栏 32 + 3 条任务 166 + 成就条 46 = 244 + 底部 Tab 52 = 296
 *   点任务项 = 切换完成（保留原逻辑：完成联动日记 + 成就判定）
 ****************************************************************************/

#include <lvgl/lvgl.h>

#include <stdio.h>
#include "screen_tasks.h"
#include "../theme/theme_plant.h"
#include "../../services/record_service.h"
#include "../../services/plant_state.h"
#include "../widgets/widget_bottomnav.h"

#define IC_DATE    "\xF0\x9F\x93\x85"          /* U+1F4C5 */
#define IC_STAR    "\xE2\xAD\x90"              /* U+2B50 */
#define IC_MEDAL   "\xF0\x9F\x8F\x85"          /* U+1F3C5 */

/* 任务序号 → 小图标（记录层没有图标字段，按序号配一个好看的） */

static const char *const task_icons[TASK_MAX] =
{
  "\xF0\x9F\x91\x80",   /* U+1F440 眼睛 */
  "\xE2\x98\x80",       /* U+2600  太阳 */
  "\xF0\x9F\x93\xB7",   /* U+1F4F7 相机 */
  "\xF0\x9F\x92\xA7",   /* U+1F4A7 水滴 */
  "\xF0\x9F\x8C\xBF",   /* U+1F33F 叶子 */
  IC_STAR,
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_progress;
static lv_obj_t *s_ach_text;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 顶部右侧进度：已完成 / 总数 */

static void tasks_update_progress(void)
{
  char buf[16];

  if (s_progress != NULL)
    {
      snprintf(buf, sizeof(buf), "%d/%d", record_task_done_count(),
               record_task_get_count());
      lv_label_set_text(s_progress, buf);
    }
}

/* 刷新单项：完成 → 绿实心圆 + 删除线；未完成 → 琥珀空心圆 */

static void task_refresh_item(lv_obj_t *row, int idx)
{
  struct record_task_s t;

  if (!record_task_get(idx, &t))
    {
      return;
    }

  {
    lv_obj_t *check = (lv_obj_t *)lv_obj_get_child(row, 0);
    lv_obj_t *body = (lv_obj_t *)lv_obj_get_child(row, 2);
    lv_obj_t *title = (lv_obj_t *)lv_obj_get_child(body, 0);
    lv_obj_t *time_l = (lv_obj_t *)lv_obj_get_child(body, 1);
    lv_obj_t *reward = (lv_obj_t *)lv_obj_get_child(row, 3);
    lv_obj_t *tick = (lv_obj_t *)lv_obj_get_child(check, 0);

    if (t.done)
      {
        lv_obj_set_style_bg_color(check, lv_color_hex(TP_CLR_GREEN), 0);
        lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(check, 0, 0);
        lv_obj_remove_flag(tick, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_STRIKETHROUGH, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(TP_CLR_TEXT_MUTED), 0);
        lv_obj_set_style_opa(reward, LV_OPA_COVER, 0);
      }
    else
      {
        lv_obj_set_style_bg_color(check, lv_color_hex(0xfef3c7), 0);
        lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(check, 2, 0);
        lv_obj_set_style_border_color(check, lv_color_hex(TP_CLR_AMBER), 0);
        lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_NONE, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(TP_CLR_TEXT_MAIN), 0);
        lv_obj_set_style_opa(reward, LV_OPA_40, 0);
      }

    /* 2026-09-18 修复：标题此前从未写入，任务页只显示图标/时间，任务名空白 */

    lv_label_set_text(title, t.title);

    if (time_l != NULL)
      {
        lv_label_set_text(time_l, t.time_slot);
      }
  }
}

static void task_on_click(lv_event_t *e)
{
  lv_obj_t *row = lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(row);

  if (idx < 0 || idx >= TASK_MAX)
    {
      return;
    }

  {
    struct record_task_s t;

    if (record_task_get(idx, &t))
      {
        record_task_set_done(idx, !t.done);

        /* 任务完成 → 日记联动 + 成就判定 */

        if (!t.done)
          {
            record_task_on_completed();
          }
      }
  }

  task_refresh_item(row, idx);
  tasks_update_progress();
}

static lv_obj_t *task_row_create(lv_obj_t *parent, int idx)
{
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_t *check;
  lv_obj_t *tick;
  lv_obj_t *icon;
  lv_obj_t *body;
  lv_obj_t *title;
  lv_obj_t *time_l;
  lv_obj_t *reward;

  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 52);
  lv_obj_set_style_bg_color(row, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 12, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_hor(row, 10, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_set_user_data(row, (void *)(intptr_t)idx);

  /* 圆形勾选指示 */

  check = lv_obj_create(row);
  lv_obj_remove_style_all(check);
  lv_obj_set_size(check, 26, 26);
  lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, 0);

  tick = lv_label_create(check);
  lv_label_set_text(tick, LV_SYMBOL_OK);
  lv_obj_set_style_text_color(tick, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(tick, TP_FONT_CAPTION, 0);
  lv_obj_center(tick);

  /* 任务图标 */

  icon = lv_label_create(row);
  lv_label_set_text(icon, task_icons[idx % TASK_MAX]);
  lv_obj_set_style_text_font(icon, TP_ICON_L, 0);

  /* 标题 + 时间 */

  body = lv_obj_create(row);
  lv_obj_remove_style_all(body);
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  title = lv_label_create(body);
  lv_label_set_text(title, "");
  lv_obj_set_style_text_font(title, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(TP_CLR_TEXT_MAIN), 0);

  time_l = lv_label_create(body);
  lv_label_set_text(time_l, "");
  lv_obj_set_style_text_font(time_l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(time_l, lv_color_hex(TP_CLR_TEXT_SUB), 0);

  /* 完成奖励星 */

  reward = lv_label_create(row);
  lv_label_set_text(reward, IC_STAR);
  lv_obj_set_style_text_font(reward, TP_ICON_L, 0);

  /* 2026-09-18 修复：check/icon/body/reward 都是 lv_obj_create 出来的，
   * 默认带 CLICKABLE，会把点击吃掉 —— 真机用手指点任务标题没有任何反应，
   * 只有点最左侧空白才生效。这里统一去掉子对象的 CLICKABLE，
   * 让点击落到整行 row 上（row 才是带回调、带 idx 的对象）。 */

  lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(reward, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_add_event_cb(row, task_on_click, LV_EVENT_CLICKED, NULL);

  task_refresh_item(row, idx);

  return row;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_tasks_create(plant_nav_cb_t cb, void *user_data)
{
  lv_obj_t *scr = lv_obj_create(lv_screen_active());
  lv_obj_t *top;
  lv_obj_t *list;
  lv_obj_t *ach;
  lv_obj_t *l;
  struct plant_state_s ps;
  char buf[64];
  int i;

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 320);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  theme_page_bg(scr, TP_CLR_TASK_BG_A, TP_CLR_TASK_BG_B);

  plant_state_get(&ps);

  /* ── 顶栏（32） ─────────────────────────────────────────────── */

  top = lv_obj_create(scr);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, 480, 32);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_style_pad_hor(top, 12, 0);
  lv_obj_set_style_pad_column(top, 7, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  l = lv_label_create(top);
  snprintf(buf, sizeof(buf), IC_DATE " 第%d天",
           ps.plant_day > 0 ? ps.plant_day : 1);
  lv_label_set_text(l, buf);
  lv_obj_set_style_bg_color(l, lv_color_hex(0xfde68a), 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(l, 8, 0);
  lv_obj_set_style_pad_hor(l, 8, 0);
  lv_obj_set_style_pad_ver(l, 3, 0);
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_AMBER_TEXT), 0);

  l = lv_label_create(top);
  lv_label_set_text(l, "今天的小任务");
  lv_obj_set_style_text_font(l, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_BROWN_TEXT), 0);

  {
    lv_obj_t *sp = lv_obj_create(top);

    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 4, 4);
    lv_obj_set_flex_grow(sp, 1);
  }

  s_progress = lv_label_create(top);
  lv_label_set_text(s_progress, "0/0");
  lv_obj_set_style_text_font(s_progress, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(s_progress, lv_color_hex(TP_CLR_AMBER_TEXT), 0);

  /* ── 任务列表 ───────────────────────────────────────────────── */

  list = lv_obj_create(scr);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, 456, 168);
  lv_obj_set_pos(list, 12, 34);
  lv_obj_set_style_pad_row(list, 5, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  for (i = 0; i < record_task_get_count() && i < TASK_MAX; i++)
    {
      task_row_create(list, i);
    }

  tasks_update_progress();

  /* ── 成就条（46） ───────────────────────────────────────────── */

  ach = lv_obj_create(scr);
  lv_obj_remove_style_all(ach);
  lv_obj_set_size(ach, 456, 46);
  lv_obj_set_pos(ach, 12, 208);
  lv_obj_set_style_bg_color(ach, lv_color_hex(TP_CLR_AMBER), 0);
  lv_obj_set_style_bg_opa(ach, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(ach, lv_color_hex(0xf97316), 0);
  lv_obj_set_style_bg_grad_dir(ach, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_radius(ach, 12, 0);
  lv_obj_set_style_border_width(ach, 0, 0);
  lv_obj_set_style_pad_hor(ach, 12, 0);
  lv_obj_set_style_pad_column(ach, 10, 0);
  lv_obj_set_flex_flow(ach, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ach, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  l = lv_label_create(ach);
  lv_label_set_text(l, IC_MEDAL);
  lv_obj_set_style_text_font(l, TP_ICON_L, 0);

  s_ach_text = lv_label_create(ach);
  snprintf(buf, sizeof(buf), "连续养护%d天",
           ps.plant_day > 0 ? ps.plant_day : 1);
  lv_label_set_text(s_ach_text, buf);
  lv_obj_set_flex_grow(s_ach_text, 1);
  lv_obj_set_style_text_font(s_ach_text, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(s_ach_text, lv_color_hex(0xffffff), 0);

  {
    lv_obj_t *bar = lv_obj_create(ach);
    lv_obj_t *fill;

    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 56, 6);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);

    fill = lv_obj_create(bar);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, 34, 6);
    lv_obj_set_style_radius(fill, 3, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
  }

  /* ── 底部 Tab（52） ─────────────────────────────────────────── */

  widget_bottomnav_create(scr, cb, user_data, PLANT_TAB_TASKS,
                          PLANT_NAV_AMBER, false);

  return scr;
}
