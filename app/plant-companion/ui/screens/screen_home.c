/****************************************************************************
 * apps/plant-companion/ui/screens/screen_home.c
 *
 * (1) 主界面 —— 对齐《嵌入式 UI V3 大字版》
 *   问候行 28 + 植物卡 76 + 4 传感器 60 + AI 提示 32 + 3 快捷键 56 = 252
 *   + 底部 Tab 52 = 304（屏 320，留 16 余量）
 *
 * 字号：12（辅助标签）/ 16（正文）/ 19（植物名、数值）/ 25（未用）
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>

#include "screen_home.h"
#include "screen_camera.h"
#include "screen_voice.h"
#include "../ui_app.h"
#include "../theme/theme_plant.h"
#include "../widgets/widget_bottomnav.h"
#include "../../services/plant_state.h"
#include "../../services/record_service.h"

#ifdef CONFIG_ESP32S3_WIFI
#  include "screen_ota.h"
#endif

#ifdef CONFIG_PLANT_WIFI_MANAGER
#  include "screen_wifi.h"
#endif

/* 图标（UTF-8 字节，避免源码里嵌 4 字节字符导致工具链不一致） */

#define IC_SUN      "\xE2\x98\x80"              /* U+2600  */
#define IC_LEAF     "\xF0\x9F\x8C\xBF"          /* U+1F33F */
#define IC_WATER    "\xF0\x9F\x92\xA7"          /* U+1F4A7 */
#define IC_TEMP     "\xF0\x9F\x8C\xA1"          /* U+1F321 */
#define IC_TUBE     "\xF0\x9F\xA7\xAA"          /* U+1F9EA */
#define IC_BULB     "\xF0\x9F\x92\xA1"          /* U+1F4A1 */
#define IC_CAMERA   "\xF0\x9F\x93\xB7"          /* U+1F4F7 */
#define IC_MIC      "\xF0\x9F\x8E\xA4"          /* U+1F3A4 */
#define IC_HAPPY    "\xF0\x9F\x98\x8A"          /* U+1F60A */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_arc;
static lv_obj_t *s_arc_val;
static lv_obj_t *s_v_moist;
static lv_obj_t *s_v_temp;
static lv_obj_t *s_v_ph;
static lv_obj_t *s_v_ec;
static lv_obj_t *s_hint;
static lv_obj_t *s_name;
static lv_obj_t *s_meta;
static lv_obj_t *s_day;
static lv_obj_t *s_st_time;         /* 顶栏：时间 "HH:MM" */
static lv_obj_t *s_st_date;         /* 顶栏：日期 "M月D日" */
static lv_obj_t *s_st_net;          /* 顶栏：网络状态 */
static lv_obj_t *s_st_bat;          /* 顶栏：电池电量 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 传感器小格：图标 + 大数值 + 标签 */

static lv_obj_t *home_cell(lv_obj_t *parent, const char *icon,
                           const char *val, const char *label,
                           uint32_t val_color)
{
  lv_obj_t *cell = lv_obj_create(parent);
  lv_obj_t *l;

  lv_obj_remove_style_all(cell);
  lv_obj_set_height(cell, 60);
  lv_obj_set_flex_grow(cell, 1);
  lv_obj_set_style_bg_color(cell, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(cell, LV_OPA_90, 0);
  lv_obj_set_style_radius(cell, 12, 0);
  lv_obj_set_style_pad_all(cell, 2, 0);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  l = lv_label_create(cell);
  lv_label_set_text(l, icon);
  lv_obj_set_style_text_font(l, TP_ICON_S, 0);

  l = lv_label_create(cell);
  lv_label_set_text(l, val);
  lv_obj_set_style_text_font(l, TP_FONT_TITLE, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(val_color), 0);

  l = lv_label_create(cell);
  lv_label_set_text(l, label);
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_TEXT_SUB), 0);

  return cell;
}

static lv_obj_t *home_cell_value(lv_obj_t *cell)
{
  return (lv_obj_t *)lv_obj_get_child(cell, 1);
}

/* 快捷入口：拍照 → (2) 拍照页；说话 → (6) 语音页 */

static void home_on_camera(lv_event_t *e)
{
  (void)e;
  ui_app_push_screen(screen_camera_create());
}

static void home_on_voice(lv_event_t *e)
{
  (void)e;
  ui_app_push_screen(screen_voice_create());
}

