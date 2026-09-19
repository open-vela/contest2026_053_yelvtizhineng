/****************************************************************************
 * apps/plant-companion/ui/screens/screen_camera.c
 *
 * (2) AI拍照 —— 对齐《嵌入式 UI V3 大字版》
 *   顶栏 34 + 取景区 202 + 控件行 82 = 318（本页无底部 Tab）
 *
 * 预览 = `plant cam preview` 的逻辑同步到 UI：
 *   - camera_preview_start(33,45,414,184) 启动后台线程：连续 capture
 *     （静默）→ lcd_put_rgb565 直接把每帧画到取景框内部（绕过 LVGL
 *     渲染器 —— 本 LVGL 版本 lv_image 渲染 RGB565 缩放有 bug，是
 *     彩屏/彩色丝往下刷的根因之一）。
 *   - 画面尺寸 = 绿框内部 (33,45,414,184)，与 3px 绿框严丝合缝。
 *   - 取景框内部不放任何 LVGL 对象（不失效该区）→ LVGL 不会重绘盖掉
 *     画面；lv_nuttx_lcd.c flush_cb 已按列分块 <=320px，消除 rowbuff_be
 *     越界（swap 标志被覆写 → 颜色错乱"彩色丝"的另一个根源）。
 *   - V3 稿在取景框内画的虚线框/四角括弧/扫描线/提示胶囊未搬：直写线程
 *     会把它们当画面内容盖掉，故只保留 3px 绿框 + 外层深色圆角取景区。
 *
 * 拍照（2026-09-10 改）：预览只走 160×120（快，可以糊），拍照时【切
 * 320×240】再上传 —— 160×120 的图服务器交给大模型后只能猜；640×480 是
 * 逐像素输出，亮部红先撞顶会出粉块（实测），320×240 走 2×2 binning 不过曝。
 * 流程：take() 冻结 → preview_stop() 杀线程/DMA → camera_mode_photo()
 * 切几何 → camera_photo_capture() 抓一帧 → worker 零拷贝上传识别。
 * 返回本页时 camera_preview_start() 内部会自动切回 160×120 预览几何。
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include "screen_camera.h"
#include "screen_diagnose.h"
#include "../ui_app.h"
#include "../theme/theme_plant.h"
#include "../widgets/widget_bottomnav.h"

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
#  include "../../components/camera_capture/camera_capture.h"
#endif

#ifdef CONFIG_PLANT_AI_MODULE
#  include "../../services/ai_service.h"
#  include "../../services/server_bridge.h"
#endif

#ifdef CONFIG_PLANT_SOIL_SENSOR
#  include "../../components/sensor_driver/soil_sensor.h"
#  include "../../services/sensor_service.h"
#endif

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
/* nuttx arch（esp32s3_lowputc.h）：DVP 释放后把 IO42/40 路由回 UART0。
 * 头文件在 nuttx 树里，apps 侧直接 extern 声明，避免加 include 路径。 */
extern void esp32s3_uart0_reclaim_pins(void);
#endif

/* 图标（单色 emoji 子集，均在 flash 字库内） */

#define IC_CAMERA   "\xF0\x9F\x93\xB7"      /* U+1F4F7 相机 */
#define IC_SHUTTER  "\xF0\x9F\x93\xB8"      /* U+1F4F8 拍照 */
#define IC_ALBUM    "\xF0\x9F\x96\xBC"      /* U+1F5BC 相框（成长日记）*/
#define IC_RETRY    "\xF0\x9F\x94\x84"      /* U+1F504 重来 */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_status_label;   /* 「AI识别中…」状态文字 */

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
/* 取景框（绿框）内部：viewport(30,42,420,190) + 3px 边框 → 内部
 * x 33..446, y 45..228 = 414×184，与 plant cam preview 直写区域一致
 * （V3 取景区 14,36,452,202，绿框居中其中，画面尺寸与旧版一致不改） */
#define CAM_VIEW_X     33
#define CAM_VIEW_Y     45
#define CAM_VIEW_W     414
#define CAM_VIEW_H     184

static lv_timer_t *s_frame_timer;   /* 帧计数显示定时器（ui_task 上下文） */
static lv_obj_t *s_frame_label;     /* 右上角帧计数（调试用，不挡画面） */
static bool s_cam_stopped;          /* 拍照后已杀摄像头进程（返回需重启）*/
static bool s_cam_busy;             /* ⚠️ 诊断进行中：等待期禁止重启预览 */

