/****************************************************************************
 * apps/plant-companion/ui/screens/screen_wifi.c
 *
 * 网络设置页（2026-09-11 用户需求：可以自己扫描 SSID、自己输密码连接，
 * 换个环境也能用）：
 *
 *   ① 一级页 screen_wifi_create()：进页起 worker 跑 wifi_manager_scan()
 *      （内部 `wapi scan wlan0`，阻塞约 5s）→ 附近网络列表（SSID / 加密 /
 *      信号强度）、底部按钮进"手动输入网络名"。
 *   ② 二级页 screen_wifi_pwd_create(ssid)：点列表某一项进入，
 *      lv_textarea（密码模式）+ lv_keyboard 输密码 → 「连接」起 worker 调
 *      wifi_manager_connect()（阻塞 20s+）→ UI 定时器轮询结果；
 *      成功即写 SD 配置（保留原服务器地址）并自动退回首页。
 *
 * 铁律（与 screen_ota 相同）：
 *   - LVGL 对象只在 ui_task 操作；worker 只写 volatile 共享状态；
 *   - 页面销毁必须删定时器 + 置空静态指针（voice/camera 页踩过 UAF）。
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include "screen_wifi.h"
#include "../ui_app.h"
#include "../theme/theme_plant.h"
#include "../../communication/wifi_manager/wifi_manager.h"

#ifdef CONFIG_PLANT_SD_CARD
#  include "../../components/system_monitor/device_cfg.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_POLL_MS      400     /* UI 轮询扫描/连接结果的周期 */
/* ⚠ 2026-09-17：8192 → 16384。
 *
 * 和 v1.3.9 修 ui_time_worker 是同一类问题：这个 worker 要跑
 * posix_spawn(wapi) + 文件读写 + printf，8KB 栈是刀尖上的余量，踩穿会
 * 直接砸坏堆 → 表现为"点 WiFi 图标之后整机静静死掉"。其它 worker
 * （ota_service/语音）都给的 16KB，这里对齐。 */

#define WIFI_THREAD_STACK 16384   /* 堆分配 worker 栈（同 ota_service） */
enum
{
  WSCAN_DONE = 0,   /* 扫描结束（可能有 0 条） */
  WSCAN_BUSY,       /* 扫描中 */
  WSCAN_FAIL        /* 扫描失败 */
};

/****************************************************************************
 * Private Data — 列表页 + 扫描
 ****************************************************************************/

static wifi_ap_t s_aps[WIFI_AP_MAX];
static volatile int  s_ap_count;
static volatile int  s_scan_state = WSCAN_DONE;
static volatile int  s_scan_gen;          /* 每完成一次扫描 +1 */
static volatile bool s_scan_live;         /* worker 是否还在跑 */

static lv_obj_t *s_list;                  /* 列表容器 */
static lv_obj_t *s_hint;                  /* 状态提示行 */
static lv_timer_t *s_timer;
static int s_shown_gen = -1;
static char s_cur_ssid[33];               /* 进页时已连接的 SSID（用于标记） */

/****************************************************************************
 * Private Functions — 列表页
 ****************************************************************************/

static const char *wifi_sig_word(int rssi)
{
  if (rssi >= -60)
    {
      return "强";
    }

  if (rssi >= -75)
    {
      return "中";
    }

  return "弱";
}

static uint32_t wifi_sig_color(int rssi)
{
  if (rssi >= -60)
    {
      return TP_CLR_STATE_OK;
    }

  if (rssi >= -75)
    {
      return TP_CLR_STATE_WARN;
    }

  return TP_CLR_STATE_BAD;
}

/* 行销毁时释放行私有的 SSID 副本 */

static void wifi_row_free_cb(lv_event_t *e)
{
  void *p = lv_event_get_user_data(e);

  if (p != NULL)
    {
      lv_free(p);
    }
}

static void wifi_row_cb(lv_event_t *e)
{
  const char *ssid = (const char *)lv_event_get_user_data(e);
  lv_obj_t *scr;

  if (ssid == NULL)
    {
      return;
    }

  scr = screen_wifi_pwd_create(ssid);
  if (scr != NULL)
    {
      ui_app_push_screen(scr);
    }
}

