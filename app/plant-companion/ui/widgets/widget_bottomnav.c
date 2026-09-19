/****************************************************************************
 * apps/plant-companion/ui/widgets/widget_bottomnav.c
 *
 * 底部 Tab 栏（V3 大字版）
 *   - 高度 52px（旧版 64）
 *   - 图标 20px 单色 emoji + 文字 12px/加粗
 *   - 每页主题色：选中项着色 + Tab 栏同色系淡底
 *   - 语音页第 4 项为「🎤 说话」
 *
 * 图标（OpenMoji 子集，见 ui/assets/fonts/lv_font_emoji_*）：
 *   🏠 首页 / 📊 数据 / 📋 任务 / 📖 日记 / 🎤 说话
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <string.h>
#include <stdio.h>

#include "widget_bottomnav.h"
#include "../theme/theme_plant.h"

#define NAV_TAB_COUNT   PLANT_TAB_COUNT

/* UTF-8 图标常量（避免源文件里直接嵌 4 字节字符） */

#define ICON_HOME    "\xef\x9d\x8f\xef\xb8\x8f"          /* not used, placeholder */
#define ICON_TXT_HOME    "\xf0\x9f\x8f\xa0"   /* U+1F3E0 house */
#define ICON_TXT_DATA    "\xf0\x9f\x93\x8a"   /* U+1F4CA chart */
#define ICON_TXT_TASKS   "\xf0\x9f\x93\x8b"   /* U+1F4CB clipboard */
#define ICON_TXT_DIARY   "\xf0\x9f\x93\x96"   /* U+1F4D6 book */
#define ICON_TXT_VOICE   "\xf0\x9f\x8e\xa4"   /* U+1F3A4 mic */

static const char *const nav_icons[NAV_TAB_COUNT] =
{
  ICON_TXT_HOME, ICON_TXT_DATA, ICON_TXT_TASKS, ICON_TXT_DIARY
};

static const char *const nav_labels[NAV_TAB_COUNT] =
{
  "首页", "数据", "任务", "日记"
};

