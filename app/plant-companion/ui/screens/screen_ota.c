/****************************************************************************
 * apps/plant-companion/ui/screens/screen_ota.c
 *
 * OTA 升级页（阶段 A）：
 *   - 普通模式：进入即起 worker 检查更新（ota_service_check），
 *     ui_task 定时器轮询结果上屏；有新版本 → 「立即升级」（worker 跑
 *     ota_check_update，阻塞下载→校验→写 otadata→自动重启），期间
 *     显示下载进度（%）。
 *   - 升级后模式：OTA 重启后 PENDING_VERIFY 自动弹出，手动确认
 *     （ota_confirm 标 VALID）保持崩溃自动回退保护；不确认则下次
 *     重启自动回退（页面上明确提示）。
 *
 * 铁律：所有 LVGL 对象操作都在 ui_task（本页创建/刷新全在 ui_task）；
 * worker 只写共享结构（ota_service）。页面销毁必须删定时器 + 置空
 * 静态指针 + abort worker（voice 页踩过 UAF，见 voice_on_delete）。
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>

#include "screen_ota.h"
#include "../ui_app.h"
#include "../theme/theme_plant.h"
#include "../../services/ota_service.h"
#include "../../ota/ota.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_status;    /* 主状态文字（大号） */
static lv_obj_t *s_sub;       /* 副文字（版本/错误/进度） */
static lv_obj_t *s_btn_up;    /* 主按钮（立即升级/重试/确认新版本） */
static lv_obj_t *s_btn_back;  /* 次按钮（返回/稍后再说） */
static lv_timer_t *s_timer;
static bool s_post_reboot;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void ota_on_back(lv_event_t *e)
{
  (void)e;
  ui_app_pop_screen();
}

static void ota_on_primary(lv_event_t *e)
{
  struct ota_ui_result_s r;
  (void)e;

  if (s_post_reboot)
    {
      /* 升级后确认页：标 VALID（取消自动回退），回首页 */

      ota_confirm();
      ui_app_pop_screen();
      return;
    }

  ota_service_get(&r);

  if (r.state == OTA_UI_AVAILABLE)
    {
      ota_service_upgrade();
    }
  else if (r.state == OTA_UI_ERROR)
    {
      ota_service_check();
    }
}

/* ⚠ 页面销毁必须清理（voice/camera 页踩过的 UAF）：删定时器 + 置空
 * 静态指针 + abort worker（检查可丢弃；下载无法打断，结果丢弃即可）。 */

static void ota_on_delete(lv_event_t *e)
{
  (void)e;

  ota_service_abort();

  if (s_timer != NULL)
    {
      lv_timer_delete(s_timer);
      s_timer = NULL;
    }

  s_status = NULL;
  s_sub = NULL;
  s_btn_up = NULL;
  s_btn_back = NULL;
}

/* 按钮显隐 + 文字（lv_obj_add/remove_flag） */