/* 浇水：本期无执行器（预留），按"记录一次浇水"处理 —— 写一条日记 +
 * 提示条确认，让用户看到真实反馈。 */

static void home_on_water(lv_event_t *e)
{
  struct record_diary_s d;
  (void)e;

  memset(&d, 0, sizeof(d));
  d.color = 0x3b82f6;
  d.day_offset = 0;
  snprintf(d.title, sizeof(d.title), "浇了200ml水");
  snprintf(d.detail, sizeof(d.detail), "手动记录");

  record_diary_add(&d);
  record_diary_save();

  if (s_hint != NULL)
    {
      lv_label_set_text(s_hint, IC_WATER " 已记下这次浇水，日记里能看到啦");
    }
}

#ifdef CONFIG_ESP32S3_WIFI
static void home_on_ota(lv_event_t *e)
{
  (void)e;
  ui_app_push_screen(screen_ota_create(false));
}
#endif

#ifdef CONFIG_PLANT_WIFI_MANAGER
/* 网络设置入口：扫 SSID / 自己输密码连接（换环境不用重刷固件） */

static void home_on_wifi(lv_event_t *e)
{
  (void)e;
  ui_app_push_screen(screen_wifi_create());
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_home_create(plant_nav_cb_t cb, void *user_data)
{
  lv_obj_t *scr = lv_obj_create(lv_screen_active());
  lv_obj_t *top;
  lv_obj_t *hero;
  lv_obj_t *row;
  lv_obj_t *qa;
  lv_obj_t *l;
#if defined(CONFIG_ESP32S3_WIFI) || defined(CONFIG_PLANT_WIFI_MANAGER)
  lv_obj_t *st_btns;
#endif
  struct plant_state_s ps;
  char buf[64];
  int i;

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 320);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  theme_page_bg(scr, TP_CLR_HOME_BG_A, TP_CLR_HOME_BG_B);

  plant_state_get(&ps);

  /* ── 问候行（28） ───────────────────────────────────────────── */

  top = lv_obj_create(scr);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, 480, 28);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_style_pad_hor(top, 12, 0);
  lv_obj_set_style_pad_right(top, 68, 0);   /* 给右上角按钮组留位 */
  lv_obj_set_style_pad_column(top, 6, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  {
    lv_obj_t *av = lv_obj_create(top);

    lv_obj_remove_style_all(av);
    lv_obj_set_size(av, 22, 22);
    lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(av, lv_color_hex(0xfbbf24), 0);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);

    l = lv_label_create(av);
    lv_label_set_text(l, IC_SUN);
    lv_obj_set_style_text_font(l, TP_ICON_S, 0);
    lv_obj_center(l);
  }

  l = lv_label_create(top);
  lv_label_set_text(l, "早安小绿");
  lv_obj_set_style_text_font(l, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_GREEN_DEEP), 0);

  s_day = lv_label_create(top);
  snprintf(buf, sizeof(buf), "第%d天", ps.plant_day > 0 ? ps.plant_day : 1);
  lv_label_set_text(s_day, buf);
  lv_obj_set_style_text_font(s_day, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(s_day, lv_color_hex(0x65a30d), 0);

  {
    lv_obj_t *sp = lv_obj_create(top);

    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 4, 4);
    lv_obj_set_flex_grow(sp, 1);
  }

  /* 顶栏状态（右）：时间 / 日期 / 网络 / 电量。

   * 全部真实值，由 screen_home_set_status() 刷新：时间来自服务器校时后
   * 的本地走时，网络来自 wifi_manager_is_connected()，电量来自电池 ADC。
   * 未取到就显示 "--" —— 不拿假值占位。
   * 原「☀ 26°」天气丸是写死的假数据，已删除（真实温度见下方温度卡）。 */

  s_st_time = lv_label_create(top);
  lv_label_set_text(s_st_time, "--:--");
  lv_obj_set_style_text_font(s_st_time, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(s_st_time, lv_color_hex(TP_CLR_GREEN_DEEP), 0);

  s_st_date = lv_label_create(top);
  lv_label_set_text(s_st_date, "");
  lv_obj_set_style_text_font(s_st_date, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(s_st_date, lv_color_hex(TP_CLR_TEXT_SUB), 0);

  s_st_net = lv_label_create(top);
  lv_label_set_text(s_st_net, "未联网");
  lv_obj_set_style_text_font(s_st_net, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(s_st_net, lv_color_hex(TP_CLR_TEXT_MUTED), 0);

  s_st_bat = lv_label_create(top);
  lv_label_set_text(s_st_bat, "电池 --%");
  lv_obj_set_style_text_font(s_st_bat, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(s_st_bat, lv_color_hex(TP_CLR_TEXT_SUB), 0);

#if defined(CONFIG_ESP32S3_WIFI) || defined(CONFIG_PLANT_WIFI_MANAGER)
  /* 右上角图标按钮组（OTA 刷新 + 网络设置）。
   *
   * 不能用 top 的 flex 流来排这两个键：顶栏状态文字（时间/日期/电量）长度
   * 随真实数据变化，flex 会把按钮一路推着走 —— 实测同一块屏上 WiFi 圆钮曾在
   * x=406 与 x=434 两处出现，tap 坐标跟着飘就点不中。
   * 改成锚定右上角，位置恒定：容器 [480-12-50, 480-12] = 418..468，
   * OTA 键 418,3、网络键 446,3（各 22x22），中心即 429,14 / 457,14。 */

  st_btns = lv_obj_create(scr);
  lv_obj_remove_style_all(st_btns);
  lv_obj_set_size(st_btns, 50, 22);
  lv_obj_align(st_btns, LV_ALIGN_TOP_RIGHT, -12, 3);
  lv_obj_remove_flag(st_btns, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(st_btns, 0, 0);
  lv_obj_set_style_pad_column(st_btns, 6, 0);
  lv_obj_set_flex_flow(st_btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(st_btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
#endif

#ifdef CONFIG_ESP32S3_WIFI
  /* OTA 入口（V3 稿没有；保留一个不抢眼的刷新键，点按进检查更新） */

  {
    lv_obj_t *b = lv_button_create(st_btns);

    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 22, 22);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);

    l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_TEXT_MUTED), 0);
    lv_obj_center(l);

    lv_obj_add_event_cb(b, home_on_ota, LV_EVENT_CLICKED, NULL);
  }
#endif

#ifdef CONFIG_PLANT_WIFI_MANAGER
  /* 网络设置入口（V3 稿没有；22x22 圆钮，与刷新键并排在最右） */

  {
    lv_obj_t *b = lv_button_create(st_btns);

    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 22, 22);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);

    l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_GREEN), 0);
    lv_obj_center(l);

    lv_obj_add_event_cb(b, home_on_wifi, LV_EVENT_CLICKED, NULL);
  }