/* ⚠️ 3C-2 跨线程结果投递（照抄 voice 页成熟模式）：
 * ai_worker 线程完成服务器诊断后，只能写这个共享缓冲 + 置 flag；
 * s_frame_timer（ui_task 上下文）轮询消费并真正操作 LVGL。
 * 满足「LVGL 对象只在 ui_task 操作」约束 —— 否则 worker 线程
 * 直接 create/push_screen 会与 LVGL 渲染线程竞态 → 花屏/卡死。 */

static char      s_ai_result[1024]; /* 服务器诊断文本（日志/兜底用） */
static struct sb_diag_result_s s_ai_diag;  /* 结构化诊断（worker 写，ui_task 读）*/
static volatile bool s_ai_pending;  /* 有结果待上屏 */
static bool s_ai_failed;             /* ⚠️ 2026-09-09：识别失败如实上屏 */
static char s_ai_error[192];         /* 失败原因（诊断页直接显示） */
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void cam_on_back(lv_event_t *e)
{
  (void)e;

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  /* ⚠️ 必须先冻结预览再切页：预览线程是 ioctl 直写 LCD（绕过 LVGL）的，
   * 页面切走后它还会继续往取景框区域画帧 —— 上一级页面已经画好了又被
   * 摄像头画面盖回去，用户看到的就是"退出拍照页一大片拖尾"。
   * 与 cam_on_gallery 同样的处理（那条路径早就这么做了，返回键漏了）。
   * 冻结后线程只空转，延迟删除里 preview_stop() 的 join 也能立刻返回，
   * 不会再把 UI 任务卡住。 */

  camera_preview_take();
#endif

  ui_app_pop_screen();
}

/* AI 完成回调（ai_worker 线程触发）：
 * ⚠️ 不能直接操作 LVGL！只把结果写入共享缓冲 + 置 pending，
 * s_frame_timer（ui_task）轮询消费后真正上屏。
 * 无服务器结果（失败/演示）时同样投递，由 ui_task 统一走演示兜底。 */

static void cam_ai_done(void *user_data,
                        const struct ai_service_result_s *result)
{
  (void)user_data;

#ifdef CONFIG_PLANT_AI_MODULE
  s_ai_failed = true;
  s_ai_error[0] = '\0';

  if (result != NULL && result->success && result->from_server)
    {
      /* 服务器真实结构化结果：正常走诊断页 */
      s_ai_failed = false;
      s_ai_diag = result->diag;
      snprintf(s_ai_result, sizeof(s_ai_result), "%s",
               result->server_text[0] != '\0' ?
               result->server_text : s_ai_diag.summary);
    }
  else
    {
      /* ⚠️ 2026-09-09 调试要求：失败必须如实提示（原"空=演示兜底"
       * 会显示假绿萝，误导排查）。原因写清楚给诊断页。 */
      s_ai_result[0] = '\0';
      memset(&s_ai_diag, 0, sizeof(s_ai_diag));
      s_ai_diag.match = -1;
      s_ai_diag.health_score = -1;

      if (result != NULL && result->server_err != 0)
        {
          /* ⚠️ 2026-09-09：错误码翻译成人话——调试期最常见的 -107 是
           * “重启后 WiFi/服务器地址丢失”，新固件已支持开机自动恢复。 */
          switch (result->server_err)
            {
              case -ENOTCONN:
                snprintf(s_ai_error, sizeof(s_ai_error),
                         "识别失败：设备还没连上服务器（WiFi/地址丢失）。"
                         "新固件会开机自动恢复，稍后重拍即可。");
                break;
              case -ECONNREFUSED:
                snprintf(s_ai_error, sizeof(s_ai_error),
                         "识别失败：服务器拒绝连接，"
                         "请确认电脑上的服务器程序已启动。");
                break;
              case -ETIMEDOUT:
                snprintf(s_ai_error, sizeof(s_ai_error),
                         "识别失败：请求服务器超时，"
                         "请检查网络后重试。");
                break;
              case -EHOSTUNREACH:
              case -ENETUNREACH:
                snprintf(s_ai_error, sizeof(s_ai_error),
                         "识别失败：网络不可达，"
                         "请检查 WiFi 与服务器地址。");
                break;
              default:
                snprintf(s_ai_error, sizeof(s_ai_error),
                         "识别失败：服务器请求未成功（错误码 %d），"
                         "请检查 WiFi/服务器地址后重试。",
                         result->server_err);
                break;
            }
        }
      else if (result != NULL && result->success &&
               !result->from_server)
        {
          snprintf(s_ai_error, sizeof(s_ai_error),
                   "识别失败：未拿到服务器识别结果，"
                   "调试期请确认服务器可用后重试。");
        }
      else
        {
          snprintf(s_ai_error, sizeof(s_ai_error),
                   "识别失败：没有取到可识别的画面，请重试。");
        }
    }

  s_ai_pending = true;
#else
  s_ai_failed = true;
  s_ai_result[0] = '\0';
  snprintf(s_ai_error, sizeof(s_ai_error),
           "识别不可用：AI 模块未编译");
  s_ai_pending = true;
#endif
}

