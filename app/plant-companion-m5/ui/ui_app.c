#include "../components/voice_wake/voice_wake.h"
#include <stdatomic.h>
/****************************************************************************
 * apps/plant-companion/ui/ui_app.c
 *
 * 植小伴 UI 入口：lv_init + lv_nuttx_init + 屏幕管理 + ui_task 主循环
 *
 * 屏幕管理模型：
 *   - 4 个 Tab 屏幕（首页/数据/任务/日记）常驻，切换 = 显隐（保留状态）
 *   - 全屏二级页（拍照/诊断/语音）后续用 push/pop 栈（3C 阶段实现）
 *   - 所有 LVGL 对象只在 ui_task 线程操作
 *
 * 数据刷新模型（ARCHITECTURE.md §3.2）：
 *   - sensor_service 在独立轮询线程更新缓存，通过回调通知
 *   - 本文件注册 LVGL 定时器（ui_refresh_timer），在 ui_task 上下文
 *     读取缓存并刷新当前可见屏 —— 满足"LVGL 对象只在 ui_task 操作"
 ****************************************************************************/

#include <nuttx/config.h>
#include "../services/power_service.h"
#include "../services/ota_service.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <stdint.h>
#include <malloc.h>
#include <time.h>
#include <nuttx/clock.h>

#include <lvgl/lvgl.h>

#include "ui_app.h"
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
#  include "../components/stackchan_body/stackchan_body.h"
#  include "../services/interaction_service.h"
#  include "screens/screen_voice.h"
#  include "screens/voice_face_cache.h"
#endif
#include "screens/screen_camera.h"
#include "screens/screen_home.h"
#include "screens/screen_data.h"
#include "screens/screen_tasks.h"
#include "screens/screen_diary.h"
#include "widgets/widget_bottomnav.h"
#include "theme/theme_plant.h"
#include "assets/fonts/zh_font.h"
#include "../services/sensor_service.h"
#include "../components/camera_capture/camera_capture.h"
#include "../services/sd_write.h"
#include "../services/plant_state.h"
#include "../services/record_service.h"
#ifdef CONFIG_PLANT_AI_VOICE
#  include "../services/voice_service.h"
#endif

#ifdef CONFIG_PLANT_AI_VOICE
/* ⚠️ 2026-09-16 三修：语音会话看门狗（1s，ui_task 上下文）。
 * 用户现场：点"说话"后界面一直停在"正在听你说"，再点按钮没反应，
 * 只能重启板卡。兜底逻辑见 voice_service_watchdog_poll()。 */

static void ui_voice_wd_timer_cb(lv_timer_t *timer)
{
  (void)timer;
  voice_service_watchdog_poll();
}
#endif

#ifdef CONFIG_PLANT_AI_MODULE
#  include "../services/server_bridge.h"
#  include "../services/actuator_service.h"
#endif

#ifdef CONFIG_ESP32S3_WIFI
#  include "screens/screen_ota.h"
#  include "../services/ota_service.h"
#  include "../ota/ota.h"
#endif

#ifdef CONFIG_PLANT_SOIL_SENSOR
#  include "../components/sensor_driver/soil_sensor.h"
#endif

#ifdef CONFIG_PLANT_SYSTEM_MONITOR
#  include "../components/system_monitor/battery_monitor.h"
#endif

#ifdef CONFIG_PLANT_WIFI_MANAGER
#  include "../communication/wifi_manager/wifi_manager.h"
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_nuttx_result_t g_ui_result;
static lv_obj_t *g_screens[PLANT_TAB_COUNT];
static int g_active_tab = PLANT_TAB_HOME;
static volatile bool g_ui_running;

/* LVGL 9.2.1 输入设备缺陷规避（2026-09-08 实机 + 模拟手指复现）：
 * 在 LVGL pointer indev 的 CLICKED 事件回调里 lv_obj_delete 当前页面
 * （点底部 Tab 切页 / 二级页点"返回"），被按对象随页面销毁 → 事件被
 * 标 deleted 中断，indev_proc_release 提前 return、不清 act_obj →
 * 悬垂指针留在输入设备状态，此后所有触摸都按该已释放对象处理
 * （PRESS_LOCK 使 LVGL 不再重新查对象）→ 表现为"跑一阵后触摸切换
 * 失灵/卡住"，而串口 nav 正常。
 * 规避：页面删除一律延迟到刷新定时器（事件派发结束后）执行；删除前
 * 先置 HIDDEN，避免残留页面遮挡下层。 */

#define UI_DELAYED_DEL_MAX 8

static lv_obj_t *g_delayed[UI_DELAYED_DEL_MAX];
static int g_delayed_cnt;

static void ui_sweep_orphans(void);

static void ui_delete_deferred(lv_obj_t *obj)
{
  if (obj == NULL)
    {
      return;
    }

  if (g_delayed_cnt < UI_DELAYED_DEL_MAX)
    {
      g_delayed[g_delayed_cnt++] = obj;
    }
  else
    {
      lv_obj_delete(obj);
    }
}

static void ui_flush_deferred(void)
{
  int i;

  for (i = 0; i < g_delayed_cnt; i++)
    {
      if (g_delayed[i] != NULL)
        {
          lv_obj_delete(g_delayed[i]);
        }
    }

  g_delayed_cnt = 0;

  ui_sweep_orphans();
}

/* 调试：plant nav <tab> 跨线程切页请求（-1=无请求）。
 * NSH 任务只写此标志；实际 LVGL 对象操作由 ui_refresh_timer_cb
 * 在 ui_task 上下文完成（铁律：LVGL 对象只在 ui_task 操作）。 */

static volatile int g_nav_pending = -1;
static volatile bool g_audit_pending;
void ui_app_audit(void) { g_audit_pending = true; }
static volatile int g_page_pending = 0;
static volatile int g_camera_mode_pending = -1;
static int g_business_capture_ticks;
static lv_obj_t *g_voice_pill;
/* A worker only publishes completion; LVGL remains owned by ui_task. */
static atomic_bool g_boot_settled_paint;

/* 调试模拟触摸（plant tsim）：额外一路 LVGL pointer indev，在 ui_task
 * 内按真实点按流程触发底部 Tab 点击（含"点击回调中删除自身页面"路径），
 * 用于复现"跑一阵后触摸切换失效"。g_syn_press_life>0 = 正在按住
 * （LVGL indev 每 ~33ms 读一次，3 次 ≈100ms，低于长按阈值）。 */

static lv_indev_t *g_syn_indev;
static int ui_capture_ascii_do(void);
static int g_syn_x;
static int g_syn_y;
static volatile int g_syn_press_life;
static volatile bool g_syn_report_hit;   /* plant tap：命中诊断只报一次 */
static volatile int g_syn_left;
static volatile bool g_syn_busy;
static volatile int g_syn_done_cnt;

/* Synthetic drag (diagnostic only): inject a press + N moves + release
 * through the same LVGL pointer indev used by plant tap. */

static volatile bool g_scroll_busy;
static volatile int g_scroll_step;
static volatile int g_scroll_steps;
static volatile bool g_scroll_release;
static int g_scroll_x;
static int g_scroll_y;
static int g_scroll_dx;
static int g_scroll_dy;

/* Real-touch diag (plant tdiag on|off|now):
 * - driver-side GT911 counters come from board gt911_diag_snapshot()
 *   (flat build direct call);
 * - LVGL-side: 25ms timer samples the real indev (g_ui_result.indev)
 *   press state to count presses LVGL actually received.  Only reads
 *   LVGL state, never writes/events. */

static lv_indev_state_t g_tdiag_last_lv_state;
static uint32_t g_tdiag_lv_press;
static uint32_t g_tdiag_lv_press_ms;
static uint32_t g_tdiag_lv_x;
static uint32_t g_tdiag_lv_y;
static volatile int g_tdiag_on;
static uint32_t g_tdiag_cnt;

/* 切页内存打点（plant memlog on|off，默认关）：开时在页面真正切完、
 * 延迟删除冲刷后的下一拍，从 ui_task 打一行系统堆余量 + 相对首页基线
 * 的增量。用于观测各页对象开销，以及"回首页增量应归零"的泄漏检查。 */

static volatile int g_memlog_on;
static volatile int g_memlog_pending_tab = -1;
static int g_heap_after_home;


/* 调试截屏状态（plant cap <name>）：NSH 任务置请求+缓冲区，
 * ui_task 定时器用 LVGL snapshot 渲染并写 BMP 后置 done。 */

static char g_cap_path[96];
static void *g_cap_buf;
static size_t g_cap_bufsize;
static volatile int g_cap_req;
static volatile int g_cap_done;
static volatile int g_cap_status;
static volatile int g_cap_mode;  /* 0=BMP 文件, 1=串口 ASCII */
static volatile int g_cap_ink;
static int g_cap_win_x;
static int g_cap_win_y;
static int g_cap_win_w;
static int g_cap_win_h;

/* 调试对象树转储（plant objs）：NSH 置请求，ui_task 定时器遍历当前屏
 * 打印 label 文本/坐标/字体/码点（定位"方框/缺字"的渲染对象）。 */

static volatile int g_dump_req;
static volatile int g_dump_done;
static volatile int g_dump_status;

/* 全屏二级页栈（②③⑥），最多 3 层 */

#define UI_SECONDARY_MAX 3

static lv_obj_t *g_secondary[UI_SECONDARY_MAX];
static int g_secondary_depth;

/* 显示测试图片（plant img show/test）：外部任务设数据+标志，
 * ui_task 定时器在 LVGL 上下文创建/刷新全屏 lv_image。 */

static lv_obj_t *g_img_test;
static lv_image_dsc_t g_img_dsc;
static const uint8_t *g_img_data;
static int g_img_w;
static int g_img_h;
static volatile bool g_img_pending;

/* 阶段 B：顶栏实时时间（方案 F，2026-09-01：本地走时 + 低频校准）。
 *
 * 架构：抓一次服务器时间（unix 北京时间 + tick 基准）后，顶栏时间由
 * 系统 tick 本地推算（每分钟自然跳），网络只在三种情况才发请求：
 *   ① 配置服务器后立即抓（配置即抓，解决"连上网却无时间"的时间差）；
 *   ② 从未成功 → 5s 快重试；
 *   ③ 已成功 → 1h 才校准一次（ESP32-S3 晶振 ±10~30ppm，1h 漂移 <0.1s）。
 * 请求频率从"60s 轮询 1440 次/天"降到 ~24 次/天；HTTP 阻塞只发生在
 * 独立 worker 线程内（socket 等待让出 CPU），不阻塞 ui_task/语音/摄像头。
 *
 * worker 线程只写静态缓冲 + 标志，ui_task 定时器消费上屏
 * （LVGL 对象只在 ui_task 的铁律，照抄 voice/camera 页模式）。 */

#define UI_TIME_REFRESH_SEC  3600   /* 校准周期：成功抓取后 1 小时 */
#define UI_TIME_RETRY_SEC    5      /* 从未成功：快重试间隔 */
#define UI_TIME_LOOP_SEC     1      /* worker 循环粒度（每秒醒一次） */

/* 心跳（2026-09-11）：板卡每 60s 向服务器发一次心跳保活，服务器据此刷新
 * last_seen_at（此前只在开机注册时写一次 → 管理台永远显示"离线"）。
 * 挂在时间 worker 里顺带发，不额外开线程（板卡内存紧张）。 */

#define UI_HB_INTERVAL_SEC   60     /* 心跳周期：服务器要求 1 分钟一次 */
#define UI_HB_RETRY_SEC      10     /* 心跳失败快重试（未联网时 10s 试一次） */
/* 网络自愈（2026-09-17）：连续这么多次心跳都发不出去（每次间隔 10s，约 40s）
 * 就判定"看着连着、其实不通"，用保存的凭据重连一次。用户现场表现是
 * "怎么这会又断网了"，以前只能断电重启。两次自愈之间至少隔
 * UI_NET_HEAL_GAP_SEC，避免服务器侧故障时反复折腾 WiFi。 */

#define UI_NET_HEAL_FAILS    2
#define UI_NET_HEAL_GAP_SEC  180

static int64_t g_time_unix;         /* 北京时间 unix（抓取时刻基准，已偏移） */
static clock_t g_tick_at_fetch;     /* 抓取时刻的系统 tick（clock_systime_ticks） */
static bool g_time_valid;           /* 是否已抓到基准（true 才显示推算时间） */
static char g_time_str[8];          /* "HH:MM"（顶栏显示，worker 每秒推算） */
static char g_date_str[16];         /* "YYYY-MM-DD"（抓取时更新，阶段 C 用） */
static volatile bool g_time_pending;

