/****************************************************************************
 * apps/plant-companion/ui/screens/screen_diagnose.c
 *
 * (3) 识别结果 —— 对齐《嵌入式 UI V3 大字版》
 *   顶栏 26 + 植物卡 58 + 诊断条 48 + 建议卡 48 + 底部按钮 36 = 216
 *
 * 数据全部来自服务器回传（ai_service / server_bridge），不做任何演示包装：
 * 失败就显示失败原文（由调用方传入的 section_title / issue_desc 决定）。
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>

#include "screen_diagnose.h"
#include "../ui_app.h"
#include "../theme/companion_style.h"
#include "../assets/fonts/zh_font.h"
#include "../../services/record_service.h"
#include "../../services/plant_state.h"

#define IC_LEAF    "\xF0\x9F\x8C\xBF"          /* U+1F33F */
#define IC_WARN    "\xE2\x9A\xA0"              /* U+26A0  */
#define IC_OK      "\xE2\x9C\x85"              /* U+2705  */
#define IC_BULB    "\xF0\x9F\x92\xA1"          /* U+1F4A1 */
#define IC_CLIP    "\xF0\x9F\x93\x8B"          /* U+1F4CB */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_name;
static lv_obj_t *s_meta;
static lv_obj_t *s_match;
static lv_obj_t *s_tags;
static lv_obj_t *s_diag_box;
static lv_obj_t *s_diag_icon;
static lv_obj_t *s_diag_title;
static lv_obj_t *s_diag_desc;
static lv_obj_t *s_advice;
static lv_obj_t *s_diary_btn_label;
static char      s_keep_name[96];   /* 保存本次识别结果，供"存日记"用 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void diag_on_back(lv_event_t *e)
{
  (void)e;
  ui_app_pop_screen();
}

/* 把这次识别结果写进成长日记（真实写盘，非演示） */