static void wifi_list_build(void)
{
  int i;

  if (s_list == NULL)
    {
      return;
    }

  lv_obj_clean(s_list);

  if (s_scan_state == WSCAN_BUSY)
    {
      lv_label_set_text(s_hint, "正在扫描附近网络…");
      return;
    }

  if (s_scan_state == WSCAN_FAIL)
    {
      lv_label_set_text(s_hint, "扫描失败，请重试");
      return;
    }

  if (s_ap_count <= 0)
    {
      lv_label_set_text(s_hint, "点下面按钮输入网络名和密码换网");
      return;
    }

  lv_label_set_text(s_hint, "点一下要连接的网络");

  for (i = 0; i < s_ap_count && i < WIFI_AP_MAX; i++)
    {
      wifi_ap_t *ap = &s_aps[i];
      lv_obj_t *row;
      lv_obj_t *name;
      lv_obj_t *info;
      char ibuf[64];
      char *copy;

      row = lv_obj_create(s_list);
      lv_obj_remove_style_all(row);
      lv_obj_add_style(row, &theme_style_card_small, 0);
      lv_obj_set_size(row, 440, 48);
      lv_obj_set_pos(row, 0, i * 54);
      lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_style_pad_all(row, 6, 0);
      lv_obj_set_style_pad_column(row, 8, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER);

      name = lv_label_create(row);
      if (strcmp(ap->ssid, s_cur_ssid) == 0)
        {
          lv_label_set_text_fmt(name, "%s（已连接）", ap->ssid);
        }
      else
        {
          lv_label_set_text(name, ap->ssid);
        }
      lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
      lv_obj_set_width(name, 200);
      lv_obj_set_style_text_font(name, TP_FONT_BODY, 0);
      lv_obj_set_style_text_color(name, lv_color_hex(TP_CLR_TEXT_MAIN), 0);

      info = lv_label_create(row);
      snprintf(ibuf, sizeof(ibuf), "%s 信号%s %d",
               ap->secure ? "加密" : "开放", wifi_sig_word(ap->rssi),
               ap->rssi);
      lv_label_set_text(info, ibuf);
      lv_obj_set_style_text_font(info, TP_FONT_CAPTION, 0);
      lv_obj_set_style_text_color(info, lv_color_hex(wifi_sig_color(ap->rssi)),
                                  0);

      /* 行私有 SSID 副本：扫描结果会被 worker 覆盖，不能把 s_aps 的
       * 指针直接挂到事件上。 */

      copy = (char *)lv_malloc(33);
      if (copy == NULL)
        {
          continue;
        }

      strncpy(copy, ap->ssid, 32);
      copy[32] = '\0';
      lv_obj_add_event_cb(row, wifi_row_cb, LV_EVENT_CLICKED, copy);
      lv_obj_add_event_cb(row, wifi_row_free_cb, LV_EVENT_DELETE, copy);
    }
}

static void *wifi_scan_worker(void *arg)
{
  int n;

  (void)arg;

  wifi_manager_init();
  n = wifi_manager_scan(s_aps, WIFI_AP_MAX);

  if (n >= 0)
    {
      s_ap_count = n;
      s_scan_state = WSCAN_DONE;
    }
  else
    {
      printf("[WifiUI] 扫描失败: %d\n", n);
      s_scan_state = WSCAN_FAIL;
    }

  s_scan_gen++;
  s_scan_live = false;
  return NULL;
}

static void wifi_scan_start(void)
{
  pthread_t tid;
  pthread_attr_t attr;

  if (s_scan_live)
    {
      return;
    }

  s_scan_live = true;
  s_scan_state = WSCAN_BUSY;
  s_scan_gen++;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, WIFI_THREAD_STACK);
  if (pthread_create(&tid, &attr, wifi_scan_worker, NULL) == 0)
    {
      pthread_detach(tid);
    }
  else
    {
      s_scan_live = false;
      s_scan_state = WSCAN_FAIL;
      s_scan_gen++;
    }

  pthread_attr_destroy(&attr);
}

static void wifi_on_back(lv_event_t *e)
{
  (void)e;
  ui_app_pop_screen();
}

static void wifi_on_rescan(lv_event_t *e)
{
  (void)e;
  wifi_scan_start();
}

/* 手动换网：直接进"输入网络名+密码"页（不再扫描 —— 实测本板
 * `wapi scan` 会把 esp32 的 STA 状态机搞坏，扫完再也连不上网，
 * 只能断电重启，代价比"扫不出列表"大得多）。 */

static void wifi_on_manual(lv_event_t *e)
{
  wifi_status_t st;

  (void)e;

  /* 预填当前已连的 SSID：多数情况只是重输密码，一眼能看到现在在哪个网，
   * 要换网就点「清空」把框清掉重新输。
   *
   * ⚠ 这里读的是 manager 里缓存的 g_ssid（纯内存），不要改成
   *   wifi_manager_get_essid() —— 那个内部要起 `wapi show` 子进程查驱动，
   *   实测跑完会把数据面打断（心跳 -110），而且在 UI 线程里要卡好几秒。 */

  memset(&st, 0, sizeof(st));

  if (wifi_manager_get_status(&st) != 0)
    {
      st.ssid[0] = '\0';
    }

  ui_app_push_screen(screen_wifi_pwd_create(st.ssid));
}

static void wifi_ui_timer_cb(lv_timer_t *timer)
{
  (void)timer;

  if (s_list == NULL)
    {
      return;
    }

  if (s_scan_gen != s_shown_gen)
    {
      s_shown_gen = s_scan_gen;
      wifi_list_build();
    }
}

static void wifi_on_delete(lv_event_t *e)
{
  (void)e;

  if (s_timer != NULL)
    {
      lv_timer_delete(s_timer);
      s_timer = NULL;
    }

  s_list = NULL;
  s_hint = NULL;
}

/****************************************************************************
 * Private Data — 密码页 + 连接
 ****************************************************************************/