#endif

  /* ── 植物主卡（76） ─────────────────────────────────────────── */

  hero = lv_obj_create(scr);
  lv_obj_remove_style_all(hero);
  lv_obj_set_size(hero, 456, 76);
  lv_obj_set_pos(hero, 12, 30);
  lv_obj_set_style_bg_color(hero, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(hero, lv_color_hex(0xf0fdf4), 0);
  lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_radius(hero, 16, 0);
  lv_obj_set_style_border_width(hero, 2, 0);
  lv_obj_set_style_border_color(hero, lv_color_hex(0xdef7e8), 0);
  lv_obj_set_style_pad_all(hero, 8, 0);
  lv_obj_set_style_pad_column(hero, 10, 0);
  lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  {
    lv_obj_t *av = lv_obj_create(hero);

    lv_obj_remove_style_all(av);
    lv_obj_set_size(av, 56, 56);
    lv_obj_set_style_radius(av, 14, 0);
    lv_obj_set_style_bg_color(av, lv_color_hex(0x86efac), 0);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(av, lv_color_hex(0x4ade80), 0);
    lv_obj_set_style_bg_grad_dir(av, LV_GRAD_DIR_VER, 0);

    l = lv_label_create(av);
    lv_label_set_text(l, IC_LEAF);
    lv_obj_set_style_text_font(l, TP_ICON_XL, 0);
    lv_obj_center(l);
  }

  {
    lv_obj_t *info = lv_obj_create(hero);

    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    s_name = lv_label_create(info);
    lv_label_set_text(s_name, ps.name[0] != '\0' ? ps.name : "小绿绿");
    lv_obj_set_style_text_font(s_name, TP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_name, lv_color_hex(TP_CLR_GREEN_DEEP), 0);

    s_meta = lv_label_create(info);
    snprintf(buf, sizeof(buf), "%s · 客厅窗台",
             ps.species[0] != '\0' ? ps.species : "绿萝");
    lv_label_set_text(s_meta, buf);
    lv_obj_set_style_text_font(s_meta, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_meta, lv_color_hex(TP_CLR_GREEN), 0);

    l = lv_label_create(info);
    lv_label_set_text(l, IC_HAPPY "\x20心情不错");
    lv_obj_set_style_bg_color(l, lv_color_hex(0xfef3c7), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 8, 0);
    lv_obj_set_style_pad_hor(l, 7, 0);
    lv_obj_set_style_pad_ver(l, 1, 0);
    lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x92400e), 0);
  }

  {
    lv_obj_t *sp = lv_obj_create(hero);

    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 4, 4);
    lv_obj_set_flex_grow(sp, 1);
  }

  /* 健康分进度环（46） */

  {
    lv_obj_t *ring = lv_obj_create(hero);

    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 46, 46);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);

    s_arc = lv_arc_create(ring);
    lv_obj_set_size(s_arc, 46, 46);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_value(s_arc, ps.health_score > 0 ? ps.health_score : 80);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xdcfce7), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(TP_CLR_GREEN), LV_PART_INDICATOR);

    s_arc_val = lv_label_create(ring);
    snprintf(buf, sizeof(buf), "%d", ps.health_score > 0 ? ps.health_score : 80);
    lv_label_set_text(s_arc_val, buf);
    lv_obj_set_style_text_font(s_arc_val, TP_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_arc_val, lv_color_hex(TP_CLR_GREEN), 0);
    lv_obj_center(s_arc_val);
  }

  /* ── 4 传感器小格（60） ─────────────────────────────────────── */

  row = lv_obj_create(scr);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 456, 60);
  lv_obj_set_pos(row, 12, 110);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  /* 四格初值一律 "--"（开机到第一次刷新之间不再闪假数据：
   * 原写死 62% / 24° / 6.5 / 1.2，看着像真读数）。 */

  s_v_moist = home_cell_value(
    home_cell(row, IC_WATER, "--", "水分", TP_CLR_GREEN));
  s_v_temp = home_cell_value(
    home_cell(row, IC_TEMP, "--", "温度", TP_CLR_GREEN));
  s_v_ph = home_cell_value(
    home_cell(row, IC_LEAF, "--", "pH", TP_CLR_GREEN));
  s_v_ec = home_cell_value(
    home_cell(row, IC_TUBE, "--", "EC", TP_CLR_GREEN));

  (void)i;

  /* ── AI 提示条（32） ────────────────────────────────────────── */

  s_hint = lv_label_create(scr);
  lv_label_set_text(s_hint, IC_BULB " AI建议：今天搬窗边晒晒太阳");
  lv_obj_set_size(s_hint, 456, 32);
  lv_obj_set_pos(s_hint, 12, 174);
  lv_obj_set_style_bg_color(s_hint, lv_color_hex(0xfef3c7), 0);
  lv_obj_set_style_bg_opa(s_hint, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(s_hint, lv_color_hex(0xfde68a), 0);
  lv_obj_set_style_bg_grad_dir(s_hint, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_radius(s_hint, 12, 0);
  lv_obj_set_style_pad_hor(s_hint, 10, 0);
  lv_obj_set_style_text_font(s_hint, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(s_hint, lv_color_hex(0x92400e), 0);
  lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_hint, LV_LABEL_LONG_CLIP);

  /* ── 3 个快捷键（56） ───────────────────────────────────────── */

  qa = lv_obj_create(scr);
  lv_obj_remove_style_all(qa);
  lv_obj_set_size(qa, 456, 56);
  lv_obj_set_pos(qa, 12, 210);
  lv_obj_set_style_pad_column(qa, 8, 0);
  lv_obj_set_flex_flow(qa, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(qa, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  {
    struct
    {
      const char *icon;
      const char *text;
      uint32_t    c1;
      uint32_t    c2;
      lv_event_cb_t fn;
    } qa_def[3] =
    {
      { IC_CAMERA, "拍照", 0x34d399, 0x10b981, home_on_camera },
      { IC_WATER,  "浇水", 0x60a5fa, 0x3b82f6, home_on_water  },
      { IC_MIC,    "说话", 0xf472b6, 0xec4899, home_on_voice  },
    };

    for (i = 0; i < 3; i++)
      {
        lv_obj_t *b = lv_button_create(qa);

        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, 56);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, lv_color_hex(qa_def[i].c1), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(qa_def[i].c2), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        l = lv_label_create(b);
        lv_label_set_text(l, qa_def[i].icon);
        lv_obj_set_style_text_font(l, TP_ICON_M, 0);

        l = lv_label_create(b);
        lv_label_set_text(l, qa_def[i].text);
        lv_obj_set_style_text_font(l, TP_FONT_BODY, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);

        lv_obj_add_event_cb(b, qa_def[i].fn, LV_EVENT_CLICKED, NULL);
      }
  }

  /* ── 底部 Tab（52） ─────────────────────────────────────────── */

  widget_bottomnav_create(scr, cb, user_data, PLANT_TAB_HOME,
                          PLANT_NAV_GREEN, false);

  return scr;
}

void screen_home_refresh(int health, int moisture, int temp, int ph_x10, int ec,
                         int valid)
{
  char buf[16];

  if (s_arc != NULL && s_arc_val != NULL)
    {
      if (health < 0)
        {
          health = 0;
        }
      else if (health > 100)
        {
          health = 100;
        }

      lv_arc_set_value(s_arc, health);
      snprintf(buf, sizeof(buf), "%d", health);
      lv_label_set_text(s_arc_val, buf);
    }

  /* 没有真实读数时一律 "--"，不拿 0 当数据展示 */

  if (s_v_moist != NULL)
    {
      if (valid)
        {
          snprintf(buf, sizeof(buf), "%d%%", moisture);
          lv_label_set_text(s_v_moist, buf);
        }
      else
        {
          lv_label_set_text(s_v_moist, "--");
        }
    }

  if (s_v_temp != NULL)
    {
      if (valid)
        {
          snprintf(buf, sizeof(buf), "%d°", temp);
          lv_label_set_text(s_v_temp, buf);
        }
      else
        {
          lv_label_set_text(s_v_temp, "--");
        }
    }

  if (s_v_ph != NULL)
    {
      /* 真实 pH（pH×10 传参，保一位小数）；5.5~7.5 显示绿色，出界琥珀色 */

      if (valid)
        {
          snprintf(buf, sizeof(buf), "%.1f", (float)ph_x10 / 10.0f);
          lv_label_set_text(s_v_ph, buf);
          lv_obj_set_style_text_color(s_v_ph,
                                      lv_color_hex((ph_x10 >= 55 && ph_x10 <= 75) ?
                                                   TP_CLR_GREEN : TP_CLR_AMBER), 0);
        }
      else
        {
          lv_label_set_text(s_v_ph, "--");
        }
    }

  if (s_v_ec != NULL)
    {
      if (valid)
        {
          /* EC 单位 uS/cm，与数据页同一口径。

           * 旧的显示逻辑是"收到值再 /10、按 0.1 mS/cm 打一位小数"，而调用
           * 方已经先 /100，两级缩放叠加后 11 uS/cm 被算成 0.0 —— 屏幕上就
           * 是恒定不变的一个 0。改为直接按 uS/cm 整数显示。 */

          snprintf(buf, sizeof(buf), "%d", ec);
          lv_label_set_text(s_v_ec, buf);
        }
      else
        {
          lv_label_set_text(s_v_ec, "--");
        }
    }
}

void screen_home_set_status(const char *time_str, const char *date_str,
                            int online, int battery_pct)
{
  char buf[24];

  if (s_st_time != NULL)
    {
      lv_label_set_text(s_st_time,
                        (time_str != NULL && time_str[0] != '\0') ?
                        time_str : "--:--");
    }

  if (s_st_date != NULL)
    {
      lv_label_set_text(s_st_date, (date_str != NULL) ? date_str : "");
    }

  if (s_st_net != NULL)
    {
      lv_label_set_text(s_st_net, online ? "已联网" : "未联网");
      lv_obj_set_style_text_color(
        s_st_net, lv_color_hex(online ? TP_CLR_GREEN : TP_CLR_TEXT_MUTED), 0);
    }

  if (s_st_bat != NULL)
    {
      if (battery_pct < 0)
        {
          lv_label_set_text(s_st_bat, "电池 --%");
        }
      else
        {
          snprintf(buf, sizeof(buf), "电池 %d%%", battery_pct);
          lv_label_set_text(s_st_bat, buf);
        }
    }
}
