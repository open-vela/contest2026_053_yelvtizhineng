/****************************************************************************
 * apps/plant-companion/ui/theme/theme_plant.c
 *
 * 植小伴设计系统 —— 全局样式初始化（对齐 V3 大字版设计稿）
 ****************************************************************************/

#include <lvgl/lvgl.h>

#include "theme_plant.h"
#include "../assets/fonts/zh_font.h"

/****************************************************************************
 * 全局样式
 ****************************************************************************/

lv_style_t theme_style_card;        /* 白底圆角卡片 */
lv_style_t theme_style_card_small;  /* 小卡片 */
lv_style_t theme_style_text_main;   /* 主文字 */
lv_style_t theme_style_text_sub;    /* 次要文字 */
lv_style_t theme_style_text_big;    /* 大数字 */
lv_style_t theme_style_btn_primary; /* 主按钮 */

const lv_font_t *g_font_caption;
const lv_font_t *g_font_body;
const lv_font_t *g_font_title;
const lv_font_t *g_font_value;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void theme_plant_init(void)
{
  /* zh_font_init（ui_app_start 先于本函数调用）已绑定主字体
   * （flash 子集 + SD 兜底），此处只取指针。 */

  g_font_caption = zh_font_get(12);
  g_font_body = zh_font_get(16);
  g_font_title = zh_font_get(19);
  g_font_value = zh_font_get(25);

  /* 兜底：任何一档取不到就退回 body，绝不让 UI 拿到 NULL 字体 */

  if (g_font_caption == NULL)
    {
      g_font_caption = g_font_body;
    }

  if (g_font_title == NULL)
    {
      g_font_title = g_font_body;
    }

  if (g_font_value == NULL)
    {
      g_font_value = g_font_body;
    }

  /* ---- 大卡片：白底 + 16 圆角 + 轻阴影 ---- */

  lv_style_init(&theme_style_card);
  lv_style_set_bg_color(&theme_style_card, lv_color_hex(TP_CLR_CARD_BG));
  lv_style_set_bg_opa(&theme_style_card, LV_OPA_COVER);
  lv_style_set_radius(&theme_style_card, TP_RADIUS_CARD);
  lv_style_set_pad_all(&theme_style_card, TP_PAD_CARD);
  lv_style_set_border_width(&theme_style_card, 0);
  lv_style_set_shadow_width(&theme_style_card, 8);
  lv_style_set_shadow_opa(&theme_style_card, LV_OPA_20);
  lv_style_set_shadow_offset_y(&theme_style_card, 2);

  /* ---- 小卡片：白底 + 12 圆角 ---- */

  lv_style_init(&theme_style_card_small);
  lv_style_set_bg_color(&theme_style_card_small, lv_color_hex(TP_CLR_CARD_BG));
  lv_style_set_bg_opa(&theme_style_card_small, LV_OPA_COVER);
  lv_style_set_radius(&theme_style_card_small, TP_RADIUS_SMALL);
  lv_style_set_pad_all(&theme_style_card_small, 8);
  lv_style_set_border_width(&theme_style_card_small, 0);
  lv_style_set_shadow_width(&theme_style_card_small, 4);
  lv_style_set_shadow_opa(&theme_style_card_small, LV_OPA_20);
  lv_style_set_shadow_offset_y(&theme_style_card_small, 2);

  /* ---- 主文字 ---- */

  lv_style_init(&theme_style_text_main);
  lv_style_set_text_color(&theme_style_text_main, lv_color_hex(TP_CLR_TEXT_MAIN));
  lv_style_set_text_font(&theme_style_text_main, TP_FONT_BODY);

  /* ---- 次要文字 ---- */

  lv_style_init(&theme_style_text_sub);
  lv_style_set_text_color(&theme_style_text_sub, lv_color_hex(TP_CLR_TEXT_SUB));
  lv_style_set_text_font(&theme_style_text_sub, TP_FONT_BODY);

  /* ---- 大数字 ---- */

  lv_style_init(&theme_style_text_big);
  lv_style_set_text_color(&theme_style_text_big, lv_color_hex(TP_CLR_TEXT_MAIN));
  lv_style_set_text_font(&theme_style_text_big, TP_FONT_VALUE);

  /* ---- 主按钮：主绿底白字 ---- */

  lv_style_init(&theme_style_btn_primary);
  lv_style_set_bg_color(&theme_style_btn_primary, lv_color_hex(TP_CLR_GREEN));
  lv_style_set_bg_opa(&theme_style_btn_primary, LV_OPA_COVER);
  lv_style_set_radius(&theme_style_btn_primary, TP_RADIUS_SMALL);
  lv_style_set_text_color(&theme_style_btn_primary, lv_color_hex(TP_CLR_TEXT_ON_PRIMARY));
  lv_style_set_text_font(&theme_style_btn_primary, TP_FONT_BODY);
  lv_style_set_pad_hor(&theme_style_btn_primary, 16);
  lv_style_set_pad_ver(&theme_style_btn_primary, 8);
}

void theme_page_bg(lv_obj_t *scr, uint32_t c1, uint32_t c2)
{
  if (scr == NULL)
    {
      return;
    }

  lv_obj_set_style_bg_color(scr, lv_color_hex(c1), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(scr, lv_color_hex(c2), 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
}

void theme_btn_set_text(lv_obj_t *btn, const char *text)
{
  lv_obj_t *label;

  if (btn == NULL || text == NULL)
    {
      return;
    }

  label = lv_obj_get_child(btn, 0);
  if (label != NULL && lv_obj_check_type(label, &lv_label_class))
    {
      lv_label_set_text(label, text);
      return;
    }

  label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
}