/* 拍照：预览线程已持续直写取景框；拍照 = 定格当前帧 + 杀掉摄像头进程
 * （用户要求：诊断期间摄像头进程不应继续跑，省 DMA/线程资源）。
 * 流程：取帧（rxbuf 静态 DRAM，stop 不清零）→ camera_preview_stop()
 * 杀线程+DMA → worker 借用 rxbuf 上传诊断 → 诊断页显示。
 * 从诊断页返回本页时，screen_camera_create 重新启动预览。 */

static void cam_on_shoot(lv_event_t *e)
{
  (void)e;

#ifdef CONFIG_PLANT_CAMERA_CAPTURE

  /* ⚠️ 2026-09-10 根因修复（用户报"上传的图偶发偏色、高光去饱和时有时无"）：
   * 同一块 rxbuf 被零拷贝借用上传，worker 还在读的时候再按一次拍照，会
   * 【先抓帧覆盖 rxbuf】、然后 analyze_async 才返回 -EBUSY —— 覆盖已经发生，
   * 发给服务器的是"半张旧帧 + 半张新帧"。所以忙的时候必须在【碰摄像头之前】
   * 就拒掉，不能先抓再让下层报忙。 */

#  ifdef CONFIG_PLANT_AI_MODULE
  if (s_cam_busy || ai_service_busy())
    {
      if (s_status_label != NULL)
        {
          lv_label_set_text(s_status_label, "识别中，请稍候…");
        }

      return;
    }
#  endif

  if (s_status_label != NULL)
    {
      lv_label_set_text(s_status_label, "识别中…");
    }

#  ifdef CONFIG_PLANT_AI_MODULE
  {
    /* 真实链路（零拷贝 + 杀进程 + 高清）：
     * 1) take()（freeze）：预览线程退出 capture 路径进入空转
     *    （否则 stop 的 join 会等当前 capture 超时 ~10s，UI 卡）；
     * 2) camera_preview_stop() 杀掉预览线程 + DMA —— 必须先停再切几何，
     *    有并发取帧时改分辨率会拿到半帧；
     * 3) camera_mode_photo() 切到 320×240（约 0.15s，不重跑软复位）；
     * 4) camera_photo_capture() 抓一帧：帧本体留在驱动 rxbuf；
     * 5) worker 借用 rxbuf（borrowed=true）上传 —— 设备端堆只有 ~17KB，
     *    无论如何都 malloc 不出 153600B，零拷贝是唯一路径；
     * ⚠️ s_cam_busy=true：诊断等待期（上传 + 服务器识别 10-40s）禁止
     * frame_timer 重启预览 —— 否则预览会覆盖 worker 正在读的帧。 */
    const uint8_t *fb = NULL;
    size_t flen = 0;
    int ret;

    camera_preview_take();          /* 先冻结：线程进入空转，stop 不等待 */
    camera_preview_stop();
    s_cam_stopped = true;
    s_cam_busy = true;

    ret = camera_mode_photo();
    if (ret == 0)
      {
        ret = camera_photo_capture(&fb, &flen);
      }

    if (ret == 0 && fb != NULL && flen != 0)
      {
        ret = ai_service_analyze_async(fb, flen, true, NULL,
                                       cam_ai_done, NULL);
        if (ret != 0)
          {
            if (s_status_label != NULL)
              {
                lv_label_set_text(s_status_label, "识别忙，稍后再试");
              }
          }
      }
    else
      {
        /* 拍照/切模式失败：如实提示，不编答案（用户要求） */

        if (s_status_label != NULL)
          {
            lv_label_set_text(s_status_label, "拍照失败，请重试");
          }

        cam_ai_done(NULL, NULL);
      }
  }
#  else
  camera_preview_take();
  camera_preview_stop();
  s_cam_stopped = true;
  s_cam_busy = true;
  cam_ai_done(NULL, NULL);
#  endif
#else
  cam_ai_done(NULL, NULL);
#endif
}