static lv_obj_t *s_pwd_ta;
static lv_obj_t *s_pwd_btn_ok;
static lv_obj_t *s_pwd_btn_cancel;
static lv_obj_t *s_pwd_status;
static lv_timer_t *s_pwd_timer;
static char s_pwd_ssid[33];
static lv_obj_t *s_ssid_ta;           /* 网络名输入框（手动换网用） */
static int s_kb_field = 1;            /* 键盘写到哪个框：0=网络名 1=密码 */
static char s_pwd_secret[65];
static volatile int  s_conn_state;   /* 0=空闲 1=连接中 2=成功 3=失败 */
static volatile bool s_conn_live;
static bool s_eye_open;
static int  s_ok_ticks;

/****************************************************************************
 * Private Functions — 密码页
 ****************************************************************************/

static void *wifi_conn_worker(void *arg)
{
  int ret;

  (void)arg;

  wifi_manager_init();
  ret = wifi_manager_connect(s_pwd_ssid, s_pwd_secret);

  if (ret == 0 && wifi_manager_has_ip())
    {
#ifdef CONFIG_PLANT_SD_CARD
      /* 只更新 WiFi：先把 SD 上已有的服务器地址读回来一起写，
       * device_cfg_save 是整文件重写，不合并。 */

      char o_ssid[33] = { 0 };
      char o_pwd[65] = { 0 };
      char host[128] = { 0 };
      uint16_t port = 0;
      int rc;

      const char *save_pwd = s_pwd_secret;

      rc = device_cfg_load(o_ssid, sizeof(o_ssid), o_pwd, sizeof(o_pwd),
                           host, sizeof(host), &port);

      /* ⚠ 别用空密码把已存好的密码覆盖掉：如果连的是一个"本来就已经连着"
       * 的加密网络，就算密码留空也会显示连接成功（旧的关联还在），
       * 但写进去的空密码会让板子下次开机再也连不上。
       * 规则：同一个 SSID + 新密码为空 + 旧密码非空 → 保留旧密码。 */

      if (s_pwd_secret[0] == '\0' && rc == 0 && o_pwd[0] != '\0' &&
          strcmp(o_ssid, s_pwd_ssid) == 0)
        {
          save_pwd = o_pwd;
          printf("[WifiUI] 新密码为空且 SSID 未变，保留 SD 上的原密码\n");
        }

      if (rc < 0 || host[0] == '\0')
        {
          device_cfg_save(s_pwd_ssid, save_pwd, NULL, 0);
        }
      else
        {
          device_cfg_save(s_pwd_ssid, save_pwd, host, port);
        }

      printf("[WifiUI] 已保存 WiFi 配置到 /mnt/sd/plant.cfg\n");
#endif
      s_conn_state = 2;
    }
  else
    {
      printf("[WifiUI] 连接 %s 失败 ret=%d\n", s_pwd_ssid, ret);
      s_conn_state = 3;
    }

  s_conn_live = false;
  return NULL;
}

static void wifi_connect_start(void)
{
  pthread_t tid;
  pthread_attr_t attr;
  const char *txt;

  if (s_conn_live || s_pwd_ta == NULL)
    {
      return;
    }

  /* 网络名以输入框为准（手动换网时可以现改） */

  if (s_ssid_ta != NULL)
    {
      const char *s = lv_textarea_get_text(s_ssid_ta);

      while (s != NULL && *s == ' ')
        {
          s++;
        }

      if (s == NULL || *s == '\0')
        {
          printf("[WifiUI] 网络名为空，拒绝连接\n");

          if (s_pwd_status != NULL)
            {
              lv_label_set_text(s_pwd_status, "请先输入网络名（SSID）");
            }

          return;
        }

      strncpy(s_pwd_ssid, s, sizeof(s_pwd_ssid) - 1);
      s_pwd_ssid[sizeof(s_pwd_ssid) - 1] = '\0';
    }

  txt = lv_textarea_get_text(s_pwd_ta);
  if (txt == NULL)
    {
      txt = "";
    }

  strncpy(s_pwd_secret, txt, sizeof(s_pwd_secret) - 1);
  s_pwd_secret[sizeof(s_pwd_secret) - 1] = '\0';

  s_conn_state = 1;
  s_conn_live = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, WIFI_THREAD_STACK);
  if (pthread_create(&tid, &attr, wifi_conn_worker, NULL) == 0)
    {
      pthread_detach(tid);
    }
  else
    {
      s_conn_live = false;
      s_conn_state = 3;
    }

  pthread_attr_destroy(&attr);
}