/* ⚠️ 2026-09-01 内存回归：时间 worker 栈**不能**用静态 .bss 缓冲——
 * 曾用静态 4KB+OTA 8KB 栈把堆从 ~112KB 挤到 ~102.6KB（_sheap 上移），
 * free 掉到 9.3KB，wapi 任务（栈 8KB+TCB≈9KB）spawn 失败 EPERM → WiFi 连不上。
 * 改为 pthread_attr_setstacksize 从堆分配（2026-09-16 起 12KB；原 4KB 会被
 * 中文 printf + socket 调用链踩爆，见 ui_app_start 中的说明）。 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 未实现的 Tab 用占位页（3C/3D 逐个替换为真实屏幕） */


/* 模拟触摸读回调：按住期间返回 PRESSED（计数递减），3 次后返回 RELEASED，
 * LVGL 在 PRESSED→RELEASED 沿上派发真实点按事件（RELEASED/CLICKED）。 */

/* ⚠️ 2026-09-10 诊断：用 LVGL 官方 hit-test 报告"这一下点到了谁"。
 * plant tap 打了但界面没反应时，看这行即可区分
 * "没点到控件" / "点到了 clickable 但回调没走"。 */

static void ui_syn_hit_report(void)
{
  lv_point_t p;
  lv_obj_t *hit;
  lv_area_t a;

  p.x = (int32_t)g_syn_x;
  p.y = (int32_t)g_syn_y;
  hit = lv_indev_search_obj(lv_screen_active(), &p);

  if (hit == NULL)
    {
      printf("[SynTap] hit @%d,%d -> 无对象（点空白）\n", g_syn_x, g_syn_y);
      fflush(stdout);
      return;
    }

  lv_obj_get_coords(hit, &a);
  printf("[SynTap] hit @%d,%d -> %p %s at %d,%d %dx%d\n",
         g_syn_x, g_syn_y, (void *)hit,
         lv_obj_has_flag(hit, LV_OBJ_FLAG_CLICKABLE) ? "clickable" : "no-click",
         (int)a.x1, (int)a.y1,
         (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
  fflush(stdout);
}

static void ui_syn_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  (void)indev;

  if (g_scroll_busy)
    {
      int denom = g_scroll_steps > 0 ? g_scroll_steps : 1;

      data->point.x = g_scroll_x + g_scroll_dx * g_scroll_step / denom;
      data->point.y = g_scroll_y + g_scroll_dy * g_scroll_step / denom;

      if (!g_scroll_release)
        {
          data->state = LV_INDEV_STATE_PRESSED;
          if (g_scroll_step >= g_scroll_steps)
            {
              g_scroll_release = true;
            }
          else
            {
              g_scroll_step++;
            }
        }
      else
        {
          data->state = LV_INDEV_STATE_RELEASED;
          g_scroll_busy = false;
        }

      return;
    }

  data->point.x = g_syn_x;
  data->point.y = g_syn_y;

  if (g_syn_press_life > 0)
    {
      data->state = LV_INDEV_STATE_PRESSED;
      g_syn_press_life--;

      if (g_syn_press_life == 0)
        {
          if (g_syn_left > 0)
            {
              g_syn_left--;
              g_syn_done_cnt++;
            }

          if (g_syn_report_hit)
            {
              ui_syn_hit_report();
              g_syn_report_hit = false;
            }
        }
    }
  else
    {
      data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* 模拟触摸步进：由 500ms 刷新定时器驱动，每次按下"当前页的下一个 Tab"。 */

static void ui_syn_tick(void)
{
  int target;
  int cx;
  int cy;
  lv_obj_t *nav;

  if (!g_syn_busy)
    {
      return;
    }

  if (g_syn_press_life > 0)
    {
      return;   /* 还在按住，read_cb 计数归零即释放 */
    }

  if (g_syn_left <= 0)
    {
      g_syn_busy = false;
      printf("[SynTap] done total=%d\n", g_syn_done_cnt);
      fflush(stdout);
      return;
    }

  nav = widget_bottomnav_find(g_screens[g_active_tab]);

  if (nav == NULL)
    {
      printf("[SynTap] WARN no nav (active=%d)\n", g_active_tab);
      fflush(stdout);
      g_syn_left = 0;
      g_syn_busy = false;
      return;
    }

  /* 目标 = 当前导航栏已激活 Tab 的下一个（避开"已激活则不响应"守卫，
   * 让每按必切页，才能持续压到"点按中销毁/隐藏原页面"的真实路径） */

  target = (widget_bottomnav_get_active(nav) + 1) % PLANT_TAB_COUNT;

  if (!widget_bottomnav_tab_center(nav, target, &cx, &cy))
    {
      printf("[SynTap] WARN no tab %d center\n", target);
      fflush(stdout);
      g_syn_left = 0;
      g_syn_busy = false;
      return;
    }

  g_syn_x = cx;
  g_syn_y = cy;
  g_syn_press_life = 3;

  if (g_syn_done_cnt % 50 == 0)
    {
      struct mallinfo mi;

      mi = mallinfo();
      printf("[SynTap] n=%d target=%d at=%d,%d heap_free=%u\n",
             g_syn_done_cnt, target, cx, cy, (unsigned)mi.fordblks);
      fflush(stdout);
    }
}

static void ui_nav_cb(void *user_data, int tab_id)
{
  int i;

  g_active_tab = tab_id;

  /* 页面用完即销毁（用户要求）：切换 Tab 时，销毁除 home 外的旧页面，
   * 释放 LVGL 内存池。home 常驻作为入口，其余页下次进入重新创建。
   * 避免页面常驻导致 LVGL 池耗尽（Tasks 页曾 81% -> grid alloc 失败
   * -> calc_cols NULL panic）。 */

  for (i = 0; i < PLANT_TAB_COUNT; i++)
    {
      if (i == PLANT_TAB_HOME)
        {
          continue;
        }

      if (g_screens[i] != NULL)
        {
          /* 先隐藏再延迟删除：不能在 LVGL 事件回调里同步删除当前页
           * （会留悬垂 act_obj，见上）。实际删除在下一拍刷新定时器。 */

          lv_obj_add_flag(g_screens[i], LV_OBJ_FLAG_HIDDEN);
          ui_delete_deferred(g_screens[i]);
          g_screens[i] = NULL;
        }
    }

  /* 懒加载：目标页未创建则现建（home 常驻；其他页每次重新创建） */

  /* 回首页时同步首页常驻导航栏的选中态：首页 nav 是常驻对象，其 ctx->active
   * 会停留在"上次从首页切走时点的那个 Tab"（曾导致回首页后：
   *   ① 高亮显示在错误的 Tab 上；
   *   ② 再点同一个 Tab 被 nav_on_click 的 active 守卫吞掉、无反应）。
   * 切回首页必须把它的选中态复位成 PLANT_TAB_HOME。 */

  if (tab_id == PLANT_TAB_HOME)
    {
      lv_obj_t *homenav = widget_bottomnav_find(g_screens[PLANT_TAB_HOME]);

      if (homenav != NULL)
        {
          widget_bottomnav_set_active(homenav, PLANT_TAB_HOME);
        }
    }

  if (g_screens[tab_id] == NULL)
    {
      switch (tab_id)
        {
          case PLANT_TAB_DATA:
            g_screens[PLANT_TAB_DATA] = screen_data_create(ui_nav_cb, NULL);
            break;

          case PLANT_TAB_TASKS:
            g_screens[PLANT_TAB_TASKS] = screen_tasks_create(ui_nav_cb, NULL);
            break;

          case PLANT_TAB_DIARY:
            g_screens[PLANT_TAB_DIARY] = screen_diary_create(ui_nav_cb, NULL);
            break;

          default:
            break;
        }
    }

  /* 隐藏所有页，只显示目标页 */

  for (i = 0; i < PLANT_TAB_COUNT; i++)
    {
      if (g_screens[i] != NULL)
        {
          lv_obj_add_flag(g_screens[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (g_screens[tab_id] != NULL)
    {
      lv_obj_remove_flag(g_screens[tab_id], LV_OBJ_FLAG_HIDDEN);
    }

  /* 进数据页：喊轮询线程立刻读一次 + 上报最新值（非阻塞，见
   * sensor_service_read_now 说明），这样打开页面看到的就是当下的真实数据。 */

  if (tab_id == PLANT_TAB_DATA)
    {
      sensor_service_read_now();
    }

  /* Tab bodies are entirely different.  Coalesce the complete transition
   * into one full-screen dirty area so the full-size LVGL buffer produces
   * one full-frame DMA submission. */

  lv_obj_invalidate(lv_screen_active());

  g_memlog_pending_tab = tab_id;
}

/* 阶段 B：顶栏时间 worker（detached，ui_app_start 启动，4KB 堆栈）。
 *
 * 状态机（方案 F）：
 *   - 每秒醒一次，本地推算显示时间（已抓到基准后，不依赖网络）；
 *   - 需要抓取的三种时机：配置变化（epoch）立即抓 / 从未成功 5s 重试 /
 *     已成功 1h 校准；
 *   - HTTP 只发生在 worker 线程内，socket 等待让出 CPU，不阻塞任何进程。
 *   - 同一循环顺带发心跳（每 60s 保活，见 UI_HB_INTERVAL_SEC）：时间与心跳
 *     共用一条 worker 线程与同一份"配置即生效"逻辑，不再多开线程。
 */

#ifdef CONFIG_PLANT_AI_MODULE
static void *ui_time_worker(void *arg)
{
  clock_t last_attempt = 0;
  clock_t last_ok = 0;
  clock_t last_hb = 0;
  int net_fail = 0;              /* 心跳连续失败次数（网络自愈用） */
  clock_t last_heal = 0;         /* 上次自愈时刻（节流用） */
  int last_epoch = -1;
  int hb_interval = UI_HB_INTERVAL_SEC;
  bool hb_ok = false;
  (void)arg;

  while (g_ui_running)
    {
      if(!power_service_background_allowed()){sleep(1);continue;}
      clock_t now = clock_systime_ticks();
      bool need_fetch = false;
      bool need_hb = false;
      int epoch = server_bridge_config_epoch();
      int i;

      /* ① 配置即抓：服务器地址刚设置/变化 → 立即抓取（不等周期） */

      if (epoch != last_epoch)
        {
          last_epoch = epoch;
          need_fetch = true;
          need_hb = true;   /* 刚连上/换服务器：立刻心跳一次保活 */
        }

      /* ② 从未成功（含未配置服务器）：5s 快重试 */

      else if (!g_time_valid)
        {
          need_fetch = (now - last_attempt) >=
                       (clock_t)(UI_TIME_RETRY_SEC * TICK_PER_SEC);
        }

      /* ③ 已成功：1h 校准一次（晶振漂移 1h <0.1s，分钟显示无感） */

      else
        {
          need_fetch = (now - last_ok) >=
                       (clock_t)(UI_TIME_REFRESH_SEC * TICK_PER_SEC);
        }

      if (need_fetch)
        {
          int64_t unix = 0;
          char date[16];
          char tm_buf[8];

          last_attempt = now;

          if (server_bridge_get_time(&unix, date, sizeof(date),
                                     tm_buf, sizeof(tm_buf)) == 0 &&
              unix > 0 && tm_buf[0] != '\0')
            {
              g_time_unix = unix;
              g_tick_at_fetch = clock_systime_ticks();
              g_time_valid = true;
              last_ok = g_tick_at_fetch;
              strncpy(g_date_str, date, sizeof(g_date_str) - 1);
              g_date_str[sizeof(g_date_str) - 1] = '\0';
            }
        }

      /* 心跳保活：① 配置变化立即发（刚注册完 token 就保活，不等周期）；
       * ② 成功后每 hb_interval 秒一次（周期可由服务器下发覆盖）；
       * ③ 失败按 UI_HB_RETRY_SEC 快重试（没连网时快速自愈）。
       * 未配置服务器/无 token 时 server_bridge_heartbeat 直接返回
       * -ENOTCONN，不产生任何网络等待，循环照常每秒醒一次。 */

      if (!need_hb)
        {
          clock_t gap = (now - last_hb) / TICK_PER_SEC;

          need_hb = gap >= (clock_t)(hb_ok ? hb_interval : UI_HB_RETRY_SEC);
        }

      if (need_hb)
        {
          int new_interval = 0;

          last_hb = now;
          if (server_bridge_heartbeat(&new_interval) == 0)
            {
              hb_ok = true;
              net_fail = 0;
              if (new_interval > 0)
                {
                  hb_interval = new_interval;
                }
            }
          else
            {
              hb_ok = false;
              /* 网络自愈：连着几次都发不出去 → 用保存的凭据重连一次。
               * （板卡"看着连着、其实不通"时的兜底，见 wifi_manager.c
               *   wifi_manager_reconnect_saved 的注释。） */

#ifdef CONFIG_PLANT_WIFI_MANAGER
              if (++net_fail >= UI_NET_HEAL_FAILS)
                {
                  net_fail = 0;

                  /* last_heal==0 = 还没自愈过：别被 "now-0 要够 180s"
                   * 卡住（last_heal 是 0 而 now 是开机以来的 tick，开机
                   * 头 180 秒内这条永远不成立）。 */

                  if (last_heal == 0 ||
                      (now - last_heal) >=
                      (clock_t)(UI_NET_HEAL_GAP_SEC * TICK_PER_SEC))
                    {
                      /* 2026-09-27 (v56): 相机活动中推迟自愈。
                       * 实测 wapi 重连（posix_spawn + waitpid + 驱动
                       * ioctl）与相机 DVP DMA 高负载并发时整机死锁
                       * （无看门狗，静默卡死只能手动复位）。心跳失败
                       * 在相机页只影响快门上传（会优雅报错重试），
                       * 退页后再愈。 */

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
                      if (camera_preview_is_running())
                        {
                          last_heal = now;
                          printf("[UI] 相机使用中，推迟网络自愈\n");
                        }
                      else
#endif
                        {
                          last_heal = now;
                          printf("[UI] 网络疑似断了，尝试自动重连\n");
                          wifi_manager_reconnect_saved();
                        }
                    }
                }
#endif

              /* ⚠ 2026-09-15：自愈"配了服务器但没注册上"。
               *
               * 开机那一刻如果那张网还不通外网（换环境、路由器刚起来、
               * 或者用户后来在屏幕里换了网），注册会失败、device_token 一直
               * 是空的；心跳会一直失败，但以前没有任何东西去补注册 ——
               * 表现就是语音页回"还没有配置服务器"、管理台看不到设备在线。
               * 这里只要"地址有、凭证没有"就补一次注册。 */

              if (server_bridge_host()[0] != '\0' &&
                  server_bridge_device_token()[0] == '\0')
                {
                  server_bridge_device_login();
                }
            }
        }

      /* 自动执行层：到点的动作关断回执 → 声明能力 → 取服务器下发的执行
       * 指令。内部自己节流（取指令 15s 一次），未配置服务器时直接返回。 */

      actuator_service_poll();

      /* 本地走时：已抓到基准 → 每秒推算当前北京时间（tick 差值，无符号
       * 回绕安全；不依赖网络节奏，顶栏每分钟自然跳） */

      if (g_time_valid)
        {
          int64_t now_unix;
          int hh;
          int mm;

          now_unix = g_time_unix +
                     (int64_t)((clock_systime_ticks() - g_tick_at_fetch) /
                               TICK_PER_SEC);
          hh = (int)((now_unix % 86400) / 3600);
          mm = (int)((now_unix % 3600) / 60);

          snprintf(g_time_str, sizeof(g_time_str), "%02d:%02d", hh, mm);
          g_time_pending = true;
        }

      /* 每秒粒度（每 1s 检查退出标志，ui_app_stop 可及时退出） */

      for (i = 0; i < UI_TIME_LOOP_SEC && g_ui_running; i++)
        {
          sleep(1);
        }
    }

  return NULL;
}
#endif /* CONFIG_PLANT_AI_MODULE */

/* ui_capture_do 实现在本文件后部（Public Functions 之前） */

static int ui_capture_do(void);
/* ui_capture_ascii_do 同：实现在本文件后部 */
static int ui_capture_ascii_do(void);
/* ui_obj_dump_do 同：实现在本文件后部（遍历对象树打印 label 文本） */
static int ui_obj_dump_do(void);

/* Secondary pages such as the camera live on LVGL's top layer.  Snapshot and
 * diagnostics must follow the actually visible layer, not only the base Tab
 * screen underneath it. */
static lv_obj_t *ui_visible_root(void)
{
  lv_obj_t *top = lv_layer_top();
  uint32_t i;

  if (top != NULL)
    {
      for (i = 0; i < lv_obj_get_child_cnt(top); i++)
        {
          lv_obj_t *child = lv_obj_get_child(top, (int32_t)i);

          if (child != NULL && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
            {
              return top;
            }
        }
    }

  return lv_screen_active();
}

/* 2026-09-17：把本线程 stdout 临时接到文件。
 *
 * 背景：板卡控制台是芯片内置 USB 串口，在"接了电脑但没人读走数据"的时候，
 * NuttX 的发送环形缓冲会被那个"超时丢数据"逻辑改坏 —— 实测界面导出几次后
 * 串口开始吐垃圾字符（一堆 '['），再往后整机宕机。所以凡是有大段突发打印的
 * 调试功能，一律改成写 SD 文件：既碰不到串口，也能用 cat 直接读，更可靠。
 *
 * 返回：成功返回保存的原 stdout fd（>=0），失败返回 -1（此时行为与原来一致）。 */

static int ui_stdout_to_file(FAR const char *path, FAR FILE **fpp)
{
  FAR FILE *fp;
  int save;

  *fpp = NULL;
  fp = fopen(path, "w");
  if (fp == NULL)
    {
      return -1;
    }

  fflush(stdout);
  save = dup(STDOUT_FILENO);
  if (save < 0)
    {
      fclose(fp);
      return -1;
    }

  dup2(fileno(fp), STDOUT_FILENO);
  *fpp = fp;
  return save;
}

static void ui_stdout_restore(int save, FAR FILE *fp)
{
  fflush(stdout);

  if (save >= 0)
    {
      dup2(save, STDOUT_FILENO);
      close(save);
    }

  if (fp != NULL)
    {
      fclose(fp);
    }
}

/* LVGL 定时器：在 ui_task 上下文刷新当前可见屏（每 500ms） */

extern int gt911_diag_snapshot(uint32_t out[8]);

static void ui_touch_watch_cb(lv_timer_t *timer)
{
  lv_indev_state_t st;
  (void)timer;

  if (g_ui_result.indev == NULL)
    {
      return;
    }

  st = lv_indev_get_state(g_ui_result.indev);

  if (st == LV_INDEV_STATE_PRESSED &&
      g_tdiag_last_lv_state != LV_INDEV_STATE_PRESSED)
    {
      lv_point_t pt;

      if(!power_service_standby())power_service_activity();
      g_tdiag_lv_press++;
      lv_indev_get_point(g_ui_result.indev, &pt);
      g_tdiag_lv_x = (uint32_t)pt.x;
      g_tdiag_lv_y = (uint32_t)pt.y;
      g_tdiag_lv_press_ms = lv_tick_get();
    }

  g_tdiag_last_lv_state = st;
}

/* Print one diag line: driver GT911 counters + LVGL real-touch counters */

static void ui_tdiag_print(void)
{
  uint32_t k[8];
  uint32_t now = lv_tick_get();

  if (gt911_diag_snapshot(k) != 0)
    {
      printf("[TD] kernel gt911_diag_snapshot unavailable\n");
      fflush(stdout);
      return;
    }

  printf("[TD] t=%u lvpress=%u lvxy=%u,%u lvlast=%ums lvstate=%d | "
         "k polls=%u err=%u d=%u u=%u st=%u xy=%u,%u rep=%u\n",
         (unsigned)now,
         (unsigned)g_tdiag_lv_press,
         (unsigned)g_tdiag_lv_x,
         (unsigned)g_tdiag_lv_y,
         (unsigned)((g_tdiag_lv_press > 0) ? (now - g_tdiag_lv_press_ms)
                                           : 0xffffffffu),
         (int)g_tdiag_last_lv_state,
         (unsigned)k[0], (unsigned)k[1],
         (unsigned)k[2], (unsigned)k[3],
         (unsigned)k[4], (unsigned)k[5],
         (unsigned)k[6], (unsigned)k[7]);
  fflush(stdout);
}

/* 切页内存打点：刷新定时器冲刷完延迟删除后调用（此时旧页已真正释放）。 */

static const char *const g_tab_names[4] =
  { "home", "data", "tasks", "diary" };

static void ui_memlog_flush_print(void)
{
  struct mallinfo mi;

  if (!g_memlog_on || g_memlog_pending_tab < 0)
    {
      return;
    }

  mi = mallinfo();
  printf("[UI-MEM] tab=%s heap_free=%luKB dlt_home=%+ldKB\n",
         g_tab_names[g_memlog_pending_tab],
         (unsigned long)(mi.fordblks / 1024),
         (long)((long)mi.fordblks - g_heap_after_home) / 1024);
  fflush(stdout);
  g_memlog_pending_tab = -1;
}

/* Consume page work on the UI task's 50ms interaction timer. Keeping this
 * behind the 500ms status refresh adds an unrelated wait to voice actions. */
static bool ui_camera_page_tick(void)
{
#if defined(CONFIG_ARCH_BOARD_M5STACK_CORES3) && defined(CONFIG_PLANT_CAMERA_GC0308) && defined(CONFIG_PLANT_CAMERA_CAPTURE)
  if(camera_boot_prepare_busy())return false;
#endif
  if (g_camera_mode_pending >= 0)
    {
      bool mode = g_camera_mode_pending != 0;

      g_camera_mode_pending = -1;
      if (screen_camera_is_active())
        {
          screen_camera_leave_if_active();
        }

      screen_camera_set_lvgl_mode(mode);
      g_page_pending = 1;
      return true;
    }

  if (g_page_pending)
    {
      lv_obj_t *pg;

      g_page_pending = 0;
      if (screen_camera_is_active())
        {
          printf("[UI] camera page already active\n");
          return true;
        }

      pg = screen_camera_create();
      printf("[UI] camera page create=%p\n", pg);
      ui_app_push_screen(pg);
      printf("[UI] camera page pushed depth=%d\n", g_secondary_depth);
      return true;
    }

  return false;
}

#if defined(CONFIG_ARCH_BOARD_M5STACK_CORES3) && defined(CONFIG_PLANT_AI_VOICE)
/* Synthetic input starts through the same UI preparation as a real wake.
 * The pending slot is inert unless explicitly requested by the diagnostic CLI. */
static _Atomic int g_voice_fixture_pending;
int ui_app_voice_fixture_request(int program)
{
  if(program==-1)
    {atomic_store(&g_voice_fixture_pending,-1);voice_service_abort();return 0;}
  if(program<1 || program>3)return -EINVAL;
  int empty=0;
  return atomic_compare_exchange_strong(&g_voice_fixture_pending,&empty,program)?0:-EBUSY;
}
static void ui_voice_fixture_tick(void)
{
  int program=atomic_exchange(&g_voice_fixture_pending,0);
  if(!program)return;
  if(program==-1){voice_service_abort();return;}
  int rc=(voice_service_busy() || voice_service_dialogue_active() ||
          interaction_voice_blocked() || screen_camera_capture_busy())?-EBUSY:0;
  if(!rc)
    {
      if(!screen_voice_is_active() && !screen_camera_is_active())
        {while(g_secondary_depth>0)ui_app_pop_screen();ui_flush_deferred();
         ui_app_push_screen(screen_voice_create());}
      rc=screen_voice_prepare_service();
      /* Cancel may arrive while codec/callback initialization is running. */
      if(!rc && atomic_load(&g_voice_fixture_pending)<0)rc=-ECANCELED;
      if(!rc)rc=voice_service_fixture_start((unsigned)program);
    }
  printf("[Fixture] start program=%u rc=%d nonblocking=1\n",(unsigned)program,rc);
}
#endif

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
/* UI-owned event dispatch must not wait for the 500ms status refresh.
 * All LVGL/page deletion stays on this same UI task. Sensor/cloud/status
 * refreshes retain their existing cadence. */
static void ui_interaction_timer_cb(lv_timer_t *timer)
{
  (void)timer;
  ui_flush_deferred();
  interaction_tick();
  ui_flush_deferred();
  ui_camera_page_tick();
  screen_voice_visibility_tick();
#if defined(CONFIG_ARCH_BOARD_M5STACK_CORES3) && defined(CONFIG_PLANT_AI_VOICE)
  ui_voice_fixture_tick();
  int wake_event = voice_wake_take_event();
  int body_touch = stackchan_body_take_touch();
  if(body_touch && interaction_active())
    {interaction_stop();voice_service_abort();body_touch=0;printf("[Interaction] head touch -> stop\n");}
  if (body_touch || wake_event)
    {
      power_service_activity();power_service_request("wake");power_service_tick(false);
      if(wake_event) printf("[WakeUI] local phrase -> voice\n");
      else printf("[BodyUI] touch source=%s\n", body_touch == 1 ? "physical" : "synthetic");
      if (voice_service_busy())
        {
          if(body_touch)
            { voice_service_abort();printf("[BodyUI] head touch -> cancel voice\n"); }
        }
      else
        {
          if (!screen_voice_is_active() && !screen_camera_is_active())
            {
              while (g_secondary_depth > 0) ui_app_pop_screen();
              ui_flush_deferred();
              ui_app_push_screen(screen_voice_create());
            }
          interaction_pause_motion();
          screen_voice_prepare_service();
          int rc=wake_event?voice_service_wake_start():voice_service_dialogue_start();
          if (wake_event) printf("[WakeUI] start dialogue rc=%d\n", rc);
          else printf("[BodyUI] head touch -> talk rc=%d\n", rc);
        }
    }
#endif

#if defined(CONFIG_ARCH_BOARD_M5STACK_CORES3) && defined(CONFIG_PLANT_AI_VOICE)
  /* Deferred page deletion must finish before the next recording owns audio. */
  voice_service_dialogue_poll(power_service_standby() || interaction_voice_blocked() ||
                             g_business_capture_ticks>0 || screen_camera_capture_busy());
  struct ota_ui_result_s power_ota;ota_service_get(&power_ota);
  power_service_tick(voice_service_busy() || voice_service_dialogue_active() ||
                     interaction_voice_blocked() || g_business_capture_ticks>0 ||
                     screen_camera_capture_busy() || camera_boot_prepare_busy() || power_ota.busy);
#endif
 }
#endif

/* Services own acquisition; this face refresh reads existing caches only. */
static int g_face_battery_cache=-1;
static uint32_t g_face_battery_sampled;
static void ui_face_cache_refresh(void)
{
 int online=-1;
#ifdef CONFIG_PLANT_WIFI_MANAGER
 online=wifi_manager_is_connected()?1:0;
#endif
 screen_voice_set_system(online,face_cached_battery_value(g_face_battery_cache,
                                  lv_tick_get(),g_face_battery_sampled));
 struct sensor_view_s cached;struct plant_state_s ps;
 sensor_service_get_view(&cached);
 bool care=false;
 if(cached.valid){
  plant_state_update_from_sensor(cached.moisture,cached.temp,cached.ec/1000.0f);
  plant_state_get(&ps);care=ps.need_water;
 }
 screen_voice_set_care(care);
}

static void ui_refresh_timer_cb(lv_timer_t *timer)
{
  struct sensor_view_s view;
  struct sensor_history_s hist;
  (void)timer;
  if(power_service_standby())return;
  if (g_audit_pending)
    {
      struct record_task_s task;
      struct record_diary_s entry;
      int task0 = record_task_get(0, &task) ? task.done : -1;
      int diary0 = record_diary_get(0, &entry);
      sensor_service_get_view(&view);
      g_audit_pending = false;
      screen_voice_audit();
      printf("[AuditUI] tab=%d depth=%d tasks=%d done=%d task0=%d diary=%d sensor_valid=%d poll=%d\n",
             g_active_tab, g_secondary_depth, record_task_get_count(),
             record_task_done_count(), task0, record_diary_get_count(),
             view.valid, sensor_service_is_running());
      if (diary0) printf("[AuditUI] latest_diary=%s\n", entry.title);
    }

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  if (g_business_capture_ticks > 0 && screen_camera_is_active())
    {
      if (screen_camera_request_capture() == 0) g_business_capture_ticks=0;
      else if (--g_business_capture_ticks == 0) printf("[Interaction] capture timed out\n");
    }
  int vs=voice_service_get_state();
  if (!g_voice_pill)
    {
      g_voice_pill=lv_label_create(lv_layer_top());
      lv_obj_set_style_text_font(g_voice_pill,TP_FONT_BODY,0);
      lv_obj_set_style_bg_color(g_voice_pill,lv_color_hex(0x204030),0);
      lv_obj_set_style_bg_opa(g_voice_pill,LV_OPA_COVER,0);
      lv_obj_align(g_voice_pill,LV_ALIGN_TOP_MID,0,0);
      lv_obj_set_width(g_voice_pill,300);
      lv_label_set_long_mode(g_voice_pill,LV_LABEL_LONG_WRAP);
    }
  const char *status[]={"","正在听你说...","正在想...","正在说话..."};
  char interaction_status[256];
  bool has_status=interaction_get_status(interaction_status,sizeof(interaction_status));
  const char *feedback=vs==0 && has_status?interaction_status:status[vs>=0 && vs<=3?vs:0];
  if(strcmp(lv_label_get_text(g_voice_pill),feedback))lv_label_set_text(g_voice_pill,feedback);
  if((vs==0 && !has_status) || screen_voice_is_active())lv_obj_add_flag(g_voice_pill,LV_OBJ_FLAG_HIDDEN);
  else
    {
      lv_obj_remove_flag(g_voice_pill,LV_OBJ_FLAG_HIDDEN);
      /* Camera/diagnosis pages are created later on the same top layer.
       * Keep the global listening/thinking feedback above those pages. */
      if(lv_obj_get_child(lv_layer_top(),-1)!=g_voice_pill)lv_obj_move_foreground(g_voice_pill);
    }
#endif

  /* 调试模拟触摸：逐步触发底部 Tab 点击（与真实点按同 LVGL 事件路径） */

  ui_syn_tick();

  /* 先执行延迟删除（上一拍切页/弹回时藏起来的页面，事件派发早已结束） */

  ui_flush_deferred();
  ui_memlog_flush_print();

  /* SD 卡挂载晚于 UI 启动（实测 [SD] Mounted 在 UI 起来之后），所以 SD 全量
   * 字库要在 UI 上下文补挂：每 ~5s 试一次，最多 12 次。⚠ 只能在本任务
   * （字体归属任务）重试，见 zh_font.c 的 fd 隔离说明。 */

  {
    static int font_retry_tick;

    if ((font_retry_tick++ % 10) == 0)
      {
        zh_font_retry_ui();
      }
  }

  /* tdiag armed: auto print every 4 refresh ticks (~2s) */

  if (g_tdiag_on)
    {
      g_tdiag_cnt++;

      if ((g_tdiag_cnt & 3u) == 0)
        {
          ui_tdiag_print();
        }
    }

  /* 调试切页请求：先在 ui_task 上下文执行 Tab 切换并返回，
   * 下一拍再刷新数据（避免在本拍内边删边读新页） */

#ifndef CONFIG_ARCH_BOARD_M5STACK_CORES3
  if (ui_camera_page_tick()) return;
#endif

  if (g_nav_pending >= 0)
    {
      int tab = g_nav_pending;

      g_nav_pending = -1;
      printf("[UI] nav request -> tab %d\n", tab);
      ui_nav_cb(NULL, tab);

      /* 显示优化①（2026-09-22）：这里原来还有一次整屏 invalidate。
       * 实测多余——LVGL 对 HIDDEN 标志的增删本身就会 invalidate 该对象
       * 占据的区域（lv_obj.c: "invalidate the area occupied by the object
       * before we hide it"），而 Tab 页都是整屏对象，所以整屏早就已经
       * 脏了。去掉这次重复的 invalidate，少一次脏区并集计算。 */

      return;
    }

  /* 调试截屏请求：在 ui_task 上下文执行 snapshot → BMP */

  if (g_cap_req)
    {
      g_cap_req = 0;
      if (g_cap_mode == 1)
        {
          FAR FILE *afp = NULL;
          int asave = ui_stdout_to_file("/mnt/sd/ui_ascii.txt", &afp);

          printf("[CapLv] timer: ascii begin win=%d,%d %dx%d bufsz=%lu\n",
                 g_cap_win_x, g_cap_win_y, g_cap_win_w, g_cap_win_h,
                 (unsigned long)g_cap_bufsize);
          fflush(stdout);
          g_cap_status = ui_capture_ascii_do();

          ui_stdout_restore(asave, afp);
        }
      else if (g_cap_mode == 2)
        {
          g_cap_status = ui_capture_ascii_do();
        }
      else
        {
          printf("[CapLv] timer: begin path=%s bufsz=%lu\n", g_cap_path,
                 (unsigned long)g_cap_bufsize);
          fflush(stdout);
          g_cap_status = ui_capture_do();
        }

      printf("[CapLv] timer: done status=%d\n", g_cap_status);
      fflush(stdout);
      g_cap_done = 1;
      return;
    }

  /* 调试对象树转储（plant objs）：在 ui_task 上下文遍历当前可见屏 */

  if (g_dump_req)
    {
      g_dump_req = 0;

      {
        FAR FILE *dfp = NULL;
        int dsave = ui_stdout_to_file("/mnt/sd/ui_objs.txt", &dfp);

        printf("[DumpLv] begin screen=%p\n", (void *)lv_screen_active());
        fflush(stdout);
        g_dump_status = ui_obj_dump_do();
        printf("[DumpLv] done status=%d\n", g_dump_status);
        fflush(stdout);

        ui_stdout_restore(dsave, dfp);
        printf("[DumpLv] 导出完成 status=%d -> /mnt/sd/ui_objs.txt\n",
               g_dump_status);
      }

      fflush(stdout);
      g_dump_done = 1;
      return;
    }

  /* 显示测试图片（plant img show/test）：置顶 lv_image。
   * 注意：不缩放、按原始尺寸显示在左上角 —— 用于验证显示链路颜色
   * （本 LVGL 版本的 RGB565 缩放渲染有错乱嫌疑，会干扰颜色判断）。
   * 数据指针由外部任务提供（rxbuf），生命周期须保持到下次更新。 */

  if (g_img_pending)
    {
      g_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
      g_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
      g_img_dsc.header.w = g_img_w;
      g_img_dsc.header.h = g_img_h;
      g_img_dsc.header.stride = g_img_w * 2;
      g_img_dsc.data_size = g_img_w * g_img_h * 2;
      g_img_dsc.data = g_img_data;

      if (g_img_test == NULL)
        {
          g_img_test = lv_image_create(lv_screen_active());
          lv_obj_remove_style_all(g_img_test);
          lv_obj_set_pos(g_img_test, 0, 0);
          lv_image_set_scale(g_img_test, 256);   /* 1:1，不缩放 */
          lv_obj_set_size(g_img_test, g_img_w, g_img_h);
        }

      lv_image_set_src(g_img_test, &g_img_dsc);
      lv_obj_remove_flag(g_img_test, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(g_img_test);
      g_img_pending = false;
    }

  if(screen_voice_is_active())ui_face_cache_refresh();

  /* 二级页（拍照/诊断/语音）在前台时不刷新 Tab 屏 */

  if (g_secondary_depth > 0)
    {
      return;
    }

  sensor_service_get_view(&view);

  /* 首页顶栏状态（时间 / 日期 / 网络 / 电量）：2026-09-11 恢复显示。

   * 旧实现按"第 0 个子对象是状态栏、其第 3 个子对象是标签"的位置约定取
   * 对象，V3 改版后位置对不上，会对非标签对象调 lv_label_set_text，在
   * lv_free 里触发堆断言（mm_heapmember）。本次改为 screen_home 内部持有
   * 自己的标签指针（s_st_*），只经 screen_home_set_status() 传值，不再按
   * 位置取对象，从根上避开那个断言。

   * 电量每拍读一次 ADC（battery_monitor 本就按 500ms 刷新设计）；读不到
   * 传 -1 → 界面显示 "电池 --%"，不拿 0 冒充真实电量。 */

  if (g_active_tab == PLANT_TAB_HOME && g_screens[PLANT_TAB_HOME] != NULL)
    {
      char date_disp[16];
      int online = 0;
      int bat = -1;

      date_disp[0] = '\0';

      /* "YYYY-MM-DD" → "M月D日"（flash 字库里没有"年"字，故不带年份） */

      if (g_time_valid && g_date_str[0] != '\0')
        {
          int mo = 0;
          int dy = 0;

          if (sscanf(g_date_str, "%*d-%d-%d", &mo, &dy) == 2)
            {
              snprintf(date_disp, sizeof(date_disp), "%d月%d日", mo, dy);
            }
        }

#ifdef CONFIG_PLANT_WIFI_MANAGER
      online = wifi_manager_is_connected() ? 1 : 0;
#endif

#ifdef CONFIG_PLANT_SYSTEM_MONITOR
      if (battery_read_pct(&bat) < 0)
        {
          bat = -1;
        }
#endif

      screen_home_set_status(g_time_valid ? g_time_str : NULL,
                             date_disp[0] != '\0' ? date_disp : NULL,
                             online, bat);
      g_face_battery_cache=bat;g_face_battery_sampled=lv_tick_get();
      screen_voice_set_system(online,bat);
    }


  switch (g_active_tab)
    {
      case PLANT_TAB_HOME:
        {
          struct plant_state_s ps;

          /* 用传感器规则更新健康分/心情（离线） */

          plant_state_update_from_sensor(view.moisture, view.temp,
                                         view.ec / 1000.0f);
          plant_state_get(&ps);
          screen_voice_set_care(view.valid && ps.need_water);

          screen_home_refresh(ps.health_score, (int)view.moisture,
                              (int)view.temp,
                              (int)(view.ph * 10.0f + 0.5f),
                              (int)(view.ec + 0.5f), view.valid ? 1 : 0);
        }
        break;

      case PLANT_TAB_DATA:
        if (g_screens[PLANT_TAB_DATA] != NULL)
          {
            sensor_service_get_history(&hist);
            screen_data_refresh(&view, &hist);
          }
        break;

      default:
        break;
    }
}

/****************************************************************************
 * ui_cap_put16 / ui_cap_put32 — BMP 小端写整数
 ****************************************************************************/

static void ui_cap_put16(FILE *fp, uint16_t v)
{
  fputc(v & 0xff, fp);
  fputc((v >> 8) & 0xff, fp);
}

static void ui_cap_put32(FILE *fp, uint32_t v)
{
  ui_cap_put16(fp, (uint16_t)(v & 0xffffu));
  ui_cap_put16(fp, (uint16_t)((v >> 16) & 0xffffu));
}

/****************************************************************************
 * ui_capture_do — 在 ui_task 上下文执行：LVGL snapshot 当前屏 → BMP 落盘
 *
 * 背景：读回 LCD GRAM（LCDDEVIO_GETRUN）会静默杀掉命令任务且文件为 0 字
 * 节（疑 ST7789 RAMRD/SPI 读路径与 LVGL 写并发有问题），改用 LVGL 自带
 * snapshot：把当前 lv_screen_active 渲染进调用方提供的大缓冲（PSRAM 系统
 * 堆，~154KB）→ 写 BMP（16bpp RGB565，自上而下，BI_BITFIELDS）。
 *
 * 注意：只能在 ui_task（LVGL 单线程）里调用；本函数由 ui_refresh_timer_cb
 * 触发，符合项目"LVGL 对象只在 ui_task 操作"铁律。
 ****************************************************************************/

static int ui_capture_do(void)
{
  lv_draw_buf_t db;
  lv_obj_t *scr;
  FILE *fp;
  int ret;

  scr = ui_visible_root();
  printf("[CapLv] s1 enter scr=%p buf=%p bufsz=%lu\n", (void *)scr,
         g_cap_buf, (unsigned long)g_cap_bufsize);
  fflush(stdout);
  if (scr == NULL || g_cap_buf == NULL ||
      g_cap_bufsize < TP_SCREEN_W * TP_SCREEN_H * 2u + 64u)
    {
      printf("[CapLv] 参数错误（无缓冲或尺寸不够）\n");
      fflush(stdout);
      return -1;
    }

  if (lv_draw_buf_init(&db, TP_SCREEN_W, TP_SCREEN_H,
                       LV_COLOR_FORMAT_RGB565, 0,
                       g_cap_buf, g_cap_bufsize) == LV_RESULT_INVALID)
    {
      printf("[CapLv] draw_buf_init 失败\n");
      fflush(stdout);
      return -1;
    }

  printf("[CapLv] s2 draw_buf_init OK\n");
  fflush(stdout);

  if (lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &db) !=
      LV_RESULT_OK)
    {
      printf("[CapLv] snapshot 失败\n");
      fflush(stdout);
      return -1;
    }

  printf("[CapLv] s3 snapshot OK stride=%lu\n",
         (unsigned long)db.header.stride);
  fflush(stdout);

  fp = fopen(g_cap_path, "wb");
  if (fp == NULL)
    {
      printf("[CapLv] 写 %s 失败\n", g_cap_path);
      fflush(stdout);
      return -1;
    }

  printf("[CapLv] s4 fopen OK -> %s\n", g_cap_path);
  fflush(stdout);

  /* 54B 头 + 12B 掩码；负高度 = 行序自上而下（snapshot 第一行=屏幕顶行） */

  fputc('B', fp);
  fputc('M', fp);
  ui_cap_put32(fp, 66u + (uint32_t)TP_SCREEN_W * TP_SCREEN_H * 2u);
  ui_cap_put16(fp, 0);
  ui_cap_put16(fp, 0);
  ui_cap_put32(fp, 66u);

  ui_cap_put32(fp, 40u);
  ui_cap_put32(fp, TP_SCREEN_W);
  ui_cap_put32(fp, (uint32_t)(0u - TP_SCREEN_H));  /* 负高，top-down */
  ui_cap_put16(fp, 1);
  ui_cap_put16(fp, 16);
  ui_cap_put32(fp, 3u);                      /* BI_BITFIELDS */
  ui_cap_put32(fp, (uint32_t)TP_SCREEN_W * TP_SCREEN_H * 2u);
  ui_cap_put32(fp, 2835u);
  ui_cap_put32(fp, 2835u);
  ui_cap_put32(fp, 0u);
  ui_cap_put32(fp, 0u);

  ui_cap_put32(fp, 0xf800u);
  ui_cap_put32(fp, 0x07e0u);
  ui_cap_put32(fp, 0x001fu);

  /* snapshot 缓冲 stride 可能大于有效行宽（对齐）；逐行去掉行尾填充 */

  {
    const uint8_t *src = (const uint8_t *)db.data;
    uint32_t stride = db.header.stride;
    int y;

    for (y = 0; y < TP_SCREEN_H; y++)
      {
        if (sd_write_aligned(fp, src + (size_t)y * stride,
                             TP_SCREEN_W * 2u) != 0)
          {
            printf("[CapLv] 写文件失败 y=%d\n", y);
            fflush(stdout);
            ret = -1;
            fclose(fp);
            return ret;
          }

        if ((y & 63) == 0)
          {
            printf("[CapLv] s5 row %d/%d\n", y, TP_SCREEN_H);
            fflush(stdout);
          }
      }

    printf("[CapLv] s5 rows done\n");
    fflush(stdout);
  }

  fclose(fp);
  printf("[CapLv] OK → %s\n", g_cap_path);
  fflush(stdout);
  return 0;
}

/****************************************************************************
 * ui_capture_ascii_do — 在 ui_task 上下文执行：LVGL snapshot 当前屏 → 串口
 * 打印 ASCII 点阵（不写文件）。每个输出字符由 HSTEP x VSTEP 个 LCD 像素的
 * 平均亮度映射：亮(白底)=空格、暗(文字笔画)=@。
 *
 * 背景：SD 大批量写 BMP 会卡死（fix4 实测），而"一串方框"又需要像素级目检，
 * 因此把整屏快照按块量化后直接从串口打印，约 120x160 字符（<3s @115200）。
 ****************************************************************************/

static int ui_capture_ascii_do(void)
{
  static const char ramp[10] = " .:-=+*#%@";
  lv_draw_buf_t db;
  lv_obj_t *scr;
  const uint8_t *src;
  uint32_t stride;
  int x0 = g_cap_win_x;
  int y0 = g_cap_win_y;
  int w = g_cap_win_w;
  int h = g_cap_win_h;
  int hstep;
  int vstep;
  int cols;
  int rows;
  int rx;
  int ry;

  scr = ui_visible_root();
  if (scr == NULL || g_cap_buf == NULL ||
      g_cap_bufsize < TP_SCREEN_W * TP_SCREEN_H * 2u + 64u)
    {
      printf("[CapaA] 参数错误\n");
      fflush(stdout);
      return -1;
    }

  if (x0 < 0)
    {
      x0 = 0;
    }

  if (y0 < 0)
    {
      y0 = 0;
    }

  if (x0 + w > TP_SCREEN_W)
    {
      w = TP_SCREEN_W - x0;
    }

  if (y0 + h > TP_SCREEN_H)
    {
      h = TP_SCREEN_H - y0;
    }

  if (w <= 0 || h <= 0)
    {
      x0 = 0;
      y0 = 0;
      w = TP_SCREEN_W;
      h = TP_SCREEN_H;
    }

  if (lv_draw_buf_init(&db, TP_SCREEN_W, TP_SCREEN_H,
                       LV_COLOR_FORMAT_RGB565, 0,
                       g_cap_buf, g_cap_bufsize) == LV_RESULT_INVALID)
    {
      printf("[CapaA] draw_buf_init 失败\n");
      fflush(stdout);
      return -1;
    }

  if (lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &db) !=
      LV_RESULT_OK)
    {
      printf("[CapaA] snapshot 失败\n");
      fflush(stdout);
      return -1;
    }

  src = (const uint8_t *)db.data;
  stride = db.header.stride;

  if (g_cap_mode == 2)
    {
      uint32_t ink = 0;
      uint32_t ink_rows = 0;
      uint32_t q[4] = {0, 0, 0, 0};
      int yy;

      for (yy = y0; yy < y0 + h; yy++)
        {
          const uint8_t *rowp = src + (size_t)yy * stride + (size_t)x0 * 2u;
          uint32_t row_ink = 0;
          int xx;

          for (xx = x0; xx < x0 + w; xx++, rowp += 2)
            {
              uint16_t v = (uint16_t)(rowp[0] | (rowp[1] << 8));
              int r8 = (v >> 8) & 0xf8u;
              int g8 = (v >> 3) & 0xfcu;
              int b8 = (v << 3) & 0xf8u;
              int lum = (r8 * 299 + g8 * 587 + b8 * 114) / 1000;

              if (lum < 210)
                {
                  ink++;
                  row_ink++;
                }
            }

          if (row_ink >= 8)
            {
              ink_rows++;
              q[(yy - y0) * 4 / h] += row_ink;
            }
        }

      g_cap_ink = (int)ink;
      printf("[InkA] region=%d,%d %dx%d ink=%u rows=%u q=%u,%u,%u,%u\n",
             x0, y0, w, h, (unsigned)ink, (unsigned)ink_rows,
             (unsigned)q[0], (unsigned)q[1], (unsigned)q[2], (unsigned)q[3]);
      fflush(stdout);
      return 0;
    }

  hstep = (w > 240) ? 4 : ((w > 120) ? 2 : 1);
  vstep = (h > 160) ? 2 : 1;
  cols = (w + hstep - 1) / hstep;
  rows = (h + vstep - 1) / vstep;

  printf("[CapaA] begin region=%d,%d %dx%d step=%d/%d out=%dx%d\n",
         x0, y0, w, h, hstep, vstep, cols, rows);
  fflush(stdout);

  for (ry = 0; ry < rows; ry++)
    {
      char line[256];
      int lp = 0;

      for (rx = 0; rx < cols; rx++)
        {
          int px0 = x0 + rx * hstep;
          int py0 = y0 + ry * vstep;
          int bx0 = (px0 + hstep > x0 + w) ? (x0 + w) : (px0 + hstep);
          int by0 = (py0 + vstep > y0 + h) ? (y0 + h) : (py0 + vstep);
          int acc = 0;
          int cnt = 0;
          int yy;
          int xx;

          for (yy = py0; yy < by0; yy++)
            {
              const uint8_t *rowp =
                src + (size_t)yy * stride + (size_t)px0 * 2u;

              for (xx = px0; xx < bx0; xx++, rowp += 2)
                {
                  uint16_t v = (uint16_t)(rowp[0] | (rowp[1] << 8));
                  int r8 = (v >> 8) & 0xf8u;
                  int g8 = (v >> 3) & 0xfcu;
                  int b8 = (v << 3) & 0xf8u;
                  int lum = (r8 * 299 + g8 * 587 + b8 * 114) / 1000;

                  acc += lum;
                  cnt++;
                }
            }

          if (cnt > 0)
            {
              int idx = (255 - acc / cnt) * 9 / 255;

              if (idx < 0)
                {
                  idx = 0;
                }

              if (idx > 9)
                {
                  idx = 9;
                }

              line[lp++] = ramp[idx];
            }
        }

      line[lp] = '\0';
      printf("%s\n", line);
      if ((ry & 15) == 15)
        {
          fflush(stdout);
        }
    }

  fflush(stdout);
  return 0;
}

/****************************************************************************
 * ui_obj_dump_* — 调试对象树转储（plant objs）：遍历当前可见屏对象树，
 * 打印每个 label 的几何/字体指针/文本与 UTF-8 码点。用于把"屏幕上的方框"
 * 精确定位到某个 label 的某个字符（字符缺失=LVGL 占位框）。
 ****************************************************************************/

static int g_dump_miss_only;   /* 1 = 只打有缺字的 label（plant objs miss） */

/* 缺字统计：用 label 实际字体查每个码点（ui_task 上下文，
 * 与绘制同一条 fallback 链）。返回无字形的字数（=屏幕上的方框数）。 */

static int ui_label_miss(const lv_font_t *font, const char *s)
{
  const unsigned char *p = (const unsigned char *)s;
  int miss = 0;

  if (s == NULL)
    {
      return 0;
    }

  while (*p != '\0')
    {
      uint32_t cp = 0;

      if (p[0] < 0x80)
        {
          cp = p[0];
          p += 1;
        }
      else if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80)
        {
          cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
          p += 2;
        }
      else if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 &&
               (p[2] & 0xc0) == 0x80)
        {
          cp = ((uint32_t)(p[0] & 0x0f) << 12) |
               ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
          p += 3;
        }
      else if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 &&
               (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80)
        {
          cp = ((uint32_t)(p[0] & 0x07) << 18) |
               ((uint32_t)(p[1] & 0x3f) << 12) |
               ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
          p += 4;
        }
      else
        {
          cp = p[0];
          p += 1;
        }

      /* 控制字符（\n \t 等）不是字形，不能算缺字——
       * LVGL 对它们本来就返回 false，以前会把行尾换行误报成方框。 */

      if (cp >= 0x20 && font != NULL)
        {
          lv_font_glyph_dsc_t dsc;

          memset(&dsc, 0, sizeof(dsc));

          if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0))
            {
              miss++;
            }
        }
    }

  return miss;
}

/* 转储用的静态缓冲。
 *
 * 不放栈上：ui_task 只有 8KB 栈（CONFIG_PLANT_COMPANION_STACKSIZE），
 * 长行缓冲放栈上会挤压渲染余量。这里全部是 BSS，不占栈。 */

static char g_dump_safe[512];        /* label 原文（安全字符版） */
static uint32_t g_dump_cps[256];     /* 逐字码点 */

/* 把 src 拷进 dst（最多 cap-1 字节 + '\0'）：
 *   - 控制字符（<0x20）写成 '.'，免得一条转储记录被换行冲散；
 *   - 只在完整 UTF-8 字符边界上截断。
 *     原来固定 72 字节硬截，长行会被砍在汉字中间，
 *     日志里就出现 "它\uFFFD" 这种看着像"服务器返回坏字节"的假乱码
 *     （2026-09-10 已核实是日志自身的显示上限，不是屏幕/服务器问题）。
 * 返回写入字节数；调用方用 src[返回值] != '\0' 判断是否被截断。 */

static int ui_utf8_copy_safe(char *dst, int cap, const char *src)
{
  const unsigned char *s = (const unsigned char *)src;
  int n = 0;
  int len;
  int i;

  if (dst == NULL || cap <= 0)
    {
      return 0;
    }

  while (*s != '\0')
    {
      len = 1;

      if ((s[0] & 0xe0) == 0xc0)
        {
          len = 2;
        }
      else if ((s[0] & 0xf0) == 0xe0)
        {
          len = 3;
        }
      else if ((s[0] & 0xf8) == 0xf0)
        {
          len = 4;
        }

      /* 续字节必须是 10xxxxxx，否则当单字节坏数据（照原样显示） */

      for (i = 1; i < len; i++)
        {
          if ((s[i] & 0xc0) != 0x80)
            {
              len = 1;
              break;
            }
        }

      if (n + len > cap - 1)
        {
          break;                     /* 放不下：整字放弃，不切半个 */
        }

      if (len == 1)
        {
          dst[n++] = (s[0] >= 0x20) ? (char)s[0] : '.';
        }
      else
        {
          for (i = 0; i < len; i++)
            {
              dst[n++] = (char)s[i];
            }
        }

      s += len;
    }

  dst[n] = '\0';
  return n;
}

static void ui_dump_utf8_cps(const lv_font_t *font, const char *s)
{
  const unsigned char *p = (const unsigned char *)s;
  uint32_t *cps = g_dump_cps;
  int n = 0;
  int i;
  int wsum = 0;

  while (*p != '\0' && n < (int)(sizeof(g_dump_cps) / sizeof(g_dump_cps[0])))
    {
      uint32_t cp = 0;

      if (p[0] < 0x80)
        {
          cp = p[0];
          p += 1;
        }
      else if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80)
        {
          cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
          p += 2;
        }
      else if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 &&
               (p[2] & 0xc0) == 0x80)
        {
          cp = ((uint32_t)(p[0] & 0x0f) << 12) |
               ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
          p += 3;
        }
      else if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 &&
               (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80)
        {
          cp = ((uint32_t)(p[0] & 0x07) << 18) |
               ((uint32_t)(p[1] & 0x3f) << 12) |
               ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
          p += 4;
        }
      else
        {
          cp = p[0];
          p += 1;
        }

      cps[n++] = cp;
    }

  printf(" cps:");
  for (i = 0; i < n; i++)
    {
      lv_font_glyph_dsc_t dsc;
      bool ok = false;

      memset(&dsc, 0, sizeof(dsc));

      if (font != NULL && cps[i] != 0)
        {
          ok = lv_font_get_glyph_dsc(font, &dsc, cps[i], 0);
        }

      /* * = 该字无字形（屏幕上是方框）。
       * (adv) = 该字步进像素：汉字≈字号（20px 字 adv=20）、
       * 半角字符更小；若出现 100+ 就是字宽单位错了
       * （屏幕上是"两字之间很大的空格"）。 */

      if (cps[i] < 0x20)
        {
          /* 换行/制表：正常换行，不是方框（不算进缺字） */

          printf(" [%s]", cps[i] == 0x0A ? "换行" : "控制");
        }
      else if (ok)
        {
          wsum += dsc.adv_w;
          printf(" U+%04X(%u)", (unsigned)cps[i], (unsigned)dsc.adv_w);
        }
      else
        {
          printf(" U+%04X*", (unsigned)cps[i]);
        }
    }

  if (*p != '\0')
    {
      printf(" ...(本行过长，后面还有字未逐字列出)");
    }

  printf("  缺=%d 总宽=%d\n", ui_label_miss(font, s), wsum);
}

static void ui_dump_walk(lv_obj_t *obj, int depth)
{
  int i;
  int child_cnt;

  if (obj == NULL || depth > 24)
    {
      return;
    }

  if (lv_obj_check_type(obj, &lv_label_class))
    {
      const char *txt = lv_label_get_text(obj);
      int n;

      if (txt == NULL)
        {
          txt = "";
        }

      n = ui_utf8_copy_safe(g_dump_safe, (int)sizeof(g_dump_safe), txt);

      const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);

      /* 紧凑模式（plant objs miss）：只打有方框的 label */

      if (!g_dump_miss_only || ui_label_miss(font, txt) > 0)
        {
          printf("[DumpLv] %*slabel xy=%d,%d wh=%d,%d font=%p txt=\"%s\"",
                 depth * 2, "", lv_obj_get_x(obj), lv_obj_get_y(obj),
                 lv_obj_get_width(obj), lv_obj_get_height(obj),
                 (const void *)font, g_dump_safe);

          if (txt[n] != '\0')
            {
              printf(" ...(已省略后面 %d 字节)", (int)strlen(txt) - n);
            }

          fflush(stdout);
          ui_dump_utf8_cps(font, txt);
        }
    }
  else if (!g_dump_miss_only)
    {
      printf("[DumpLv] %*sobj xy=%d,%d wh=%d,%d\n",
             depth * 2, "", lv_obj_get_x(obj), lv_obj_get_y(obj),
             lv_obj_get_width(obj), lv_obj_get_height(obj));
    }

  child_cnt = lv_obj_get_child_cnt(obj);
  for (i = 0; i < child_cnt; i++)
    {
      ui_dump_walk(lv_obj_get_child(obj, i), depth + 1);
    }
}