/* 页面销毁（返回）：停止预览线程 + 删除帧计数定时器。
 * ⚠️ 3C-2：必须清掉未消费的 AI 结果 + 停定时器 —— 否则页面已删除，
 * timer 仍访问 s_frame_label/s_ai_result → use-after-free（voice 页
 * 同款 PANIC 教训）。 */

static void cam_on_delete(lv_event_t *e)
{
  (void)e;

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  camera_preview_stop();

  /* M2：完整关闭摄像头（停 CAM/DMA + SCCB 软复位 OV3660）；传感器被软复位
   * 后不再推挽驱动 DVP 输出，下面的 reclaim 才能把 IO42/40 收回给 UART0。 */
  camera_shutdown();

  s_ai_pending = false;
  s_ai_failed = false;
  s_ai_error[0] = '\0';
  s_ai_result[0] = '\0';
  memset(&s_ai_diag, 0, sizeof(s_ai_diag));
  s_ai_diag.match = -1;
  s_ai_diag.health_score = -1;
  s_cam_stopped = false;
  s_cam_busy = false;

  if (s_frame_timer != NULL)
    {
      lv_timer_delete(s_frame_timer);
      s_frame_timer = NULL;
    }

  s_frame_label = NULL;
#endif

#ifdef CONFIG_PLANT_SOIL_SENSOR
  /* M2：IO42/40 切回 UART0 → 解除土壤挂起 → 恢复 3s 轮询（含 30s 上云节流） */
  esp32s3_uart0_reclaim_pins();
  soil_sensor_set_paused(false);
  sensor_service_start(3000, NULL, NULL);
#endif
}

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
/* LVGL 定时器：ui_task 上下文刷新右上角帧计数（只失效标签自身小区域，
 * 不碰取景框内部；同时证明预览线程活着）。
 * 附带职责：拍照冻结（take）后，从诊断页返回本页（可见）时自动恢复
 * 实时预览 —— 冻结必须在页面隐藏期间保持（否则线程直写会撕坏盖在
 * 上层的诊断页内容）。 */