static void diag_on_save_diary(lv_event_t *e)
{
  struct record_diary_s d;

  (void)e;

  memset(&d, 0, sizeof(d));
  d.color = 0x22c55e;
  d.day_offset = 0;
  snprintf(d.title, sizeof(d.title), "拍到了%s",
           s_keep_name[0] != '\0' ? s_keep_name : "一株植物");
  snprintf(d.detail, sizeof(d.detail), "AI识别记录");

  record_diary_add(&d);
  record_diary_save();

  if (s_diary_btn_label != NULL)
    {
      lv_label_set_text(s_diary_btn_label, IC_OK " 已存进成长日记");
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_diagnose_create(void)
{
  lv_obj_t *scr = lv_obj_create(lv_screen_active());
  lv_obj_t *top;
  lv_obj_t *card;
  lv_obj_t *diag;
  lv_obj_t *act;
  lv_obj_t *go;
  lv_obj_t *box;
  lv_obj_t *l;

  s_keep_name[0] = '\0';

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, TP_SCREEN_W, TP_SCREEN_H);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  theme_page_bg(scr, TP_CLR_RESULT_BG_A, TP_CLR_RESULT_BG_B);

  /* ── 顶栏（26）：← 识别结果 ─────────────────────────────────── */

  top = lv_obj_create(scr);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, TP_SCREEN_W, TP_TOP_BAR_H);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_style_pad_hor(top, 8, 0);
  lv_obj_set_style_pad_column(top, 4, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  {
    lv_obj_t *back = lv_button_create(top);

    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 34, 24);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);

    l = lv_label_create(back);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(l, TP_FONT_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_GREEN_DARK), 0);
    lv_obj_center(l);

    lv_obj_add_event_cb(back, diag_on_back, LV_EVENT_CLICKED, NULL);
  }

  l = lv_label_create(top);
  lv_label_set_text(l, "识别结果");
  lv_obj_set_style_text_font(l, TP_FONT_TITLE, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_GREEN_DEEP), 0);

  /* ── 植物卡（58） ───────────────────────────────────────────── */

  card = lv_obj_create(scr);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, TP_SCREEN_W - 16, 58);
  lv_obj_set_pos(card, 8, 29);
  lv_obj_set_style_bg_color(card, lv_color_hex(PAL_PAPER), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 7, 0);
  lv_obj_set_style_pad_column(card, 7, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  {
    pal_face(card,0,0,40,40);
  }

  {
    lv_obj_t *info = lv_obj_create(card);

    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    s_name = lv_label_create(info);
    lv_label_set_text(s_name, "识别中");
    lv_obj_set_style_text_font(s_name, TP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_name, lv_color_hex(TP_CLR_GREEN_DEEP), 0);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_name, LV_PCT(100));

    s_meta = lv_label_create(info);
    lv_label_set_text(s_meta, "");
    lv_obj_set_style_text_font(s_meta, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_meta, lv_color_hex(TP_CLR_TEXT_MUTED), 0);
    lv_label_set_long_mode(s_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_meta, LV_PCT(100));

    s_tags = lv_label_create(info);
    lv_label_set_text(s_tags, "");
    lv_obj_set_style_text_font(s_tags, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_tags, lv_color_hex(TP_CLR_GREEN), 0);
    lv_label_set_long_mode(s_tags, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_tags, LV_PCT(100));
  }

  {
    lv_obj_t *conf = lv_obj_create(card);

    lv_obj_remove_style_all(conf);
    lv_obj_set_size(conf, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(conf, 1);
    lv_obj_set_flex_flow(conf, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(conf, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    s_match = lv_label_create(conf);
    lv_label_set_text(s_match, "--");
    lv_obj_set_style_text_font(s_match, TP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_match, lv_color_hex(TP_CLR_GREEN), 0);

    l = lv_label_create(conf);
    lv_label_set_text(l, "匹配");
    lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_GREEN), 0);
  }

  /* ── 诊断条（48）：始终可见，有/无问题换配色与图标 ─────────── */

  diag = lv_obj_create(scr);
  lv_obj_remove_style_all(diag);
  lv_obj_set_size(diag, TP_SCREEN_W - 16, 48);
  lv_obj_set_pos(diag, 8, 91);
  lv_obj_set_style_bg_color(diag, lv_color_hex(PAL_YELLOW), 0);
  lv_obj_set_style_bg_opa(diag, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(diag, 10, 0);
  lv_obj_set_style_border_width(diag, 0, 0);
  lv_obj_set_style_border_side(diag, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_pad_hor(diag, 8, 0);
  lv_obj_set_style_pad_column(diag, 6, 0);
  lv_obj_set_flex_flow(diag, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(diag, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  s_diag_icon = lv_label_create(diag);
  lv_label_set_text(s_diag_icon, IC_WARN);
  lv_obj_set_style_text_font(s_diag_icon, TP_ICON_M, 0);

  {
    lv_obj_t *txt = lv_obj_create(diag);

    lv_obj_remove_style_all(txt);
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(txt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    s_diag_title = lv_label_create(txt);
    lv_label_set_text(s_diag_title, "等待识别");
    lv_obj_set_style_text_font(s_diag_title, TP_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_diag_title, lv_color_hex(TP_CLR_AMBER_TEXT), 0);
    lv_label_set_long_mode(s_diag_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_diag_title, LV_PCT(100));

    s_diag_desc = lv_label_create(txt);
    lv_label_set_text(s_diag_desc, "");
    lv_obj_set_style_text_font(s_diag_desc, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_diag_desc, lv_color_hex(TP_CLR_BROWN_TEXT), 0);
    lv_label_set_long_mode(s_diag_desc, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_diag_desc, LV_PCT(100));
  }

  s_diag_box = diag;

  /* ── 建议卡（48） ───────────────────────────────────────────── */

  act = lv_obj_create(scr);
  lv_obj_remove_style_all(act);
  lv_obj_set_size(act, TP_SCREEN_W - 16, 48);
  lv_obj_set_pos(act, 8, 143);
  lv_obj_set_style_bg_color(act, lv_color_hex(PAL_BG), 0);
  lv_obj_set_style_bg_opa(act, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(act, 10, 0);
  lv_obj_set_style_border_width(act, 0, 0);
  lv_obj_set_style_pad_all(act, 5, 0);
  lv_obj_set_style_pad_row(act, 2, 0);
  lv_obj_set_flex_flow(act, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(act, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  l = lv_label_create(act);
  lv_label_set_text(l, IC_BULB " 小绿建议");
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(PAL_FG), 0);

  box = lv_obj_create(act);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(box, lv_color_hex(PAL_PAPER), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_60, 0);
  lv_obj_set_style_radius(box, 8, 0);
  lv_obj_set_style_pad_all(box, 3, 0);

  s_advice = lv_label_create(box);
  lv_label_set_text(s_advice, "");
  lv_obj_set_style_text_font(s_advice, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(s_advice, lv_color_hex(PAL_FG), 0);
  lv_label_set_long_mode(s_advice, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_advice, LV_PCT(100));

  /* ── 底部按钮（36）：把这株植物存进成长日记 ─────────────────── */

  go = lv_button_create(scr);
  lv_obj_remove_style_all(go);
  lv_obj_set_size(go, TP_SCREEN_W - 16, 36);
  lv_obj_set_pos(go, 8, 195);
  lv_obj_set_style_bg_color(go, lv_color_hex(PAL_YELLOW), 0);
  lv_obj_set_style_bg_opa(go, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(go, lv_color_hex(PAL_YELLOW), 0);
  lv_obj_set_style_bg_grad_dir(go, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_radius(go, 9, 0);
  lv_obj_set_style_pad_all(go, 0, 0);
  lv_obj_set_flex_flow(go, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(go, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  s_diary_btn_label = lv_label_create(go);
  lv_label_set_text(s_diary_btn_label, IC_CLIP " 存进成长日记");
  lv_obj_set_style_text_font(s_diary_btn_label, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(s_diary_btn_label, lv_color_hex(PAL_FG), 0);

  lv_obj_add_event_cb(go, diag_on_save_diary, LV_EVENT_CLICKED, NULL);

  return scr;
}

void screen_diagnose_set(lv_obj_t *scr,
                         const char *name, const char *latin,
                         int match, const char *tags,
                         const char *section_title,
                         bool issue, const char *issue_desc,
                         const char *advice)
{
  char buf[192];
  (void)scr;

  if (s_name != NULL)
    {
      zh_text_sanitize(s_keep_name, sizeof(s_keep_name),
                       name != NULL ? name : "未知");
      zh_label_set_text_safe(s_name, s_keep_name);
    }

  if (s_meta != NULL)
    {
      zh_label_set_text_safe(s_meta, latin != NULL ? latin : "");
    }

  if (s_match != NULL)
    {
      if (match > 0)
        {
          snprintf(buf, sizeof(buf), "%d%%", match);
        }
      else
        {
          snprintf(buf, sizeof(buf), "--");
        }

      lv_label_set_text(s_match, buf);
    }

  if (s_tags != NULL)
    {
      zh_label_set_text_safe(s_tags, tags != NULL ? tags : "");
    }

  /* 诊断条：有问题 → 琥珀 + ⚠；没问题 → 浅绿 + ✅ */

  if (s_diag_box != NULL)
    {
      lv_obj_set_style_bg_color(s_diag_box,
                                lv_color_hex(issue ? 0xfef3c7 : 0xdcfce7), 0);
    }

  if (s_diag_icon != NULL)
    {
      lv_label_set_text(s_diag_icon, issue ? IC_WARN : IC_OK);
      lv_obj_set_style_text_font(s_diag_icon,
                                 issue ? TP_ICON_M : TP_FONT_CAPTION, 0);
    }

  if (s_diag_title != NULL)
    {
      const char *title = (section_title != NULL && section_title[0] != '\0')
                          ? section_title
                          : (issue ? "发现小问题" : "看起来还不错");

      if (s_diag_icon != NULL && !issue)
        {
          snprintf(buf, sizeof(buf), "%s", title);
        }
      else
        {
          snprintf(buf, sizeof(buf), "%s%s", IC_WARN, title);
        }

      zh_label_set_text_safe(s_diag_title, buf);
      lv_obj_set_style_text_color(s_diag_title,
                                  lv_color_hex(issue ? TP_CLR_AMBER_TEXT
                                                     : TP_CLR_GREEN_DEEP), 0);
    }

  if (s_diag_desc != NULL)
    {
      zh_label_set_text_safe(s_diag_desc,
                             issue_desc != NULL ? issue_desc : "");
      lv_obj_set_style_text_color(s_diag_desc,
                                  lv_color_hex(issue ? TP_CLR_BROWN_TEXT
                                                     : 0x166534), 0);
    }

  if (s_advice != NULL)
    {
      zh_label_set_text_safe(s_advice, advice != NULL ? advice : "");
    }
}