static void ota_show_btn(lv_obj_t *btn, const char *text)
{
  if (btn == NULL)
    {
      return;
    }

  if (text != NULL)
    {
      theme_btn_set_text(btn, text);
    }

  lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

static void ota_hide_btn(lv_obj_t *btn)
{
  if (btn != NULL)
    {
      lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ui_task 定时器：轮询 ota_service 结果刷新状态（仅普通模式） */

static void ota_ui_timer_cb(lv_timer_t *timer)
{
  struct ota_ui_result_s r;
  char buf[96];
  (void)timer;

  if (s_post_reboot || s_status == NULL)
    {
      return;   /* 确认页静态内容；或页面已销毁（定时器未及删除） */
    }

  ota_service_get(&r);

  if (r.state == OTA_UI_DOWNLOADING)
    {
      if (r.total > 0 && r.downloaded >= r.total)
        {
          lv_label_set_text(s_status, "下载完成，校验中…");
          lv_label_set_text(s_sub, "即将自动重启");
        }
      else if (r.total > 0)
        {
          int pct = (int)(r.downloaded * 100 / r.total);

          if (pct > 100)
            {
              pct = 100;
            }

          lv_label_set_text(s_status, "正在下载固件…");
          snprintf(buf, sizeof(buf), "%d%%（%d / %d KB）",
                   pct, (int)(r.downloaded / 1024),
                   (int)(r.total / 1024));
          lv_label_set_text(s_sub, buf);
        }
      else
        {
          lv_label_set_text(s_status, "正在下载固件…");
          lv_label_set_text(s_sub, "连接 OTA 服务器…");
        }

      ota_hide_btn(s_btn_up);
      ota_hide_btn(s_btn_back);
      return;
    }

  switch (r.state)
    {
      case OTA_UI_CHECKING:
        lv_label_set_text(s_status, "正在检查更新…");
        lv_label_set_text(s_sub, "请稍候");
        ota_hide_btn(s_btn_up);
        ota_hide_btn(s_btn_back);
        break;

      case OTA_UI_AVAILABLE:
        snprintf(buf, sizeof(buf), "发现新版本 v%d.%d.%d",
                 r.new_major, r.new_minor, r.new_patch);
        lv_label_set_text(s_status, buf);
        snprintf(buf, sizeof(buf), "当前版本 v%d.%d.%d，点「立即升级」开始",
                 r.current_major, r.current_minor, r.current_patch);
        lv_label_set_text(s_sub, buf);
        ota_show_btn(s_btn_up, "立即升级");
        ota_show_btn(s_btn_back, "返回");
        break;

      case OTA_UI_UP_TO_DATE:
        lv_label_set_text(s_status, "已是最新版本 ✅");
        snprintf(buf, sizeof(buf), "当前版本 v%d.%d.%d",
                 r.current_major, r.current_minor, r.current_patch);
        lv_label_set_text(s_sub, buf);
        ota_hide_btn(s_btn_up);
        ota_show_btn(s_btn_back, "返回");
        break;

      case OTA_UI_ERROR:
        lv_label_set_text(s_status, "检查失败");
        lv_label_set_text(s_sub, r.error[0] != '\0' ? r.error : "未知错误");
        ota_show_btn(s_btn_up, "重试");
        ota_show_btn(s_btn_back, "返回");
        break;

      default:
        break;
    }
}

/* 创建按钮（返回按钮复用 diagnose 页样式） */

static lv_obj_t *ota_make_back(lv_obj_t *scr)
{
  lv_obj_t *back = lv_button_create(scr);

  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, 96, TP_TOP_BAR_H);
  lv_obj_set_pos(back, 0, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x00000000), 0);
  lv_obj_set_style_text_color(back, lv_color_hex(TP_CLR_DATA_BLUE), 0);
  lv_obj_set_style_text_font(back, TP_FONT_TITLE, 0);
  theme_btn_set_text(back, LV_SYMBOL_LEFT " 返回");
  lv_obj_add_event_cb(back, ota_on_back, LV_EVENT_CLICKED, NULL);
  return back;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_ota_create(bool post_reboot)
{
  lv_obj_t *scr = lv_obj_create(lv_screen_active());
  lv_obj_t *title;
  lv_obj_t *body;
  lv_obj_t *btn_row;
  char buf[96];

  s_post_reboot = post_reboot;

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 320);
  lv_obj_set_style_bg_color(scr, lv_color_hex(TP_CLR_BG_GRAD_A), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(scr, lv_color_hex(TP_CLR_BG_GRAD_B), 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

  ota_make_back(scr);

  title = lv_label_create(scr);
  lv_label_set_text(title, post_reboot ? "升级完成" : "固件升级");
  lv_obj_set_style_text_font(title, TP_FONT_TITLE, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  /* 内容区 */

  body = lv_obj_create(scr);
  lv_obj_remove_style_all(body);
  lv_obj_set_pos(body, 0, TP_TOP_BAR_H);
  lv_obj_set_size(body, 480, 320 - TP_TOP_BAR_H - 64);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(body, TP_PAD_PAGE, 0);
  lv_obj_set_style_pad_row(body, 16, 0);

  s_status = lv_label_create(body);
  lv_label_set_text(s_status, post_reboot ? "已升级到新版本" : "正在检查更新…");
  lv_obj_set_style_text_font(s_status, TP_FONT_BIG, 0);
  lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);

  s_sub = lv_label_create(body);
  lv_label_set_text(s_sub, "");
  lv_obj_add_style(s_sub, &theme_style_text_sub, 0);
  lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_sub, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_sub, 448);

  if (post_reboot)
    {
      /* 升级后确认页（PENDING_VERIFY）：显示当前/上一版本 + 回退警告 */

      int cur_major;
      int cur_minor;
      int cur_patch;
      int prev_major;
      int prev_minor;
      int prev_patch;

      ota_get_current_version(&cur_major, &cur_minor, &cur_patch);
      snprintf(buf, sizeof(buf), "当前版本 v%d.%d.%d",
               cur_major, cur_minor, cur_patch);
      lv_label_set_text(s_status, buf);

      if (ota_read_prev_version(&prev_major, &prev_minor, &prev_patch) == 0)
        {
          snprintf(buf, sizeof(buf),
                   "上一版本 v%d.%d.%d\n\n"
                   "⚠ 不点「确认新版本」的话，下次重启将自动回退到上一版本。",
                   prev_major, prev_minor, prev_patch);
        }
      else
        {
          snprintf(buf, sizeof(buf),
                   "⚠ 不点「确认新版本」的话，下次重启将自动回退到上一版本。");
        }

      lv_label_set_text(s_sub, buf);
    }
  else
    {
      /* 普通模式：进入即自动检查更新（除非已有下载任务在跑，如
       * 页面销毁后重新进入时 worker 仍在下载） */

      struct ota_ui_result_s r;

      ota_service_get(&r);
      if (!r.busy)
        {
          ota_service_check();
        }
    }

  /* 按钮行 */

  btn_row = lv_obj_create(body);
  lv_obj_remove_style_all(btn_row);
  lv_obj_set_width(btn_row, LV_PCT(100));
  lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  s_btn_up = lv_button_create(btn_row);
  lv_obj_remove_style_all(s_btn_up);
  lv_obj_add_style(s_btn_up, &theme_style_btn_primary, 0);
  lv_obj_set_size(s_btn_up, 200, 44);
  theme_btn_set_text(s_btn_up, post_reboot ? "确认新版本" : "立即升级");
  lv_obj_add_event_cb(s_btn_up, ota_on_primary, LV_EVENT_CLICKED, NULL);

  s_btn_back = lv_button_create(btn_row);
  lv_obj_remove_style_all(s_btn_back);
  lv_obj_set_style_bg_color(s_btn_back, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(s_btn_back, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_btn_back, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_text_color(s_btn_back, lv_color_hex(TP_CLR_TEXT_MAIN), 0);
  lv_obj_set_style_text_font(s_btn_back, TP_FONT_TITLE, 0);
  lv_obj_set_style_pad_hor(s_btn_back, 24, 0);
  lv_obj_set_style_pad_ver(s_btn_back, 10, 0);
  lv_obj_set_size(s_btn_back, 200, 44);
  theme_btn_set_text(s_btn_back, post_reboot ? "稍后再说" : "返回");
  lv_obj_add_event_cb(s_btn_back, ota_on_back, LV_EVENT_CLICKED, NULL);

  if (post_reboot)
    {
      /* 确认页静态，不需要轮询定时器 */

      s_timer = NULL;
    }
  else
    {
      s_timer = lv_timer_create(ota_ui_timer_cb, 500, NULL);

      /* 首次渲染前隐藏按钮，等第一次轮询按状态显示（避免闪现） */

      ota_hide_btn(s_btn_up);
      ota_hide_btn(s_btn_back);
    }

  /* ⚠ 页面销毁必须删 timer + 置空指针（否则 UAF 崩溃），见 ota_on_delete */

  lv_obj_add_event_cb(scr, ota_on_delete, LV_EVENT_DELETE, NULL);

  return scr;
}