static void frame_timer_cb(lv_timer_t *timer)
{
  (void)timer;

#ifdef CONFIG_PLANT_AI_MODULE
  /* 3C-2：消费跨线程诊断结果（worker 写 s_ai_pending，ui_task 上屏）。
   * 先于帧计数处理：拍照后首个 tick 即跳诊断页。 */

  if (s_ai_pending)
    {
      lv_obj_t *diag;
      const char *name;
      char latin[96];
      char tags[64];
      char issue_desc[288];
      char advice[288];
      char local[1024];
      const char *box_title = NULL;
      bool has_issue = true;
      int match_pct;

      s_ai_pending = false;   /* 消费（先清，防 timer 重入） */
      snprintf(local, sizeof(local), "%s", s_ai_result);

      printf("[Cam] 诊断结果到达，准备上屏 (len=%d)\n",
             (int)strlen(local));
      if (local[0] != '\0')
        {
          /* 调试：服务器返回原话（截断）也打串口，方便对照屏显 */
          printf("[Cam] 服务器回复: %.240s\n", local);
        }

      if (s_ai_failed)
        {
          /* ⚠️ 2026-09-09 调试要求：失败明确提示，且内容框必须显示
           * （此前 has_issue=false 会把失败原因一起藏掉，用户只看到
           * “识别失败”四个字）。 */
          name = "识别失败";
          latin[0] = '\0';
          match_pct = 0;
          box_title = "⚠ 识别失败";
          snprintf(issue_desc, sizeof(issue_desc), "%s",
                   s_ai_error[0] != '\0' ? s_ai_error : "服务器没有返回结果");
          snprintf(advice, sizeof(advice),
                   "可稍后重拍一次；具体原因请看串口日志。");
          snprintf(tags, sizeof(tags), "#需重试");
          has_issue = true;
          s_ai_failed = false;
        }
      else if (s_ai_diag.name[0] != '\0')
        {
          /* 服务器识别出植物：结构化结果正常展示（问题/小结都显示） */
          char lvl[24];

          name = s_ai_diag.name;
          snprintf(latin, sizeof(latin), "%s", s_ai_diag.latin);
          match_pct = (s_ai_diag.match >= 0) ? s_ai_diag.match : 0;

          if (s_ai_diag.health_score >= 0)
            {
              if (s_ai_diag.health_score >= 80)
                {
                  strlcpy(lvl, "状态良好", sizeof(lvl));
                }
              else if (s_ai_diag.health_score >= 60)
                {
                  strlcpy(lvl, "需要留意", sizeof(lvl));
                }
              else
                {
                  strlcpy(lvl, "需要帮助", sizeof(lvl));
                }

              snprintf(tags, sizeof(tags), "健康 %d 分 · %s",
                       s_ai_diag.health_score, lvl);
            }
          else
            {
              snprintf(tags, sizeof(tags), "#AI体检 #诊断");
            }

          if (s_ai_diag.issue[0] != '\0')
            {
              box_title = "⚠ 发现小问题";
              snprintf(issue_desc, sizeof(issue_desc), "%s", s_ai_diag.issue);

              if (s_ai_diag.advice[0] != '\0')
                {
                  snprintf(advice, sizeof(advice), "%s", s_ai_diag.advice);
                }
              else if (s_ai_diag.summary[0] != '\0')
                {
                  snprintf(advice, sizeof(advice), "%s", s_ai_diag.summary);
                }
              else
                {
                  advice[0] = '\0';
                }

              has_issue = true;
            }
          else if (s_ai_diag.summary[0] != '\0')
            {
              box_title = "💬 服务器小结";
              snprintf(issue_desc, sizeof(issue_desc), "%s",
                       s_ai_diag.summary);
              snprintf(advice, sizeof(advice), "%s", s_ai_diag.advice);
              has_issue = true;
            }
          else if (s_ai_diag.advice[0] != '\0')
            {
              box_title = "💬 服务器小结";
              snprintf(issue_desc, sizeof(issue_desc), "%s",
                       s_ai_diag.advice);
              advice[0] = '\0';
              has_issue = true;
            }
          else
            {
              issue_desc[0] = '\0';
              advice[0] = '\0';
              has_issue = false;
            }
        }
      else
        {
          /* 服务器应答了但没识别出植物（name 为空）：原话照实展示 */
          name = "未能识别";
          latin[0] = '\0';
          match_pct = (s_ai_diag.match >= 0) ? s_ai_diag.match : 0;
          snprintf(tags, sizeof(tags), "#AI未识别");
          box_title = "🤔 服务器回复";

          if (s_ai_diag.summary[0] != '\0')
            {
              snprintf(issue_desc, sizeof(issue_desc), "%s",
                       s_ai_diag.summary);
              snprintf(advice, sizeof(advice), "%s", s_ai_diag.advice);
            }
          else if (s_ai_diag.advice[0] != '\0')
            {
              snprintf(issue_desc, sizeof(issue_desc), "%s",
                       s_ai_diag.advice);
              advice[0] = '\0';
            }
          else if (s_ai_diag.issue[0] != '\0')
            {
              snprintf(issue_desc, sizeof(issue_desc), "%s",
                       s_ai_diag.issue);
              advice[0] = '\0';
            }
          else
            {
              snprintf(issue_desc, sizeof(issue_desc), "%s", local);
              advice[0] = '\0';
            }

          has_issue = (issue_desc[0] != '\0');
        }

      /* ⚠️ 2026-08-31 修复（用户指正）：摄像头画面是 ioctl 直写 LCD
       * （LCDDEVIO_PUTAREA，绕过 LVGL 渲染管线），与 LVGL 是两套逻辑
       * —— LVGL 不认为取景框区域脏，invalidate 依赖 LVGL 重绘不可靠。
       * 正解：物理填充【在诊断页创建之前】——复用 rxbuf（worker 已
       * 上传完，不再借用）填成诊断页背景色（0xeff6ff），
       * lcd_put_rgb565 走 ioctl 直写取景框区域 → 与摄像头同通道
       * 物理覆盖残留。之后诊断页 push 的重绘背景色与填充一致，无冲突。
       * （lcd_put_rgb565 逐行分块 ≤320px，不触发 st7789 rowbuff_be
       * 越界。） */
      {
        extern uint8_t *esp32s3_cam_dvp_get_frame(void);
        uint8_t *fb = esp32s3_cam_dvp_get_frame();
        uint16_t bg = ((0xEF >> 3) << 11) | ((0xF6 >> 2) << 5) |
                      (0xFF >> 3);   /* 0xeff6ff → RGB565 LE */
        int i;

        if (fb != NULL)
          {
            uint16_t *p = (uint16_t *)fb;

            for (i = 0; i < CAM_PREVIEW_W * CAM_PREVIEW_H; i++)
              {
                p[i] = bg;
              }

            lcd_put_rgb565(fb, CAM_PREVIEW_W, CAM_PREVIEW_H,
                           CAM_VIEW_X, CAM_VIEW_Y, CAM_VIEW_W, CAM_VIEW_H);
            printf("[Cam] 取景框已物理填充背景色\n");
          }
        else
          {
            printf("[Cam] 填充失败: rxbuf 不可用\n");
          }
      }

      diag = screen_diagnose_create();
      screen_diagnose_set(diag, name, latin, match_pct, tags, box_title,
                          has_issue, issue_desc, advice);
      ui_app_push_screen(diag);

      /* 双保险：也让 LVGL 重绘诊断页（覆盖取景框外的所有残留） */

      lv_obj_invalidate(diag);
      lv_obj_invalidate(lv_screen_active());

      /* 诊断页已显示（拍照页被隐藏）→ 解除 busy，允许返回本页时重启
       * 预览（此时拍照页 HIDDEN，frame_timer 不会误重启）。 */
      s_cam_busy = false;

      if (s_status_label != NULL)
        {
          lv_label_set_text(s_status_label, "识别完成 ✅");
        }
    }
#endif

  if (s_frame_label != NULL)
    {
      lv_obj_t *scr = lv_obj_get_parent(s_frame_label);

    /* 拍照后杀过进程（s_cam_stopped）→ 返回本页时重启预览；
     * 未杀过（首次进入/未拍照）→ resume 即可。
     * ⚠️ s_cam_busy（诊断进行中）禁止重启：worker 上传期间拍照页
     * 仍可见，误重启会把摄像头画面盖回取景框（用户反馈）。 */

    if (!lv_obj_has_flag(scr, LV_OBJ_FLAG_HIDDEN) && !s_cam_busy)
      {
        if (s_cam_stopped)
          {
            camera_preview_start(CAM_VIEW_X, CAM_VIEW_Y,
                                 CAM_VIEW_W, CAM_VIEW_H);
            s_cam_stopped = false;
          }
        else
          {
            camera_preview_resume();
          }
      }

      lv_label_set_text_fmt(s_frame_label, "帧 %d",
                            camera_preview_get_frame_index());
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* 左键（相框）：这次拍到的记录去「成长日记」页看 —— 先冻结预览再切页。
 * 预览线程是直写 LCD 的，页面切走后继续直写会把日记页画花，必须先停。 */

static void cam_on_gallery(lv_event_t *e)
{
  (void)e;

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  camera_preview_take();        /* 冻结：线程进入空转，不再写 LCD */
#endif

  ui_app_pop_screen();          /* 弹出本页（延迟删除 → cam_on_delete 停线程） */
  ui_app_nav_to(PLANT_TAB_DIARY);
}

/* 右键（重来）：重新取景 —— 清掉上一轮状态并重启实时预览。
 * 上传/识别进行中禁止重启（会覆盖 worker 正在读的那一帧）。 */

static void cam_on_retry(lv_event_t *e)
{
  (void)e;

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
#  ifdef CONFIG_PLANT_AI_MODULE
  if (s_cam_busy || ai_service_busy())
    {
      if (s_status_label != NULL)
        {
          lv_label_set_text(s_status_label, "识别中，请稍候…");
        }

      return;
    }
#  endif

  camera_preview_take();
  camera_preview_start(CAM_VIEW_X, CAM_VIEW_Y, CAM_VIEW_W, CAM_VIEW_H);
  s_cam_stopped = false;

  if (s_status_label != NULL)
    {
      lv_label_set_text(s_status_label, "AI识别中");
    }
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_camera_create(void)
{
  lv_obj_t *scr = lv_obj_create(lv_screen_active());
  lv_obj_t *top;
  lv_obj_t *back;
  lv_obj_t *title;
  lv_obj_t *ai_tag;
  lv_obj_t *viewfinder;
  lv_obj_t *viewport;
  lv_obj_t *ctl;
  lv_obj_t *btn;
  lv_obj_t *shoot;

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 320);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, lv_color_hex(TP_CLR_CAMERA_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  /* ⚠️ 防残留：上次会话（页面销毁时 worker 未完成）可能留下 pending，
   * 新建页面时必须清掉，否则一进页面就跳诊断页。 */

  s_ai_pending = false;
  s_ai_failed = false;
  s_ai_error[0] = '\0';
  s_ai_result[0] = '\0';
  memset(&s_ai_diag, 0, sizeof(s_ai_diag));
  s_ai_diag.match = -1;
  s_ai_diag.health_score = -1;
  s_cam_stopped = false;
  s_cam_busy = false;
#endif

  /* ── 顶栏（34）：✕ 返回 + 📷 拍一拍 + AI 状态胶囊 ───────────── */

  top = lv_obj_create(scr);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, 480, 34);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_style_pad_hor(top, 6, 0);
  lv_obj_set_style_pad_column(top, 7, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  back = lv_button_create(top);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, 44, 30);
  lv_obj_set_style_text_color(back, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(back, TP_FONT_TITLE, 0);
  theme_btn_set_text(back, LV_SYMBOL_CLOSE);
  lv_obj_add_event_cb(back, cam_on_back, LV_EVENT_CLICKED, NULL);

  title = lv_label_create(top);
  lv_label_set_text(title, IC_CAMERA " 拍一拍");
  lv_obj_set_style_text_font(title, TP_FONT_TITLE, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

  btn = lv_obj_create(top);          /* 撑开：把 AI 胶囊顶到最右 */
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 4, 4);
  lv_obj_set_flex_grow(btn, 1);

  ai_tag = lv_label_create(top);
  lv_label_set_text(ai_tag, "AI识别中");
  lv_obj_set_style_text_font(ai_tag, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(ai_tag, lv_color_hex(0x34d399), 0);
  lv_obj_set_style_bg_color(ai_tag, lv_color_hex(0x14333a), 0);
  lv_obj_set_style_bg_opa(ai_tag, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(ai_tag, 8, 0);
  lv_obj_set_style_pad_hor(ai_tag, 9, 0);
  lv_obj_set_style_pad_ver(ai_tag, 3, 0);
  s_status_label = ai_tag;

  /* ── 取景区（202）：外层深色圆角 + 内层 3px 绿框 ─────────────
   * 绿框内部 (33,45,414,184) 只给直写预览用，不放任何 LVGL 对象。 */

  viewfinder = lv_obj_create(scr);
  lv_obj_remove_style_all(viewfinder);
  lv_obj_set_size(viewfinder, 452, 202);
  lv_obj_set_pos(viewfinder, 14, 36);
  lv_obj_set_style_radius(viewfinder, TP_RADIUS_TILE, 0);
  lv_obj_set_style_clip_corner(viewfinder, true, 0);
  lv_obj_set_style_bg_color(viewfinder, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_bg_opa(viewfinder, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(viewfinder, lv_color_hex(0x334155), 0);
  lv_obj_set_style_bg_grad_dir(viewfinder, LV_GRAD_DIR_VER, 0);

  viewport = lv_obj_create(scr);
  lv_obj_remove_style_all(viewport);
  lv_obj_set_size(viewport, 420, 190);
  lv_obj_set_pos(viewport, 30, 42);
  lv_obj_set_style_border_width(viewport, 3, 0);
  lv_obj_set_style_border_color(viewport, lv_color_hex(0x34d399), 0);
  lv_obj_set_style_border_side(viewport, LV_BORDER_SIDE_FULL, 0);
  lv_obj_set_style_radius(viewport, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_bg_color(viewport, lv_color_hex(TP_CLR_CAMERA_BG), 0);
  lv_obj_set_style_bg_opa(viewport, LV_OPA_COVER, 0);

  /* ── 控件行（82）：相框（成长日记） / 拍照 / 重来 ───────────── */

  ctl = lv_obj_create(scr);
  lv_obj_remove_style_all(ctl);
  lv_obj_set_size(ctl, 480, 82);
  lv_obj_set_pos(ctl, 0, 238);

  btn = lv_button_create(ctl);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, TP_TOUCH_MIN + 2, TP_TOUCH_MIN + 2);
  lv_obj_set_pos(btn, 66, 16);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x202a3b), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(0xe2e8f0), 0);
  lv_obj_set_style_text_font(btn, TP_ICON_M, 0);
  theme_btn_set_text(btn, IC_ALBUM);
  lv_obj_add_event_cb(btn, cam_on_gallery, LV_EVENT_CLICKED, NULL);

  shoot = lv_button_create(ctl);
  lv_obj_remove_style_all(shoot);
  lv_obj_set_size(shoot, 60, 60);
  lv_obj_set_pos(shoot, 210, 11);
  lv_obj_set_style_radius(shoot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(shoot, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(shoot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(shoot, 4, 0);
  lv_obj_set_style_border_color(shoot, lv_color_hex(0x34d399), 0);
  lv_obj_set_style_border_side(shoot, LV_BORDER_SIDE_FULL, 0);
  lv_obj_set_style_text_color(shoot, lv_color_hex(TP_CLR_GREEN), 0);
  lv_obj_set_style_text_font(shoot, TP_ICON_L, 0);
  theme_btn_set_text(shoot, IC_SHUTTER);
  lv_obj_add_event_cb(shoot, cam_on_shoot, LV_EVENT_CLICKED, NULL);

  btn = lv_button_create(ctl);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, TP_TOUCH_MIN + 2, TP_TOUCH_MIN + 2);
  lv_obj_set_pos(btn, 364, 16);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x202a3b), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(0xe2e8f0), 0);
  lv_obj_set_style_text_font(btn, TP_ICON_M, 0);
  theme_btn_set_text(btn, IC_RETRY);
  lv_obj_add_event_cb(btn, cam_on_retry, LV_EVENT_CLICKED, NULL);

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  /* 手机相机式预览（直写 LCD，= plant cam preview 的逻辑）：
   * 后台线程 capture → lcd_put_rgb565 直写取景框内部；无 LVGL 对象
   * 与之竞争，无 lv_image 渲染 bug。 */

  /* M2：进拍照页 → 土壤让出 IO42/40：先停 3s 轮询（join 轮询线程，
   * 避免与 DVP 抢线），再挂起手动读；随后 camera_init 把引脚配成 DVP 输入 */

#ifdef CONFIG_PLANT_SOIL_SENSOR
  sensor_service_stop();
  soil_sensor_set_paused(true);
#endif

  if (camera_init() == 0)
    {
      /* 每次都调 camera_mode_preview()：已配置且已在预览模式 → 零开销
       * 立即返回；摄像头被 camera_shutdown() 软复位过（上次离页时）→
       * 这里会自动重跑完整 RGB565 配置（含 1s 收敛等待），
       * 保证第 2、3… 次进页面画面依然正常。 */

      (void)camera_mode_preview();

      camera_preview_start(CAM_VIEW_X, CAM_VIEW_Y,
                           CAM_VIEW_W, CAM_VIEW_H);

      /* 帧计数（调试辅助）：放在控件行左侧空白处，不进取景区 */

      s_frame_label = lv_label_create(scr);
      lv_label_set_text(s_frame_label, "帧 0");
      lv_obj_set_style_text_font(s_frame_label, TP_FONT_CAPTION, 0);
      lv_obj_set_style_text_color(s_frame_label, lv_color_hex(0x475569), 0);
      lv_obj_set_pos(s_frame_label, 6, 271);

      if (s_frame_timer == NULL)
        {
          s_frame_timer = lv_timer_create(frame_timer_cb, 500, NULL);
        }
    }
  else
    {
      /* camera_init 失败（无摄像头/探测失败）：引脚未交付摄像头，
       * 立即把 IO42/40 交还土壤并恢复轮询 */

#ifdef CONFIG_PLANT_SOIL_SENSOR
      esp32s3_uart0_reclaim_pins();
      soil_sensor_set_paused(false);
      sensor_service_start(3000, NULL, NULL);
#endif
    }
#endif

  /* 页面销毁（返回）时停止预览线程 */

  lv_obj_add_event_cb(scr, cam_on_delete, LV_EVENT_DELETE, NULL);

  return scr;
}