static int ui_obj_dump_do(void)
{
  printf("[DumpLv] --- top layer ---\n");
  ui_dump_walk(lv_layer_top(), 0);
  printf("[DumpLv] --- active screen ---\n");
  ui_dump_walk(lv_screen_active(), 0);
  return 0;
}

/* 幽灵页清扫（2026-09-26）：lv_screen_active() 的直接子对象必须恰好是
 * 4 个 Tab 屏 + 二级栈里的页 + 调试图（g_img_test）。历史 bug：诊断
 * 结果回传后首页出现可点的幽灵层（诊断页 act/go 卡片泄漏在首页上层，
 * 盖住拍照/浇水/说话按钮），重启才恢复。这里加兜底清扫：凡不在白名单
 * 里的直接子对象一律删除 —— 任何异常路径都留不下可点击残留。
 * 只在 ui_task 的刷新定时器里跑（随 ui_flush_deferred，同一安全上下文）。 */

static void ui_sweep_orphans(void)
{
  lv_obj_t *scr = lv_screen_active();
  int cnt;
  int i;
  int j;

  if (scr == NULL)
    {
      return;
    }

  cnt = (int)lv_obj_get_child_cnt(scr);

  for (i = cnt - 1; i >= 0; i--)   /* 倒序删，索引不漂移 */
    {
      lv_obj_t *child = lv_obj_get_child(scr, (int32_t)i);
      bool keep = false;

      if (child == NULL || child == g_img_test)
        {
          continue;
        }

      for (j = 0; j < PLANT_TAB_COUNT; j++)
        {
          if (child == g_screens[j])
            {
              keep = true;
              break;
            }
        }

      if (!keep)
        {
          for (j = 0; j < g_secondary_depth; j++)
            {
              if (child == g_secondary[j])
                {
                  keep = true;
                  break;
                }
            }
        }

      if (!keep)
        {
          printf("[UI] ⚠ 孤儿页面清扫: %p（幽灵层拦截）\n", (void *)child);
          lv_obj_delete(child);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static bool ui_is_live_child(lv_obj_t *parent, lv_obj_t *obj)
{
  uint32_t i;

  if (parent == NULL || obj == NULL)
    {
      return false;
    }

  for (i = 0; i < lv_obj_get_child_cnt(parent); i++)
    {
      if (lv_obj_get_child(parent, (int32_t)i) == obj)
        {
          return true;
        }
    }

  return false;
}

/* Navigation calls may be interrupted between hide/delete/restore.  Keep the
 * secondary stack authoritative by dropping entries whose LVGL objects no
 * longer exist in either the Tab screen or the top overlay layer. */
static void ui_secondary_prune(void)
{
  lv_obj_t *active = lv_screen_active();
  lv_obj_t *top = lv_layer_top();
  int dst = 0;
  int i;

  for (i = 0; i < g_secondary_depth; i++)
    {
      lv_obj_t *obj = g_secondary[i];

      if (ui_is_live_child(active, obj) || ui_is_live_child(top, obj))
        {
          g_secondary[dst++] = obj;
        }
      else
        {
          printf("[UI] prune stale secondary page %p\n", (void *)obj);
        }
    }

  g_secondary_depth = dst;
}

void ui_app_push_screen(lv_obj_t *scr)
{
  int i;

  ui_secondary_prune();

  if (scr == NULL || g_secondary_depth >= UI_SECONDARY_MAX)
    {
      return;
    }

  /* 隐藏所有 Tab 屏 */

  for (i = 0; i < PLANT_TAB_COUNT; i++)
    {
      if (g_screens[i] != NULL)
        {
          lv_obj_add_flag(g_screens[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  /* 隐藏已存在的二级页 */

  for (i = 0; i < g_secondary_depth; i++)
    {
      lv_obj_add_flag(g_secondary[i], LV_OBJ_FLAG_HIDDEN);
    }

  g_secondary[g_secondary_depth++] = scr;
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_HIDDEN);

  /* 二级页显式提到父对象子链末尾 = 最上面一层，并合并为整屏一次提交。 */

  lv_obj_move_foreground(scr);
  lv_obj_invalidate(lv_layer_top());
  lv_obj_invalidate(lv_screen_active());
}

void ui_app_pop_screen(void)
{
  printf("[UI] pop_screen depth=%d\n", g_secondary_depth);
  int i;

  ui_secondary_prune();

  if (g_secondary_depth <= 0)
    {
      return;
    }

  g_secondary_depth--;
  lv_obj_add_flag(g_secondary[g_secondary_depth], LV_OBJ_FLAG_HIDDEN);

  /* 二级页用完即销毁（用户要求）：释放 LVGL 池，下次进入重新创建。
   * 延迟到下一拍刷新定时器再删：pop 常在二级页自己的"返回"按钮
   * 事件里被调用，同步删除会留下悬垂 indev act_obj（见上注释）。 */

  ui_delete_deferred(g_secondary[g_secondary_depth]);
  g_secondary[g_secondary_depth] = NULL;

  if (g_secondary_depth > 0)
    {
      /* 回到上一层二级页 */

      lv_obj_remove_flag(g_secondary[g_secondary_depth - 1], LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(g_secondary[g_secondary_depth - 1]);
    }
  else
    {
      /* 回 Tab 屏：恢复当前 Tab */

      for (i = 0; i < PLANT_TAB_COUNT; i++)
        {
          if (g_screens[i] != NULL)
            {
              lv_obj_add_flag(g_screens[i], LV_OBJ_FLAG_HIDDEN);
            }
        }

      if (g_screens[g_active_tab] != NULL)
        {
          lv_obj_remove_flag(g_screens[g_active_tab], LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(g_screens[g_active_tab]);
        }
    }

  /* 与 push 对称：恢复的页面置顶并整屏提交。 */

  lv_obj_invalidate(lv_layer_top());
  lv_obj_invalidate(lv_screen_active());
}

#if defined(CONFIG_ARCH_BOARD_M5STACK_CORES3) && defined(CONFIG_PLANT_CAMERA_GC0308) && defined(CONFIG_PLANT_CAMERA_CAPTURE)
#ifdef CONFIG_PLANT_SOIL_SENSOR
static bool g_camera_boot_soil_ready;
#endif
static void *ui_camera_boot_worker(void *arg)
{
  (void)arg;
  clock_t started=clock_systime_ticks();
  int shutdown_rc=-EIO;
  printf("[CamBoot] preparation start; full40 frames, no display/upload\n");
  int rc=camera_boot_prepare(4500,&shutdown_rc);
#ifdef CONFIG_PLANT_SOIL_SENSOR
  if(g_camera_boot_soil_ready && shutdown_rc==0)
    {

      soil_sensor_set_paused(false);
      if(g_ui_running)sensor_service_start(3000,NULL,NULL);
    }
#endif
  printf("[CamBoot] preparation done rc=%d shutdown=%d elapsed_ms=%u\n",rc,shutdown_rc,
         (unsigned)((clock_systime_ticks()-started)*1000/TICK_PER_SEC));
  camera_boot_prepare_release();
  atomic_store_explicit(&g_boot_settled_paint,true,memory_order_release);
  return NULL;
}
static void ui_camera_boot_start(void)
{
  if(camera_boot_prepare_claim()<0)return;
  pthread_t tid;pthread_attr_t attr;
  struct sched_param priority={.sched_priority=90};
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr,4096);
  pthread_attr_setschedpolicy(&attr,SCHED_RR);
  pthread_attr_setschedparam(&attr,&priority);
  pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
  int rc=pthread_create(&tid,&attr,ui_camera_boot_worker,NULL);
  pthread_attr_destroy(&attr);
  if(rc)
    {
#ifdef CONFIG_PLANT_SOIL_SENSOR
      if(g_camera_boot_soil_ready)
        {soil_sensor_set_paused(false);sensor_service_start(3000,NULL,NULL);}
#endif
      camera_boot_prepare_release();
      atomic_store_explicit(&g_boot_settled_paint,true,memory_order_release);
      printf("[CamBoot] worker start failed rc=%d; on-demand preparation retained\n",rc);
    }
}
#endif

void ui_app_stop(void)
{
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  voice_wake_stop();
  stackchan_body_stop();
#endif
  g_ui_running = false;
}

int ui_app_start(void)
{
#if defined(CONFIG_SMP) && CONFIG_SMP_NCPUS > 1
  cpu_set_t task_cpu_set;
  CPU_ZERO(&task_cpu_set);CPU_SET(1,&task_cpu_set);
  int task_affinity_rc=sched_setaffinity(0,sizeof(task_cpu_set),&task_cpu_set);
  printf("[CoreMap] task=ui target_cpu=1 actual_cpu=%d rc=%d\n",sched_getcpu(),task_affinity_rc);
#endif

  lv_nuttx_dsc_t info;
  int i;

  /* 控制台反压保护 v2（2026-09-09）：USB-Serial-JTAG 的 4KB TX 缓冲在没人
   * 读串口时会写满；阻塞式 printf 会把 UI/工作线程永久卡住（表现：拍一拍
   * 后界面冻结 → 看门狗复位重启）。不能直接对 fd1 置 O_NONBLOCK——NuttX
   * 控制台 fd0/fd1 共享同一打开文件描述，会把 stdin 一起变成非阻塞，NSH
   * readline 一空闲就 EAGAIN 退出。正解：为 stdout/stderr 重新 open 一份
   * O_NONBLOCK 的 /dev/console（独立打开文件描述），缓冲满时丢行立即返回；
   * stdin 保持阻塞，NSH 会话不受影响。调试打印 = 尽力而为。 */

  {
    int cfd = open("/dev/console", O_WRONLY | O_NONBLOCK);
    if (cfd < 0)
      {
        cfd = open("/dev/console", O_RDWR | O_NONBLOCK);
      }
    if (cfd >= 0)
      {
        dup2(cfd, 1);
        dup2(cfd, 2);
        close(cfd);
      }
  }

  if (lv_is_initialized())
    {
      printf("[UI] LVGL already initialized!\n");
      return -1;
    }

  lv_init();

  lv_nuttx_dsc_init(&info);
  info.fb_path = "/dev/lcd0";
  info.input_path = "/dev/input0";

  lv_nuttx_init(&info, &g_ui_result);

  if (g_ui_result.disp == NULL)
    {
      printf("[UI] lv_nuttx_init failed (no display)!\n");
      lv_deinit();
      return -1;
    }

  /* Product pages manage their own scrolling.  Never let a drag scroll the
   * display root and move an entire page sideways/vertically. */

  lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

  printf("[UI] LVGL display ready, %dx%d\n",
         lv_display_get_horizontal_resolution(g_ui_result.disp),
         lv_display_get_vertical_resolution(g_ui_result.disp));
  /* 调试用第二路 pointer indev：plant tsim 用它在 ui_task 内模拟真实点按 */

  g_syn_indev = lv_indev_create();
  if (g_syn_indev == NULL)
    {
      printf("[UI] synth indev create FAILED\n");
    }
  else
    {
      lv_indev_set_type(g_syn_indev, LV_INDEV_TYPE_POINTER);
      lv_indev_set_read_cb(g_syn_indev, ui_syn_read_cb);
      lv_indev_set_display(g_syn_indev, g_ui_result.disp);
      printf("[UI] synth indev ready\n");
    }


  /* 触摸设备诊断：确认 LVGL indev 是否创建成功 */

  if (g_ui_result.indev == NULL)
    {
      printf("[UI] WARNING: touch indev NOT created! Check /dev/input0\n");
    }
  else
    {
      printf("[UI] touch indev OK\n");
      lv_indev_set_scroll_limit(g_ui_result.indev, 3);
    }

  /* 设计系统初始化 */

  /* ⚠️ 2026-09-01 字体先行：zh_font_init 绑定主字体（flash 子集，含 ASCII）
   * 并探测 SD 全量字库（/mnt/sd/fonts/plant_zh_*.bin，main() 已挂载 SD）
   * 作为子集外汉字的 LVGL fallback → theme_plant_init 的 TP_FONT_* 拿到的
   * 是主字体指针（fallback 已挂好，见 §14 / zh_font.c）。 */

  zh_font_bind_ui();          /* 绑定 ui_task 为字体归属任务（见 zh_font.c） */
  theme_plant_init();

  /* 植物档案初始化（默认 小绿绿/绿萝/今天） */

  plant_state_init();

  /* 任务/日记记录初始化（/data 持久化，降级仅内存） */

  record_service_init();

  /* 阶段 A：OTA 服务初始化（抓当前固件版本供升级页显示） */

#ifdef CONFIG_ESP32S3_WIFI
  ota_service_init();
#endif

  /* 传感器轮询服务（3B） */

#ifdef CONFIG_PLANT_SOIL_SENSOR
  if (soil_sensor_init(SOIL_SENSOR_DEV_DEFAULT, SOIL_SENSOR_BAUD_DEFAULT) == 0)
    {
#if defined(CONFIG_ARCH_BOARD_M5STACK_CORES3) && defined(CONFIG_PLANT_CAMERA_GC0308) && defined(CONFIG_PLANT_CAMERA_CAPTURE)
      g_camera_boot_soil_ready=true;
      soil_sensor_set_paused(true);
#else
      sensor_service_start(3000, NULL, NULL);
      printf("[UI] sensor_service started (3s poll)\n");
#endif
    }
  else
    {
      printf("[UI] soil_sensor init failed — sensor disabled\n");
    }
#endif

  /* 创建 Tab 屏幕：只常驻 home 页，其余懒加载（省 LVGL 池，
   * 避免 4 页常驻耗尽内存导致绘制时崩溃）。 */

  memset(g_screens, 0, sizeof(g_screens));

  printf("[UI] creating home screen...\n");
  g_screens[PLANT_TAB_HOME] = screen_home_create(ui_nav_cb, NULL);
  printf("[UI] home done\n");
  {
    struct mallinfo mb;

    mb = mallinfo();
    g_heap_after_home = (int)mb.fordblks;
    printf("[UI-MEM] after home: heap_free=%luKB\n",
           (unsigned long)(mb.fordblks / 1024));
  }

  /* 只显示首页 */

  for (i = 0; i < PLANT_TAB_COUNT; i++)
    {
      if (g_screens[i] != NULL)
        {
          lv_obj_add_flag(g_screens[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  lv_obj_remove_flag(g_screens[PLANT_TAB_HOME], LV_OBJ_FLAG_HIDDEN);
  g_active_tab = PLANT_TAB_HOME;

#ifdef CONFIG_PLANT_AI_VOICE
  /* Default child landing page. Creating the face does not start capture.
   * Pending OTA confirmation keeps its existing priority. */
#ifdef CONFIG_ESP32S3_WIFI
  if (!ota_is_pending_verify())
#endif
    {
      ui_app_push_screen(screen_voice_create());
      printf("[UIBoot] page=face dialogue_auto_start=0\n");
    }
#endif

  /* Force one complete 320x240 first paint.  Some CoreS3 panel revisions
   * retain the first scanline(s) in GRAM until a full-screen dirty area is
   * submitted, which otherwise looks like a grey strip above the top bar. */
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(lv_display_get_default());

  /* 阶段 A：OTA 升级后（当前槽 PENDING_VERIFY，等待 confirm）开机自动
   * 弹「升级完成」确认页（当前/上一版本 + 回退警告）。必须在 home 创建
   * 之后调用（ui_app_push_screen 依赖 g_screens 就绪）。不确认则下次
   * 重启 bootloader 自动回退旧槽——保持 CLI 的崩溃回退保护语义。 */

#ifdef CONFIG_ESP32S3_WIFI
  if (ota_is_pending_verify())
    {
      printf("[UI] OTA PENDING_VERIFY: 显示升级完成确认页\n");
      ui_app_push_screen(screen_ota_create(true));
    }
#endif

  /* 注册数据刷新定时器（500ms，ui_task 上下文） */

  lv_timer_create(ui_refresh_timer_cb, 500, NULL);
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  lv_timer_create(ui_interaction_timer_cb, 50, NULL);
#endif
  lv_timer_create(ui_touch_watch_cb, 25, NULL);

#ifdef CONFIG_PLANT_AI_VOICE
  lv_timer_create(ui_voice_wd_timer_cb, 1000, NULL);   /* 语音看门狗 */
#endif

  /* ⚠️ 必须先置运行标志再起时间 worker：worker 循环条件读
   * g_ui_running，若在置位前创建会立刻退出（首轮时间抓不到）。 */

  g_ui_running = true;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  printf("[Body] start rc=%d\n", stackchan_body_start());
  printf("[Wake] start rc=%d\n", voice_wake_start());
#if defined(CONFIG_ARCH_BOARD_M5STACK_CORES3) && defined(CONFIG_PLANT_CAMERA_GC0308) && defined(CONFIG_PLANT_CAMERA_CAPTURE)
  ui_camera_boot_start();
#endif
#endif

  /* 自动执行层（2026-09-12）：配置执行引脚为输出并保持关断。
   * 没接硬件时什么都不做，链路照跑。 */

#ifdef CONFIG_PLANT_AI_MODULE
  actuator_service_init();
#endif

  /* 阶段 B：启动顶栏时间 worker（detached，4KB 堆栈——不能用静态
   * .bss 栈，见 g_time_str 上方注释的内存回归说明）。方案 F：首次
   * 抓取由状态机驱动（未成功 5s 重试），成功后本地走时 + 1h 校准。 */

#ifdef CONFIG_PLANT_AI_MODULE
  {
    pthread_t tid;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* 2026-09-16: 原 4KB 栈会被踩爆。这条线程要跑 socket + 注册/心跳的中文
     * printf，实测崩溃现场：任务名乱码、sched_unlock 断言 lockcount 为 0、
     * 板卡死机（串口真在读时才复现）。与其它 worker 对齐 12KB（堆分配）。 */

    pthread_attr_setstacksize(&attr, 8192);

    if (pthread_create(&tid, &attr, ui_time_worker, NULL) != 0)
      {
        printf("[UI] time worker create failed\n");
      }

    pthread_attr_destroy(&attr);
  }
#endif

  /* 主循环：lv_timer_handler 30fps */

  power_service_init();
  printf("[UI] main loop start\n");

  {
    int loop_cnt = 0;
    uint32_t stat_last = lv_tick_get();
    uint32_t render_sum = 0;
    uint32_t idle_sum = 0;

    while (g_ui_running)
      {
        uint32_t t_start = lv_tick_get();
        /* First paint precedes hardware preparation to keep the face prompt.
         * Commit the complete visible composition once preparation has ended;
         * static header pixels must not depend on a later navigation event.
         * No periodic repaint, page switch, worker LVGL call or fixed delay. */
        if(atomic_exchange_explicit(&g_boot_settled_paint,false,memory_order_acquire))
          {
            lv_obj_invalidate(lv_layer_top());
            lv_obj_invalidate(lv_screen_active());
            printf("[UIBoot] settled_repaint_requested=1 owner=ui_task\n");
          }
        uint32_t idle = lv_timer_handler();
        uint32_t render_ms = lv_tick_get() - t_start;

        render_sum += render_ms;
        idle = idle ? idle : 1;
        idle_sum += idle;
        usleep(idle * 1000);

        loop_cnt++;

        {
          uint32_t now = lv_tick_get();

          if (now - stat_last >= 5000u)
            {
              uint32_t el = now - stat_last;

              printf("[UI] 循环 %u 次/%ums = %.1f Hz（渲染均 %u ms, 空闲均 %u ms）\n",
                     (unsigned)loop_cnt, (unsigned)el,
                     (double)loop_cnt * 1000.0 / (double)(el ? el : 1),
                     (unsigned)(render_sum / (loop_cnt ? loop_cnt : 1)),
                     (unsigned)(idle_sum / (loop_cnt ? loop_cnt : 1)));
              loop_cnt = 0;
              render_sum = 0;
              idle_sum = 0;
              stat_last = now;
            }
        }
      }
  }

#if defined(CONFIG_ARCH_BOARD_M5STACK_CORES3) && defined(CONFIG_PLANT_CAMERA_GC0308) && defined(CONFIG_PLANT_CAMERA_CAPTURE)
  /* Preparation touches no LVGL objects, but finish pin/service cleanup
   * before tearing down the UI application's ownership. */
  while(camera_boot_prepare_busy())usleep(10000);
#endif
  sensor_service_stop();
  lv_nuttx_deinit(&g_ui_result);
  lv_deinit();

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ui_app_show_image(const uint8_t *rgb565, int w, int h)
{
  /* 只存指针+标志，LVGL 对象操作交给 ui_task 定时器（500ms 内生效）。
   * 调用方须保证 rgb565 缓冲区在图片更新前一直有效（如 rxbuf 静态区）。 */

  g_img_data = rgb565;
  g_img_w = w;
  g_img_h = h;
  g_img_pending = true;
}

void ui_app_hide_image(void)
{
  g_img_pending = false;

  /* 删除对象而非仅隐藏：隐藏只加 flag，对象仍留在屏幕上并干扰后续
   * 渲染（曾见 img test 色块残留在首页主内容区）。 */

  if (g_img_test != NULL)
    {
      lv_obj_delete(g_img_test);
      g_img_test = NULL;
    }
}

/* 调试截屏转 ASCII（plant capa [x y w h]）：同 snapshot 流程但不写文件，
 * 快照缓冲由 ui_task 量化成字符后直接打印到串口。x/y/w/h 为可选窗口。 */

int ui_app_capture_ascii(void *buf, size_t bufsize,
                         int x, int y, int w, int h)
{
  int i;

  if (buf == NULL)
    {
      return -1;
    }

  if (w <= 0 || h <= 0)
    {
      x = 0;
      y = 0;
      w = TP_SCREEN_W;
      h = TP_SCREEN_H;
    }

  if (x < 0)
    {
      x = 0;
    }

  if (y < 0)
    {
      y = 0;
    }

  if (x >= TP_SCREEN_W)
    {
      x = 0;
    }

  if (y >= TP_SCREEN_H)
    {
      y = 0;
    }

  if (x + w > TP_SCREEN_W)
    {
      w = TP_SCREEN_W - x;
    }

  if (y + h > TP_SCREEN_H)
    {
      h = TP_SCREEN_H - y;
    }

  g_cap_win_x = x;
  g_cap_win_y = y;
  g_cap_win_w = w;
  g_cap_win_h = h;
  g_cap_buf = buf;
  g_cap_bufsize = bufsize;
  g_cap_done = 0;
  g_cap_status = -1;
  g_cap_mode = 1;
  g_cap_req = 1;

  /* 等 ui_task 的 500ms 刷新定时器执行（同上，留足 60s） */

  for (i = 0; i < 1200 && !g_cap_done; i++)
    {
      usleep(50 * 1000);
    }

  return g_cap_status;
}

int ui_app_capture_ink(int x, int y, int w, int h)
{
  int i;

  g_cap_win_x = x;
  g_cap_win_y = y;
  g_cap_win_w = w;
  g_cap_win_h = h;
  g_cap_buf = malloc(TP_SCREEN_W * TP_SCREEN_H * 2u + 512u);
  if (g_cap_buf == NULL)
    {
      return -1;
    }

  g_cap_bufsize = TP_SCREEN_W * TP_SCREEN_H * 2u + 512u;
  g_cap_done = 0;
  g_cap_status = -1;
  g_cap_mode = 2;
  g_cap_req = 1;

  for (i = 0; i < 1200 && !g_cap_done; i++)
    {
      usleep(50 * 1000);
    }

  free(g_cap_buf);
  g_cap_buf = NULL;
  return (g_cap_status == 0) ? g_cap_ink : -1;
}

/* 调试对象树转储（plant objs）：请求 ui_task 遍历当前可见屏并打印
 * label 文本/坐标/字体/码点。阻塞等待 ui_task 完成后返回。 */

int ui_app_dump_objects(void)
{
  int i;

  g_dump_miss_only = 0;
  g_dump_req = 0;
  g_dump_done = 0;
  g_dump_status = -1;
  g_dump_req = 1;

  /* 等 ui_task 的 500ms 刷新定时器执行（与截屏同款等待，留足 60s） */

  for (i = 0; i < 1200 && !g_dump_done; i++)
    {
      usleep(50 * 1000);
    }

  return g_dump_status;
}

/* 紧凑版：只打印"存在无字形字符"的 label（plant objs miss） */

int ui_app_dump_missing(void)
{
  int i;

  g_dump_miss_only = 1;
  g_dump_req = 0;
  g_dump_done = 0;
  g_dump_status = -1;
  g_dump_req = 1;

  for (i = 0; i < 1200 && !g_dump_done; i++)
    {
      usleep(50 * 1000);
    }

  return g_dump_status;
}

/* 调试：请求切换 4-Tab 页面（plant nav <home|data|tasks|diary>）。
 * 非阻塞：置标志后由 ui_task 的 500ms 刷新定时器实际执行切换。 */

int ui_app_nav_to(int tab_id)
{
  if (tab_id < 0 || tab_id >= PLANT_TAB_COUNT)
    {
      return -1;
    }

  g_nav_pending = tab_id;
  return 0;
}


int ui_app_tsim_start(int cycles)
{
  if (g_syn_indev == NULL)
    {
      return -1;
    }

  if (g_syn_busy)
    {
      return -2;
    }

  g_syn_left = (cycles > 0) ? cycles : 1;
  g_syn_done_cnt = 0;
  g_syn_press_life = 0;
  g_syn_busy = true;
  printf("[SynTap] start cycles=%d\n", g_syn_left);
  fflush(stdout);
  return 0;
}

void ui_app_tsim_stop(void)
{
  g_syn_left = 0;
  g_syn_busy = false;
  g_syn_press_life = 0;
  printf("[SynTap] stopped by user\n");
  fflush(stdout);
}

int ui_app_tsim_left(void)
{
  return g_syn_busy ? g_syn_left : -1;
}


/* 调试单点触摸（plant tap <x> <y>）：在指定坐标注入一次真实点按。
 * 走的是 tsim 那路合成 indev，完整经过 LVGL 输入管线（按下->抬起->CLICKED）
 * —— 用于远程进二级页（拍照页等）验证布局，不必手点屏幕。
 * 只能从任意任务置状态；真正的采样/派发仍在 ui_task 里。 */

int ui_app_tap_at(int x, int y)
{
  if (g_syn_indev == NULL)
    {
      return -1;
    }

  if (g_syn_press_life > 0)
    {
      return -2;      /* 上一次还没松手，稍后再来 */
    }

  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x > 479) x = 479;
  if (y > 319) y = 319;

  g_syn_x = x;
  g_syn_y = y;
  g_syn_press_life = 3;
  g_syn_report_hit = true;
  printf("[SynTap] tap at %d,%d\n", x, y);
  fflush(stdout);
  return 0;
}

int ui_app_scroll_at(int x, int y, int dy, int steps)
{
  if (g_syn_indev == NULL)
    {
      return -1;
    }

  if (g_syn_press_life > 0 || g_scroll_busy)
    {
      return -2;
    }

  if (steps < 2)
    {
      steps = 2;
    }

  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x > 479) x = 479;
  if (y > 319) y = 319;

  g_scroll_x = x;
  g_scroll_y = y;
  g_scroll_dx = 0;
  g_scroll_dy = dy;
  g_scroll_step = 0;
  g_scroll_steps = steps;
  g_scroll_release = false;
  g_scroll_busy = true;
  printf("[SynScroll] start at %d,%d dy=%d steps=%d\n", x, y, dy, steps);
  fflush(stdout);
  return 0;
}

int ui_app_scroll_left(void)
{
  return g_scroll_busy ? (g_scroll_steps - g_scroll_step + 1) : -1;
}

/* plant tdiag on|off|now: mode=1 periodic, mode=0 stop, mode=2 once */

void ui_app_tdiag_ctl(int mode)
{
  if (mode == 1)
    {
      g_tdiag_on = 1;
      g_tdiag_cnt = 0;
    }
  else if (mode == 0)
    {
      g_tdiag_on = 0;
    }

  ui_tdiag_print();
}

/* plant memlog on|off：设置共享标志，由 ui_task 在切页完成时打点。
 * 默认关（不产生任何串口输出）；仅调试时按需开启。 */

void ui_app_memlog_ctl(int on)
{
  g_memlog_on = on ? 1 : 0;
  printf("[MemLog] %s\n", on ? "on (切页完成时打印堆余量)"
                            : "off");
  fflush(stdout);
}

/* 调试截屏（plant cap <name>）：置请求并等待 ui_task 完成 */

int ui_app_capture_to_file(const char *path, void *buf, size_t bufsize)
{
  int i;

  if (path == NULL || buf == NULL)
    {
      return -1;
    }

  strncpy(g_cap_path, path, sizeof(g_cap_path) - 1);
  g_cap_path[sizeof(g_cap_path) - 1] = '\0';
  g_cap_buf = buf;
  g_cap_bufsize = bufsize;
  g_cap_done = 0;
  g_cap_status = -1;
  g_cap_mode = 0;
  g_cap_req = 1;

  /* 等 ui_task 的 500ms 刷新定时器执行（snapshot 整屏软件渲染可能很慢，
   * 留足 60s；若超时则说明定时器没走到 cap 分支） */

  for (i = 0; i < 1200 && !g_cap_done; i++)
    {
      usleep(50 * 1000);
    }

  return g_cap_status;
}


void ui_app_request_camera_page(void)
{
  g_page_pending = 1;
}

void ui_app_request_camera_mode(bool lvgl_mode)
{
  g_camera_mode_pending = lvgl_mode ? 1 : 0;
}

int ui_app_business_page(const char *page)
{
  if (!page) return -EINVAL;
  int tab=-1;
  const char *names[]={"home","data","tasks","diary"};
  for(int i=0;i<4;i++)if(!strcmp(page,names[i]))tab=i;
  if (tab<0 && strcmp(page,"camera"))return -EINVAL;
  if (!strcmp(page,"camera") && screen_camera_is_active())
    {
      while(g_secondary_depth>0 && !screen_camera_is_visible())ui_app_pop_screen();
      ui_flush_deferred();
      if(screen_camera_is_active())return 0;
    }
  while(g_secondary_depth>0)ui_app_pop_screen();
  ui_flush_deferred();
  if (tab>=0)return ui_app_nav_to(tab);
  g_page_pending=1;
  return 0;
}
int ui_app_business_capture(void)
{
  int rc=ui_app_business_page("camera");
  if(rc==0)g_business_capture_ticks=12;
  return rc;
}