struct nav_ctx_s
{
  plant_nav_cb_t cb;
  void *user_data;
  lv_obj_t *tabs[NAV_TAB_COUNT];
  lv_obj_t *labels[NAV_TAB_COUNT];
  lv_obj_t *icons[NAV_TAB_COUNT];
  plant_nav_theme_t theme;
  int active;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t nav_accent(plant_nav_theme_t th)
{
  switch (th)
    {
      case PLANT_NAV_BLUE:
        return TP_CLR_BLUE;

      case PLANT_NAV_AMBER:
        return TP_CLR_AMBER_DARK;

      case PLANT_NAV_PINK:
        return TP_CLR_PINK_DARK;

      default:
        return TP_CLR_GREEN;
    }
}

static uint32_t nav_bg(plant_nav_theme_t th)
{
  switch (th)
    {
      case PLANT_NAV_BLUE:
        return TP_CLR_NAV_BG_BLUE;

      case PLANT_NAV_AMBER:
        return TP_CLR_NAV_BG_AMBER;

      case PLANT_NAV_PINK:
        return TP_CLR_NAV_BG_PINK;

      default:
        return TP_CLR_NAV_BG_GREEN;
    }
}

static void nav_apply(struct nav_ctx_s *ctx)
{
  uint32_t accent = nav_accent(ctx->theme);
  int i;

  for (i = 0; i < NAV_TAB_COUNT; i++)
    {
      uint32_t col = (i == ctx->active) ? accent : TP_CLR_TEXT_MUTED;

      if (ctx->labels[i] != NULL)
        {
          lv_obj_set_style_text_color(ctx->labels[i], lv_color_hex(col), 0);
        }

      if (ctx->icons[i] != NULL)
        {
          lv_obj_set_style_text_color(ctx->icons[i], lv_color_hex(col), 0);
        }
    }
}

static void nav_on_click(lv_event_t *e)
{
  lv_obj_t *tab = lv_event_get_target(e);
  struct nav_ctx_s *ctx = (struct nav_ctx_s *)lv_obj_get_user_data(
                             lv_obj_get_parent(tab));
  int tab_id = (int)(intptr_t)lv_obj_get_user_data(tab);

  if (ctx == NULL)
    {
      return;
    }

  if (ctx->active != tab_id)
    {
      ctx->active = tab_id;
      nav_apply(ctx);

      if (ctx->cb != NULL)
        {
          ctx->cb(ctx->user_data, tab_id);
        }
    }
}

/* 页面删除时释放 nav_ctx_s —— 否则每次建页 lv_malloc 的 ctx 会泄漏
 * （LVGL 不会自动释放 user_data），长期切 Tab 会耗尽 LVGL 池 → 黑屏。 */

static void nav_on_delete(lv_event_t *e)
{
  struct nav_ctx_s *ctx = (struct nav_ctx_s *)lv_event_get_user_data(e);

  if (ctx != NULL)
    {
      lv_free(ctx);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *widget_bottomnav_create(lv_obj_t *parent, plant_nav_cb_t cb,
                                  void *user_data, int active,
                                  plant_nav_theme_t theme, bool voice)
{
  struct nav_ctx_s *ctx;
  lv_obj_t *nav;
  int i;

  ctx = (struct nav_ctx_s *)lv_malloc(sizeof(struct nav_ctx_s));
  if (ctx == NULL)
    {
      printf("[Nav] lv_malloc ctx FAILED\n");
      return NULL;
    }

  memset(ctx, 0, sizeof(*ctx));
  ctx->cb = cb;
  ctx->user_data = user_data;
  ctx->theme = theme;
  ctx->active = (active >= 0 && active < NAV_TAB_COUNT) ? active
                                                        : PLANT_TAB_HOME;

  nav = lv_obj_create(parent);
  lv_obj_remove_style_all(nav);
  lv_obj_set_size(nav, TP_CONTENT_W, TP_NAV_BAR_H);
  lv_obj_set_pos(nav, 0, 320 - TP_NAV_BAR_H);
  lv_obj_set_style_bg_color(nav, lv_color_hex(nav_bg(theme)), 0);
  lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(nav, 0, 0);
  lv_obj_set_style_border_width(nav, 1, 0);
  lv_obj_set_style_border_color(nav, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_opa(nav, LV_OPA_10, 0);
  lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_pad_all(nav, 0, 0);
  lv_obj_set_user_data(nav, ctx);

  lv_obj_add_event_cb(nav, nav_on_delete, LV_EVENT_DELETE, ctx);

  /* flex 横向均分 4 Tab */

  lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  for (i = 0; i < NAV_TAB_COUNT; i++)
    {
      lv_obj_t *tab = lv_obj_create(nav);
      lv_obj_t *icon;
      lv_obj_t *label;

      lv_obj_remove_style_all(tab);
      lv_obj_set_size(tab, 110, TP_NAV_BAR_H);
      lv_obj_set_style_radius(tab, 0, 0);
      lv_obj_set_style_pad_all(tab, 0, 0);
      lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
      lv_obj_set_user_data(tab, (void *)(intptr_t)i);

      lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(tab, 1, 0);

      icon = lv_label_create(tab);
      lv_label_set_text(icon, (voice && i == PLANT_TAB_DIARY) ?
                        ICON_TXT_VOICE : nav_icons[i]);
      lv_obj_set_style_text_font(icon, TP_ICON_M, 0);

      label = lv_label_create(tab);
      lv_label_set_text(label, (voice && i == PLANT_TAB_DIARY) ?
                        "说话" : nav_labels[i]);
      lv_obj_set_style_text_font(label, TP_FONT_CAPTION, 0);

      ctx->tabs[i] = tab;
      ctx->icons[i] = icon;
      ctx->labels[i] = label;

      lv_obj_add_event_cb(tab, nav_on_click, LV_EVENT_CLICKED, NULL);
    }

  nav_apply(ctx);

  return nav;
}

void widget_bottomnav_set_active(lv_obj_t *nav, int tab_id)
{
  struct nav_ctx_s *ctx = (struct nav_ctx_s *)lv_obj_get_user_data(nav);

  if (ctx == NULL || tab_id < 0 || tab_id >= NAV_TAB_COUNT)
    {
      return;
    }

  ctx->active = tab_id;
  nav_apply(ctx);
}

int widget_bottomnav_get_active(lv_obj_t *nav)
{
  struct nav_ctx_s *ctx = (struct nav_ctx_s *)lv_obj_get_user_data(nav);

  return (ctx != NULL) ? ctx->active : PLANT_TAB_HOME;
}

lv_obj_t *widget_bottomnav_find(lv_obj_t *screen)
{
  uint32_t i;
  uint32_t cnt;

  if (screen == NULL)
    {
      return NULL;
    }

  cnt = lv_obj_get_child_count(screen);

  for (i = 0; i < cnt; i++)
    {
      lv_obj_t *child = lv_obj_get_child(screen, i);

      if (lv_obj_get_parent(child) != screen)
        {
          continue;
        }

      /* 底部导航几何特征：全宽 480、高 TP_NAV_BAR_H、底边贴屏底。
       * （避免解引用任意 child 的 user_data——Tab 按钮的 user_data 是
       * 0..3 的小整数，当指针用会段错误。） */

      if (lv_obj_get_width(child) == TP_CONTENT_W &&
          lv_obj_get_height(child) == TP_NAV_BAR_H &&
          lv_obj_get_y(child) + lv_obj_get_height(child) == 320)
        {
          struct nav_ctx_s *ctx;

          ctx = (struct nav_ctx_s *)lv_obj_get_user_data(child);
          if (ctx != NULL && ctx->cb != NULL)
            {
              return child;
            }
        }
    }

  return NULL;
}

bool widget_bottomnav_tab_center(lv_obj_t *nav, int tab_id, int *cx, int *cy)
{
  struct nav_ctx_s *ctx;
  lv_obj_t *tab;
  lv_obj_t *o;
  int x = 0;
  int y = 0;

  if (nav == NULL)
    {
      return false;
    }

  ctx = (struct nav_ctx_s *)lv_obj_get_user_data(nav);
  if (ctx == NULL || tab_id < 0 || tab_id >= NAV_TAB_COUNT)
    {
      return false;
    }

  tab = ctx->tabs[tab_id];
  if (tab == NULL)
    {
      return false;
    }

  for (o = tab; o != NULL; o = lv_obj_get_parent(o))
    {
      x += lv_obj_get_x(o);
      y += lv_obj_get_y(o);
    }

  if (cx != NULL)
    {
      *cx = x + lv_obj_get_width(tab) / 2;
    }

  if (cy != NULL)
    {
      *cy = y + lv_obj_get_height(tab) / 2;
    }

  return true;
}