static void wifi_pwd_enable(bool en)
{
  if (s_pwd_btn_ok != NULL)
    {
      if (en)
        {
          lv_obj_remove_flag(s_pwd_btn_ok, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(s_pwd_btn_ok, LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (s_pwd_btn_cancel != NULL)
    {
      if (en)
        {
          lv_obj_remove_flag(s_pwd_btn_cancel, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(s_pwd_btn_cancel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void wifi_pwd_on_ok(lv_event_t *e)
{
  (void)e;
  wifi_connect_start();
}

static void wifi_pwd_on_cancel(lv_event_t *e)
{
  (void)e;
  ui_app_pop_screen();
}

static void wifi_pwd_on_eye(lv_event_t *e)
{
  lv_obj_t *btn = lv_event_get_target_obj(e);
  lv_obj_t *l;

  if (s_pwd_ta == NULL)
    {
      return;
    }

  s_eye_open = !s_eye_open;
  lv_textarea_set_password_mode(s_pwd_ta, !s_eye_open);

  l = (btn != NULL) ? lv_obj_get_child(btn, 0) : NULL;
  if (l != NULL && lv_obj_check_type(l, &lv_label_class))
    {
      lv_label_set_text(l, s_eye_open ? LV_SYMBOL_EYE_CLOSE
                                      : LV_SYMBOL_EYE_OPEN);
    }
}

/****************************************************************************
 * 自绘软键盘
 *
 * ⚠ 为什么不用 lv_keyboard：本板 LVGL 没有套用主题样式（其它页面能看全靠
 * "显式配色"），而 lv_keyboard 的键帽底色/字色全部来自主题；LVGL 9 的
 * buttonmatrix 又不再生成子对象。实测结果就是屏幕 0,156 480x164 整块空白，
 * 用户根本没法输密码。这里改用工程内通用的 lv_button + 显式样式自绘一套，
 * 行为完全可控。
 ****************************************************************************/

#define WKB_ROWS    4
#define WKB_COLS    11
#define WKB_KEY_H   36
#define WKB_GAP     4
#define WKB_MARGIN  12
#define WKB_TOP     160
#define WKB_WIDE    456

enum
{
  WKB_LOWER = 0,
  WKB_UPPER,
  WKB_SYM
};

enum
{
  WKB_CHAR = 0,   /* 普通字符 */
  WKB_BACK,       /* 退格 */
  WKB_TOGGLE      /* 大小写/符号页 三态切换 */
};

typedef struct
{
  lv_obj_t *btn;
  lv_obj_t *lbl;
  uint8_t   kind;
  char      ch;
  char      txt[2];
} wkb_key_t;

static lv_obj_t *s_kb_box;
static wkb_key_t s_kb_keys[WKB_ROWS][WKB_COLS];
static int       s_kb_layout = WKB_LOWER;

/* 三页每行按钮数（第 0/1/2 行 10 键，第 3 行 11 键） */

static const uint8_t s_kb_rowlen[3][WKB_ROWS] =
{
  { 10, 10, 10, 11 },
  { 10, 10, 10, 11 },
  { 10, 10, 10, 11 }
};

static const char *const s_kb_row0[3] =
{
  "1234567890",
  "1234567890",
  "!@#$%^&*()"
};

static const char *const s_kb_row1[3] =
{
  "qwertyuiop",
  "QWERTYUIOP",
  "-_=+[]{}:;"
};

static const char *const s_kb_row2[3] =
{
  "asdfghjkl",
  "ASDFGHJKL",
  "~,./?;:'|"
};

/* 第 3 行固定：首键为切换键，其余为字母 + 三个常用符号 */

static const char WKB_ROW3[] = "zxcvbnm.-_";

static void wkb_apply_layout(void)
{
  static const char *const toggle_txt[3] =
  {
    LV_SYMBOL_UP, LV_SYMBOL_DOWN, "abc"
  };

  int r;
  int c;

  for (r = 0; r < WKB_ROWS; r++)
    {
      const char *chars;
      int n = (int)s_kb_rowlen[s_kb_layout][r];

      if (r == 0)
        {
          chars = s_kb_row0[s_kb_layout];
        }
      else if (r == 1)
        {
          chars = s_kb_row1[s_kb_layout];
        }
      else if (r == 2)
        {
          chars = s_kb_row2[s_kb_layout];
        }
      else
        {
          chars = WKB_ROW3;
        }

      for (c = 0; c < n; c++)
        {
          wkb_key_t *k = &s_kb_keys[r][c];
          int ci;

          if (k->btn == NULL || k->lbl == NULL)
            {
              continue;
            }

          if (r == 3 && c == 0)
            {
              k->kind = WKB_TOGGLE;
              k->ch = 0;
              lv_label_set_text(k->lbl, toggle_txt[s_kb_layout]);

              if (s_kb_layout == WKB_UPPER)
                {
                  lv_obj_set_style_bg_color(k->btn,
                                            lv_color_hex(TP_CLR_GREEN), 0);
                  lv_obj_set_style_text_color(k->lbl,
                                              lv_color_hex(0xffffff), 0);
                }
              else
                {
                  lv_obj_set_style_bg_color(k->btn,
                                            lv_color_hex(0xdfe7ee), 0);
                  lv_obj_set_style_text_color(k->lbl,
                                              lv_color_hex(TP_CLR_TEXT_MAIN),
                                              0);
                }

              continue;
            }

          ci = (r == 3) ? (c - 1) : c;

          if (r == 2 && c == n - 1)
            {
              k->kind = WKB_BACK;
              k->ch = 0;
              lv_label_set_text(k->lbl, LV_SYMBOL_BACKSPACE);
            }
          else
            {
              k->kind = WKB_CHAR;
              k->ch = chars[ci];
              k->txt[0] = chars[ci];
              k->txt[1] = '\0';
              lv_label_set_text(k->lbl, k->txt);
            }
        }
    }
}

static void wkb_on_key(lv_event_t *e)
{
  wkb_key_t *k = (wkb_key_t *)lv_event_get_user_data(e);
  lv_obj_t *ta;

  /* 两个输入框共用一个键盘：写到"用户最后点的那个框"（默认密码框） */

  ta = (s_kb_field == 0 && s_ssid_ta != NULL) ? s_ssid_ta : s_pwd_ta;

  if (k == NULL || ta == NULL)
    {
      return;
    }

  switch (k->kind)
    {
      case WKB_BACK:
        lv_textarea_delete_char(ta);
        break;

      case WKB_TOGGLE:
        s_kb_layout = (s_kb_layout + 1) % 3;
        wkb_apply_layout();
        break;

      default:
        lv_textarea_add_char(ta, (uint32_t)(unsigned char)k->ch);
        break;
    }
}

/* 键盘只有一套、两个框共用，用边框高亮告诉用户"现在输入的是哪个框"。 */

static void wifi_pwd_highlight(void)
{
  lv_color_t on = lv_color_hex(TP_CLR_GREEN);
  lv_color_t off = lv_color_hex(0xd5dde5);

  if (s_ssid_ta != NULL)
    {
      lv_obj_set_style_border_width(s_ssid_ta, (s_kb_field == 0) ? 2 : 1, 0);
      lv_obj_set_style_border_color(s_ssid_ta,
                                   (s_kb_field == 0) ? on : off, 0);
    }

  if (s_pwd_ta != NULL)
    {
      lv_obj_set_style_border_width(s_pwd_ta, (s_kb_field == 1) ? 2 : 1, 0);
      lv_obj_set_style_border_color(s_pwd_ta,
                                   (s_kb_field == 1) ? on : off, 0);
    }
}

static void wifi_pwd_on_clear(lv_event_t *e)
{
  (void)e;

  if (s_ssid_ta == NULL)
    {
      return;
    }

  lv_textarea_set_text(s_ssid_ta, "");
  s_kb_field = 0;
  wifi_pwd_highlight();
}

static void wifi_pwd_on_ta(lv_event_t *e)
{
  lv_obj_t *ta = lv_event_get_target_obj(e);

  s_kb_field = (ta == s_ssid_ta) ? 0 : 1;
  wifi_pwd_highlight();
}

static void wkb_build(lv_obj_t *parent)
{
  int r;
  int c;

  s_kb_box = lv_obj_create(parent);
  lv_obj_remove_style_all(s_kb_box);
  lv_obj_set_size(s_kb_box, 480, 160);
  lv_obj_set_pos(s_kb_box, 0, WKB_TOP);
  lv_obj_set_style_bg_color(s_kb_box, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(s_kb_box, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s_kb_box, LV_OBJ_FLAG_SCROLLABLE);

  for (r = 0; r < WKB_ROWS; r++)
    {
      int n = (int)s_kb_rowlen[WKB_LOWER][r];
      int w = (WKB_WIDE - WKB_GAP * (n - 1)) / n;
      int used = w * n + WKB_GAP * (n - 1);
      int sx = WKB_MARGIN + (WKB_WIDE - used) / 2;
      int y = 2 + r * (WKB_KEY_H + WKB_GAP);

      for (c = 0; c < n; c++)
        {
          wkb_key_t *k = &s_kb_keys[r][c];

          k->btn = lv_button_create(s_kb_box);
          lv_obj_remove_style_all(k->btn);
          lv_obj_set_size(k->btn, w, WKB_KEY_H);
          lv_obj_set_pos(k->btn, sx + c * (w + WKB_GAP), y);
          lv_obj_set_style_radius(k->btn, 6, 0);
          lv_obj_set_style_bg_color(k->btn, lv_color_hex(0xeef2f6), 0);
          lv_obj_set_style_bg_opa(k->btn, LV_OPA_COVER, 0);
          lv_obj_set_style_border_width(k->btn, 0, 0);
          lv_obj_set_style_shadow_width(k->btn, 0, 0);
          lv_obj_set_style_bg_color(k->btn, lv_color_hex(TP_CLR_GREEN),
                                    LV_STATE_PRESSED);

          k->lbl = lv_label_create(k->btn);
          lv_obj_set_style_text_font(k->lbl, &lv_font_montserrat_14, 0);
          lv_obj_set_style_text_color(k->lbl,
                                      lv_color_hex(TP_CLR_TEXT_MAIN), 0);
          lv_obj_center(k->lbl);

          lv_obj_add_event_cb(k->btn, wkb_on_key, LV_EVENT_CLICKED, k);
        }
    }

  wkb_apply_layout();
}

static void wifi_pwd_timer_cb(lv_timer_t *timer)
{
  (void)timer;

  if (s_pwd_status == NULL)
    {
      return;
    }

  switch (s_conn_state)
    {
      case 1:
        lv_label_set_text(s_pwd_status, "正在连接，请稍候…");
        wifi_pwd_enable(false);
        break;

      case 2:
        lv_label_set_text(s_pwd_status, "连接成功，正在保存配置…");
        wifi_pwd_enable(false);

        /* 停 2 拍（~0.8s）让用户看到成功提示，再退回首页 */
        s_ok_ticks++;
        if (s_ok_ticks >= 3)
          {
            s_conn_state = 0;
            ui_app_pop_screen();   /* 密码页 → 网络列表页 */
            ui_app_pop_screen();   /* 网络列表页 → 首页 */
          }
        break;

      case 3:
        lv_label_set_text(s_pwd_status, "连接失败，请检查密码后重试");
        wifi_pwd_enable(true);
        s_conn_state = 0;
        break;

      default:
        break;
    }
}

static void wifi_pwd_on_delete(lv_event_t *e)
{
  (void)e;

  if (s_pwd_timer != NULL)
    {
      lv_timer_delete(s_pwd_timer);
      s_pwd_timer = NULL;
    }

  s_pwd_ta = NULL;
  s_ssid_ta = NULL;
  s_kb_box = NULL;

  {
    int kb_r;
    int kb_c;

    for (kb_r = 0; kb_r < WKB_ROWS; kb_r++)
      {
        for (kb_c = 0; kb_c < WKB_COLS; kb_c++)
          {
            s_kb_keys[kb_r][kb_c].btn = NULL;
            s_kb_keys[kb_r][kb_c].lbl = NULL;
          }
      }
  }
  s_pwd_btn_ok = NULL;
  s_pwd_btn_cancel = NULL;
  s_pwd_status = NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_wifi_create(void)
{
  lv_obj_t *scr = lv_obj_create(lv_screen_active());
  lv_obj_t *top;
  lv_obj_t *btn;
  lv_obj_t *l;
  char buf[64];

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 320);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  theme_page_bg(scr, TP_CLR_DATA_BG_A, TP_CLR_DATA_BG_B);

  /* ── 顶部行：返回 + 标题 + 联网状态 ─────────────────────────── */

  top = lv_obj_create(scr);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, 480, 32);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_style_pad_hor(top, 10, 0);
  lv_obj_set_style_pad_column(top, 8, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  btn = lv_button_create(top);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 74, 26);
  lv_obj_set_style_radius(btn, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(TP_CLR_TEXT_MAIN), 0);
  lv_obj_set_style_text_font(btn, TP_FONT_CAPTION, 0);
  theme_btn_set_text(btn, LV_SYMBOL_LEFT " 返回");
  lv_obj_add_event_cb(btn, wifi_on_back, LV_EVENT_CLICKED, NULL);

  {
    lv_obj_t *sp = lv_obj_create(top);

    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 4, 4);
    lv_obj_set_flex_grow(sp, 1);
  }

  l = lv_label_create(top);
  lv_label_set_text(l, LV_SYMBOL_WIFI " 网络连接");
  lv_obj_set_style_text_font(l, TP_FONT_TITLE, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_GREEN_DEEP), 0);

  {
    lv_obj_t *sp = lv_obj_create(top);

    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 4, 4);
    lv_obj_set_flex_grow(sp, 1);
  }

  snprintf(buf, sizeof(buf), "%s",
           wifi_manager_is_connected() ? "已联网" : "未联网");
  l = lv_label_create(top);
  lv_label_set_text(l, buf);
  lv_obj_set_style_text_font(l, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(
              wifi_manager_is_connected() ? TP_CLR_STATE_OK
                                          : TP_CLR_TEXT_MUTED), 0);

  /* ── 状态提示行 ─────────────────────────────────────────────── */

  s_hint = lv_label_create(scr);
  lv_label_set_text(s_hint, "输入网络名和密码即可换网");
  lv_obj_set_style_text_font(s_hint, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(s_hint, lv_color_hex(TP_CLR_TEXT_SUB), 0);
  lv_obj_set_pos(s_hint, 12, 36);
  lv_obj_set_width(s_hint, 456);

  /* ── 网络列表（可上下滚动） ─────────────────────────────────── */

  s_list = lv_obj_create(scr);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, 456, 206);
  lv_obj_set_pos(s_list, 12, 60);
  lv_obj_set_style_pad_all(s_list, 0, 0);
  lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

  /* ── 底部：手动输入网络名（不再扫描，原因见 wifi_on_manual） ── */

  btn = lv_button_create(scr);
  lv_obj_remove_style_all(btn);
  lv_obj_add_style(btn, &theme_style_btn_primary, 0);
  lv_obj_set_size(btn, 456, 40);
  lv_obj_set_pos(btn, 12, 272);
  theme_btn_set_text(btn, "输入网络名 / 密码换网");
  lv_obj_add_event_cb(btn, wifi_on_manual, LV_EVENT_CLICKED, NULL);

  /* 当前已连接的 SSID（用于列表里标「已连接」） */

  s_cur_ssid[0] = '\0';

  {
    wifi_status_t st;

    if (wifi_manager_get_status(&st) == 0)
      {
        strncpy(s_cur_ssid, st.ssid, sizeof(s_cur_ssid) - 1);
        s_cur_ssid[sizeof(s_cur_ssid) - 1] = '\0';
      }
  }

  s_shown_gen = -1;
  wifi_list_build();
  s_shown_gen = s_scan_gen;

  s_timer = lv_timer_create(wifi_ui_timer_cb, WIFI_POLL_MS, NULL);

  /* ⚠ 页面销毁必须删 timer + 置空指针（否则 UAF 崩溃） */

  lv_obj_add_event_cb(scr, wifi_on_delete, LV_EVENT_DELETE, NULL);

  return scr;
}

lv_obj_t *screen_wifi_pwd_create(const char *ssid)
{
  lv_obj_t *scr;
  lv_obj_t *top;
  lv_obj_t *btn;
  lv_obj_t *l;
  char buf[80];

  if (ssid == NULL)
    {
      return NULL;
    }

  strncpy(s_pwd_ssid, ssid, sizeof(s_pwd_ssid) - 1);
  s_pwd_ssid[sizeof(s_pwd_ssid) - 1] = '\0';
  s_pwd_secret[0] = '\0';
  s_eye_open = false;
  s_ok_ticks = 0;
  if (!s_conn_live)
    {
      s_conn_state = 0;
    }

  scr = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 320);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  theme_page_bg(scr, TP_CLR_HOME_BG_A, TP_CLR_HOME_BG_B);

  /* ── 顶部行：取消 + 标题 ───────────────────────────────────── */

  top = lv_obj_create(scr);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, 480, 28);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_style_pad_hor(top, 10, 0);
  lv_obj_set_style_pad_column(top, 8, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  btn = lv_button_create(top);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 74, 26);
  lv_obj_set_style_radius(btn, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(TP_CLR_TEXT_MAIN), 0);
  lv_obj_set_style_text_font(btn, TP_FONT_CAPTION, 0);
  theme_btn_set_text(btn, LV_SYMBOL_LEFT " 返回");
  lv_obj_add_event_cb(btn, wifi_pwd_on_cancel, LV_EVENT_CLICKED, NULL);

  l = lv_label_create(top);
  lv_label_set_text(l, "连接网络");
  lv_obj_set_style_text_font(l, TP_FONT_TITLE, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(TP_CLR_GREEN_DEEP), 0);

  /* ── 目标网络 ───────────────────────────────────────────────── */

  s_ssid_ta = lv_textarea_create(scr);
  lv_obj_set_size(s_ssid_ta, 388, 34);
  lv_obj_set_pos(s_ssid_ta, 12, 30);
  lv_textarea_set_one_line(s_ssid_ta, true);
  lv_textarea_set_max_length(s_ssid_ta, 32);
  lv_textarea_set_placeholder_text(s_ssid_ta, "网络名（SSID）");
  lv_textarea_set_text(s_ssid_ta, s_pwd_ssid);
  lv_obj_set_style_text_font(s_ssid_ta, TP_FONT_CAPTION, 0);
  lv_obj_set_style_bg_color(s_ssid_ta, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(s_ssid_ta, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_ssid_ta, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_border_width(s_ssid_ta, 1, 0);
  lv_obj_set_style_border_color(s_ssid_ta, lv_color_hex(0xd5dde5), 0);
  lv_obj_set_style_pad_hor(s_ssid_ta, 10, 0);
  lv_obj_set_style_pad_ver(s_ssid_ta, 2, 0);
  lv_obj_set_style_text_color(s_ssid_ta, lv_color_hex(TP_CLR_TEXT_MAIN), 0);
  lv_obj_set_style_text_color(s_ssid_ta, lv_color_hex(TP_CLR_TEXT_MUTED),
                              LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(s_ssid_ta, lv_color_hex(TP_CLR_GREEN),
                            LV_PART_CURSOR);
  lv_obj_set_style_bg_opa(s_ssid_ta, LV_OPA_COVER, LV_PART_CURSOR);
  lv_obj_add_event_cb(s_ssid_ta, wifi_pwd_on_ta, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(s_ssid_ta, wifi_pwd_on_ta, LV_EVENT_FOCUSED, NULL);

  btn = lv_button_create(scr);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 48, 34);
  lv_obj_set_pos(btn, 408, 30);
  lv_obj_set_style_radius(btn, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(TP_CLR_TEXT_SUB), 0);
  lv_obj_set_style_text_font(btn, TP_FONT_CAPTION, 0);
  theme_btn_set_text(btn, "清空");
  lv_obj_add_event_cb(btn, wifi_pwd_on_clear, LV_EVENT_CLICKED, NULL);

  /* ── 密码输入框 + 显示/隐藏 ─────────────────────────────────── */

  s_pwd_ta = lv_textarea_create(scr);
  lv_obj_set_size(s_pwd_ta, 388, 34);
  lv_obj_set_pos(s_pwd_ta, 12, 68);
  lv_obj_set_style_pad_ver(s_pwd_ta, 2, 0);
  lv_obj_add_event_cb(s_pwd_ta, wifi_pwd_on_ta, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(s_pwd_ta, wifi_pwd_on_ta, LV_EVENT_FOCUSED, NULL);
  lv_textarea_set_one_line(s_pwd_ta, true);
  lv_textarea_set_password_mode(s_pwd_ta, true);

  /* 字库只覆盖 U+0020~U+007E，LVGL 默认掩码 U+2022 没有字形 → 会画方框，
   * 改用 * （已在字库范围内）。 */

  lv_textarea_set_password_bullet(s_pwd_ta, "*");
  lv_textarea_set_max_length(s_pwd_ta, 63);
  lv_textarea_set_placeholder_text(s_pwd_ta, "密码");
  lv_obj_set_style_text_font(s_pwd_ta, TP_FONT_BODY, 0);
  lv_obj_set_style_bg_color(s_pwd_ta, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(s_pwd_ta, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_pwd_ta, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_border_width(s_pwd_ta, 1, 0);
  lv_obj_set_style_border_color(s_pwd_ta, lv_color_hex(0xd5dde5), 0);
  lv_obj_set_style_pad_hor(s_pwd_ta, 10, 0);
  lv_obj_set_style_text_color(s_pwd_ta, lv_color_hex(TP_CLR_TEXT_MAIN), 0);
  lv_obj_set_style_text_color(s_pwd_ta, lv_color_hex(TP_CLR_TEXT_MUTED),
                              LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(s_pwd_ta, lv_color_hex(TP_CLR_GREEN),
                            LV_PART_CURSOR);
  lv_obj_set_style_bg_opa(s_pwd_ta, LV_OPA_COVER, LV_PART_CURSOR);

  btn = lv_button_create(scr);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 48, 34);
  lv_obj_set_pos(btn, 408, 68);
  lv_obj_set_style_radius(btn, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(TP_CLR_TEXT_SUB), 0);
  lv_obj_set_style_text_font(btn, TP_FONT_TITLE, 0);
  theme_btn_set_text(btn, LV_SYMBOL_EYE_OPEN);
  lv_obj_add_event_cb(btn, wifi_pwd_on_eye, LV_EVENT_CLICKED, NULL);

  /* ── 状态 + 按钮 ────────────────────────────────────────────── */

  s_pwd_status = lv_label_create(scr);
  lv_label_set_text(s_pwd_status, "输入网络名和密码后点「连接」");
  lv_obj_set_width(s_pwd_status, 456);
  lv_obj_set_pos(s_pwd_status, 12, 106);
  lv_obj_set_style_text_font(s_pwd_status, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(s_pwd_status,
                              lv_color_hex(TP_CLR_TEXT_SUB), 0);

  s_pwd_btn_ok = lv_button_create(scr);
  lv_obj_remove_style_all(s_pwd_btn_ok);
  lv_obj_add_style(s_pwd_btn_ok, &theme_style_btn_primary, 0);
  lv_obj_set_size(s_pwd_btn_ok, 220, 30);
  lv_obj_set_pos(s_pwd_btn_ok, 12, 126);
  theme_btn_set_text(s_pwd_btn_ok, "连接");
  lv_obj_add_event_cb(s_pwd_btn_ok, wifi_pwd_on_ok, LV_EVENT_CLICKED, NULL);

  s_pwd_btn_cancel = lv_button_create(scr);
  lv_obj_remove_style_all(s_pwd_btn_cancel);
  lv_obj_set_size(s_pwd_btn_cancel, 220, 30);
  lv_obj_set_pos(s_pwd_btn_cancel, 248, 126);
  lv_obj_set_style_radius(s_pwd_btn_cancel, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_bg_color(s_pwd_btn_cancel, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(s_pwd_btn_cancel, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s_pwd_btn_cancel,
                              lv_color_hex(TP_CLR_TEXT_MAIN), 0);
  lv_obj_set_style_text_font(s_pwd_btn_cancel, TP_FONT_BODY, 0);
  theme_btn_set_text(s_pwd_btn_cancel, "取消");
  lv_obj_add_event_cb(s_pwd_btn_cancel, wifi_pwd_on_cancel,
                      LV_EVENT_CLICKED, NULL);

  /* ── 软键盘（自绘，见上方 WKB_* 实现） ─────────────────────── */

  s_kb_layout = WKB_LOWER;
  s_kb_field = (s_pwd_ssid[0] == '\0') ? 0 : 1;
  wkb_build(scr);
  wifi_pwd_highlight();

  if (s_conn_live)
    {
      lv_label_set_text(s_pwd_status, "正在连接，请稍候…");
      wifi_pwd_enable(false);
    }

  s_pwd_timer = lv_timer_create(wifi_pwd_timer_cb, WIFI_POLL_MS, NULL);

  lv_obj_add_event_cb(scr, wifi_pwd_on_delete, LV_EVENT_DELETE, NULL);

  /* 布局自检（串口一行，方便核对键盘位置/键数） */

  lv_obj_update_layout(scr);

  {
    lv_area_t a;
    uint32_t n = 0;

    if (s_kb_box != NULL)
      {
        n = (uint32_t)lv_obj_get_child_count(s_kb_box);
        lv_obj_get_coords(s_kb_box, &a);
        printf("[WifiUI] kbbox=%p scr=%p %d,%d %dx%d keys=%u hidden=%d\n",
               (void *)s_kb_box, (void *)scr, (int)a.x1, (int)a.y1,
               (int)lv_area_get_width(&a), (int)lv_area_get_height(&a),
               (unsigned)n,
               (int)lv_obj_has_flag(s_kb_box, LV_OBJ_FLAG_HIDDEN));

        if (n > 0)
          {
            lv_obj_get_coords(lv_obj_get_child(s_kb_box, 0), &a);
            printf("[WifiUI] key0 %d,%d %dx%d\n", (int)a.x1, (int)a.y1,
                   (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
          }
      }
  }
  return scr;
}