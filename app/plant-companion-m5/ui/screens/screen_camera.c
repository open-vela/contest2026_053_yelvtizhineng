#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
#include "../../services/interaction_service.h"
#include "../../services/voice_service.h"
#endif
/****************************************************************************
 * apps/plant-companion/ui/screens/screen_camera.c
 *
 * (2) AI拍照 —— 对齐《嵌入式 UI V3 大字版》
 *   顶栏 34 + 取景区 202 + 控件行 82 = 318（本页无底部 Tab）
 *
 * 预览=直接取 OV3660 1024×768 JPEG，板端 TJpgDec 解码到 512×384 RGB565，
 * 再由 lcd_put_rgb565_cover 直写取景框（绕过 LVGL 图像渲染器）。
 *   - 画面尺寸 = 绿框内部 (33,45,414,184)，与 3px 绿框严丝合缝。
 *   - 取景框内部不放任何 LVGL 对象（不失效该区）→ LVGL 不会重绘盖掉
 *     画面；lv_nuttx_lcd.c flush_cb 已按列分块 <=320px，消除 rowbuff_be
 *     越界（swap 标志被覆写 → 颜色错乱"彩色丝"的另一个根源）。
 *   - V3 稿在取景框内画的虚线框/四角括弧/扫描线/提示胶囊未搬：直写线程
 *     会把它们当画面内容盖掉，故只保留 3px 绿框 + 外层深色圆角取景区。
 *
 * 拍照：take() 冻结 → preview_stop() 杀线程/DMA → 取预览线程最后一次
 * 成功解码并显示的那份 JPEG 原始字节上传。屏上定格和云端原图是同一份
 * 字节，不再在按快门时重新取一帧。
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "screen_camera.h"
#include "screen_diagnose.h"
#include "../ui_app.h"
#include "../theme/companion_style.h"
#include "../widgets/widget_bottomnav.h"

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
#  include "../../components/camera_capture/camera_capture.h"
#endif

#ifdef CONFIG_PLANT_AI_MODULE
#  include "../../services/ai_service.h"
#  include "../../services/server_bridge.h"
#  include "../../services/pokedex_service.h"
#endif

#ifdef CONFIG_PLANT_SOIL_SENSOR
#  include "../../components/sensor_driver/soil_sensor.h"
#  include "../../services/sensor_service.h"
#endif

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
/* nuttx arch（esp32s3_lowputc.h）：DVP 释放后把 IO42/40 路由回 UART0。
 * 头文件在 nuttx 树里，apps 侧直接 extern 声明，避免加 include 路径。 */

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
/* 大取景框：铺满 320×240，底部 40px 叠加控制栏。
 * 预览源 320×240 按目标宽高比居中裁剪后写入，避免 4:3 被拉伸。 */
/* 撕裂（"油膜"）控制见 camera_capture.c 中的说明：写屏行速 63us/行
 * 慢于面板扫描 52us/行，写入期间必留错位带；面积越小带越窄。
 * 320x240 是"尺寸可用 + 撕裂可控"的折中（高度 160 太小影响取景体验）。 */

#define CAM_SCREEN_H   TP_SCREEN_H
#define CAM_VIEW_X     0
#define CAM_VIEW_Y     0
#define CAM_VIEW_W     TP_SCREEN_W
#define CAM_BAR_H      40
#define CAM_BAR_Y      (CAM_SCREEN_H - CAM_BAR_H)
#define CAM_VIEW_H     CAM_BAR_Y   /* 取景区避开底部控制栏 */

/* v165：控制栏 64px -> 40px，取景框 480x256 -> 480x280。
 * 取景框已经是「整宽 + 控制栏之外的最大高度」，再大就必须叠加在画面上；
 * 而叠加会让 LVGL 重绘把直写的画面刷掉（v160 已定位的根因）。 */

static lv_timer_t *s_frame_timer;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
static unsigned s_frame_period_request=20;
static unsigned s_frame_period_applied=20; /* UI thread only. */
int screen_camera_request_frame_period(unsigned milliseconds)
{
  if(milliseconds!=10 && milliseconds!=20 && milliseconds!=40)return -EINVAL;
  __atomic_store_n(&s_frame_period_request,milliseconds,__ATOMIC_RELEASE);
  return 0;
}
unsigned screen_camera_frame_period_requested(void)
{
  return __atomic_load_n(&s_frame_period_request,__ATOMIC_ACQUIRE);
}
static void camera_apply_frame_period(lv_timer_t *timer)
{
  unsigned requested=screen_camera_frame_period_requested();
  if(requested!=s_frame_period_applied)
    {lv_timer_set_period(timer,requested);s_frame_period_applied=requested;}
}
#endif


#ifdef CONFIG_PLANT_CAMERA_CAPTURE
static lv_obj_t *s_cam_img;
static lv_obj_t *s_cam_scr;
static lv_image_dsc_t s_cam_img_dsc;
static uint32_t s_cam_img_seq;
static bool s_cam_direct;
/* One renderer owns both preview and feedback. Direct strip writes race
 * with the top-layer listening/exploration label and erase its rows. */
static bool s_cam_force_lvgl = true;
#endif   /* 帧计数显示定时器（ui_task 上下文） */
static lv_obj_t *s_frame_label;     /* 右上角帧计数（调试用，不挡画面） */
static bool s_cam_stopped;          /* 拍照后已杀摄像头进程（返回需重启）*/
static bool s_cam_busy;             /* ⚠️ 诊断进行中：等待期禁止重启预览 */
static volatile bool s_shoot_worker_running;

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
static uint32_t s_shoot_t0;          /* 快门按下时刻（[Perf] 全链路计时） */

static uint32_t cam_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void cam_on_back(lv_event_t *e)
{
  (void)e;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  if(interaction_active())interaction_stop();
  if(voice_service_busy())voice_service_abort();
#endif

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

      /* 图鉴模式：成功识别即更新本地图鉴，下次快门秒回植物名 */
      pokedex_update(&result->diag);
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

  printf("[Perf][Cam] 快门->AI结果 全程 %u ms\n",
         (unsigned)(cam_now_ms() - s_shoot_t0));
#else
  s_ai_failed = true;
  s_ai_result[0] = '\0';
  snprintf(s_ai_error, sizeof(s_ai_error),
           "识别不可用：AI 模块未编译");
  s_ai_pending = true;
#endif
}

/* 拍照：冻结并停止高清预览线程，把线程最后一次成功显示的那份 JPEG
 * 交给 worker 上传；诊断期间不再继续跑摄像头 DMA。 */

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
static void *cam_shoot_worker(void *arg)
{
  const uint8_t *fb = NULL;
  size_t flen = 0;
  uint32_t t0 = cam_now_ms();
  int ret;

  (void)arg;

  camera_preview_take();          /* 冻结：线程进入空转，stop 不等待 */
  camera_preview_stop();
  s_cam_stopped = true;

  printf("[Perf][Cam] 冻结+停预览 %u ms\n",
         (unsigned)(cam_now_ms() - t0));

  /* v61：预览是 RGB565 640x480 直采，没有现成 JPEG。快门 = 停预览后
   * 切 JPEG 1024x768 冷拍（静模式链路实测 100% 完好），严格解码验证
   * +失败重抓，解码帧定格取景框（所见即所传），原始 JPEG 上传。 */

#ifdef CONFIG_PLANT_CAMERA_GC0308
  /* GC0308 has no JPEG encoder. The preview buffer is already the exact
   * 320x240 RGB565 payload understood by /media/image?fmt=raw565. */

  fb = camera_preview_get_frame();
  flen = CAM_PREVIEW_SIZE;
  if (fb == NULL)
    {
      ret = -ENODATA;
    }
  else
    {
      ret = 0;
    }

  printf("[Perf][Cam] 取RGB565快照 %u ms (%u B)\n",
         (unsigned)(cam_now_ms() - t0), (unsigned)flen);
#else
  ret = camera_hd_jpeg_capture_verified(&fb, &flen,
                                        CAM_VIEW_X, CAM_VIEW_Y,
                                        CAM_VIEW_W, CAM_VIEW_H, 3);

  printf("[Perf][Cam] 取JPEG快照 %u ms (%u B)\n",
         (unsigned)(cam_now_ms() - t0), (unsigned)flen);
#endif

  if (ret == 0 && fb != NULL && flen != 0)
    {
      ret = ai_service_analyze_async(fb, flen, true, NULL,
                                     cam_ai_done, NULL);
    }

  if (ret != 0)
    {
      cam_ai_done(NULL, NULL);
    }

  s_shoot_worker_running = false;
  return NULL;
}
#endif

static void cam_on_shoot(lv_event_t *e)
{
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  if(e!=NULL)
    {
      /* Physical shutter: fast snapshot/object answer, not a full health
       * report. An explicit voice health check still calls with NULL. */
      if(s_cam_busy || s_shoot_worker_running || interaction_active())
        {if(s_status_label)lv_label_set_text(s_status_label,"识别中…");return;}
      if(voice_service_busy())voice_service_abort();
      static unsigned shutter_sequence;
      char id[48];snprintf(id,sizeof(id),"shutter-%x-%u",cam_now_ms(),++shutter_sequence);
      int rc=interaction_submit("inspect","photo",id);
      if(s_status_label)lv_label_set_text(s_status_label,rc==0?"让我看看~":"请稍后重试");
      return;
    }
#endif
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
          lv_label_set_text(s_status_label, "识别中…");
        }

      return;
    }
#  endif

  /* 镜头被挡拦截（2026-09-26）：连续单色帧 = 手捂住镜头或传感器异常，
   * 不浪费一次上传识别，直接提示（秒回，孩子立刻知道怎么回事）。 */

  if (camera_preview_mono_streak() >= 3)
    {
      if (s_status_label != NULL)
        {
          lv_label_set_text(s_status_label, "镜头被挡住啦？");
        }

      printf("[Cam] 单色帧连击 %u，快门转提示不上传\n",
             camera_preview_mono_streak());
      return;
    }

  if (s_status_label != NULL)
    {
      struct pokedex_entry_s pd;

      /* 图鉴模式秒回：上次成功识别过的植物，先喊出名字（孩子立刻
       * 有反馈）；AI 体检后台照跑，结果回来再上诊断页。 */
      if (pokedex_get(&pd) == 0)
        {
          char msg[96];

          snprintf(msg, sizeof(msg), "是%s呀！体检中…", pd.name);
          lv_label_set_text(s_status_label, msg);
          printf("[Pokedex] 快门秒回: %s\n", pd.name);
        }
      else
        {
          lv_label_set_text(s_status_label, "让我看看~");
        }
    }

#  ifdef CONFIG_PLANT_AI_MODULE
  {
    /* 真实链路（零拷贝 + 杀线程 + 所见即所传）：
     * 1) take() 冻结，不再更新屏上画面；
     * 2) preview_stop() 等当前高清帧收尾并停 DMA；
     * 3) camera_preview_get_jpeg() 取最后一次成功显示的 JPEG 快照；
     * 4) worker 零拷贝上传同一份字节；
     * ⚠️ s_cam_busy=true：诊断等待期（上传 + 服务器识别 10-40s）禁止
     * frame_timer 重启预览 —— 否则预览会覆盖 worker 正在读的帧。 */
    if (!s_shoot_worker_running)
      {
        pthread_attr_t attr;
        pthread_t tid;

        s_shoot_t0 = cam_now_ms();
        s_shoot_worker_running = true;
        s_cam_busy = true;

        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8192);
        if (pthread_create(&tid, &attr, cam_shoot_worker, NULL) == 0)
          {
            pthread_detach(tid);
          }
        else
          {
            s_shoot_worker_running = false;
            s_cam_busy = false;
            if (s_status_label != NULL)
              {
                lv_label_set_text(s_status_label, "失败重试");
              }
          }

        pthread_attr_destroy(&attr);
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

static bool s_cam_page_active;   /* 拍照页激活中（用于外部强制离开） */

static void cam_on_delete(lv_event_t *e)
{
  (void)e;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  if(interaction_active())interaction_stop();
#endif

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  s_cam_page_active = false;
  camera_preview_stop();

  /* M2：完整关闭摄像头（停 CAM/DMA + SCCB 软复位 OV3660）；传感器被软复位
   * 后不再推挽驱动 DVP 输出，下面的 reclaim 才能把 IO42/40 收回给 UART0。 */
  camera_shutdown();
#ifdef CONFIG_PLANT_CAMERA_GC0308
  camera_preview_release_frame();
#endif

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
  /* Resume the cloud-only producer after camera memory is released. */

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
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  camera_apply_frame_period(timer);
#endif

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  if (!s_cam_direct && camera_preview_is_running())
    {
      extern uint8_t *esp32s3_cam_dvp_get_frame(void);
      uint32_t seq = 0;
#ifdef CONFIG_PLANT_CAMERA_GC0308
      uint8_t *fb = (uint8_t *)camera_preview_acquire_frame(&seq);
#else
      seq = camera_preview_get_frame_index();
      uint8_t *fb = esp32s3_cam_dvp_get_frame();
#endif
      if (fb != NULL && seq != s_cam_img_seq)
        {

          if (fb != NULL)
            {
              s_cam_img_seq = seq;
              s_cam_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
              s_cam_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
              s_cam_img_dsc.header.w = CAM_VIEW_W;
#ifdef CONFIG_PLANT_CAMERA_GC0308
              s_cam_img_dsc.header.h = CAM_VIEW_H;
              s_cam_img_dsc.header.stride = CAM_VIEW_W * 2;
              s_cam_img_dsc.data_size = CAM_VIEW_W * CAM_VIEW_H * 2;
#else
              s_cam_img_dsc.header.h = CAM_PREVIEW_H;
              s_cam_img_dsc.header.stride = CAM_PREVIEW_W * 2;
              s_cam_img_dsc.data_size = CAM_PREVIEW_W * CAM_PREVIEW_H * 2;
#endif
              s_cam_img_dsc.data = fb;

              if (s_cam_img == NULL)
                {
                  s_cam_img = lv_image_create(s_cam_scr != NULL ? s_cam_scr : lv_screen_active());
                  lv_obj_remove_style_all(s_cam_img);
                  lv_obj_set_pos(s_cam_img, 0, 0);
                  lv_obj_set_size(s_cam_img, CAM_VIEW_W, CAM_VIEW_H);
                  lv_image_set_scale(s_cam_img, 256);
                  lv_obj_set_style_opa(s_cam_img, LV_OPA_COVER, 0);
                  lv_obj_set_style_image_opa(s_cam_img, LV_OPA_COVER, 0);
                }

              /* Match StackChan SetPreviewImage(): set_src() performs the
               * required invalidation; do not add a duplicate dirty area. */
              lv_image_set_src(s_cam_img, &s_cam_img_dsc);
            }
        }
    }
#endif

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
          lv_label_set_text(s_status_label, "识别完成");
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
            camera_preview_start_rgb(CAM_VIEW_X, CAM_VIEW_Y,
                                    CAM_VIEW_W, CAM_VIEW_H);
            s_cam_stopped = false;
          }
        else
          {
            camera_preview_resume();
          }
      }

      /* The label is hidden in production.  Avoid creating a needless LVGL
       * dirty area every 100 ms while the camera worker owns the viewport. */
      if (!lv_obj_has_flag(s_frame_label, LV_OBJ_FLAG_HIDDEN))
        {
          lv_label_set_text_fmt(s_frame_label, "帧 %d",
                                camera_preview_get_frame_index());
        }
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
  camera_preview_start_rgb(CAM_VIEW_X, CAM_VIEW_Y, CAM_VIEW_W, CAM_VIEW_H);
  s_cam_stopped = false;

  if (s_status_label != NULL)
    {
      lv_label_set_text(s_status_label, "AI就绪");
    }
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_camera_create(void)
{
  /* Camera is a full-screen secondary page.  Parent it to the top layer so
   * it cannot be covered by the active Tab screen or lost behind a stale
   * navigation layer. */
  lv_obj_t *scr = lv_obj_create(lv_layer_top());

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  s_cam_scr = scr;
#endif

#ifdef CONFIG_PLANT_CAMERA_CAPTURE
  {
    extern void camera_preview_set_lcd_skip(int on);
    s_cam_img = NULL;
    s_cam_img_seq = 0;
#ifdef CONFIG_PLANT_CAMERA_GC0308
    /* Runtime selectable: direct strip writes remain the performance path;
     * LVGL mode follows StackChan's SetPreviewImage architecture. */
    s_cam_direct = !s_cam_force_lvgl;
    camera_preview_set_lcd_skip(s_cam_force_lvgl ? 1 : 0);
#else
    /* BOX keeps its proven direct-LCD path unchanged. */
    s_cam_direct = true;
    camera_preview_set_lcd_skip(0);
#endif

  }
#endif
  lv_obj_t *back;
  lv_obj_t *ai_tag;
  lv_obj_t *ctl;
  lv_obj_t *btn;
  lv_obj_t *shoot;

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, TP_SCREEN_W, TP_SCREEN_H);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  /* v160：页面背景改为透明，并在取景框四周显式铺 4 块背景。
   * 原因：取景框是相机线程直写 LCD 的，LVGL 不知道 —— 只要 LVGL 重画页面
   * 背景（例如帧计数标签每 500ms 更新、或任何页内对象变化），就会把取景框
   * 那块一起刷成背景色。两者交替刷屏正是"油膜横扫/闪烁/黑屏"的真因：
   *  - 预览连续写（7fps）时多数抢赢 → 可见但伴随错位带；
   *  - 预览低频写（快照模式）时 LVGL 抢赢 → 取景框被刷黑。
   * 让 LVGL 完全不绘制取景框区域后，预览写进去的内容不再被覆盖。 */

  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
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

#  ifdef CONFIG_PLANT_AI_MODULE
  pokedex_preload();        /* 图鉴模式：进页即备货，快门秒回 */
#  endif
#endif

  /* ── 取景框四周的背景（v160）：LVGL 不画取景框本体，只画这四块 ── */

  {
    static const struct
    {
      int x;
      int y;
      int w;
      int h;
    } k_bg[4] =
    {
      { 0, 0, TP_SCREEN_W, CAM_VIEW_Y },                      /* 上 */
      { 0, CAM_VIEW_Y + CAM_VIEW_H, TP_SCREEN_W,
        CAM_BAR_Y - CAM_VIEW_Y - CAM_VIEW_H },                /* 下（控制栏之上） */
      { 0, CAM_VIEW_Y, CAM_VIEW_X, CAM_VIEW_H },              /* 左 */
      { CAM_VIEW_X + CAM_VIEW_W, CAM_VIEW_Y,
        TP_SCREEN_W - CAM_VIEW_X - CAM_VIEW_W, CAM_VIEW_H }   /* 右 */
    };
    int bi;

    for (bi = 0; bi < 4; bi++)
      {
        if (k_bg[bi].w <= 0 || k_bg[bi].h <= 0)
          {
            continue;
          }

        lv_obj_t *bg = lv_obj_create(scr);

        lv_obj_remove_style_all(bg);
        lv_obj_set_size(bg, k_bg[bi].w, k_bg[bi].h);
        lv_obj_set_pos(bg, k_bg[bi].x, k_bg[bi].y);
        lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);
      }
  }

  /* ── 取景框本体由摄像头线程直写；底部 40px 控制栏 ── */

  s_status_label = NULL;

  ctl = lv_obj_create(scr);
  lv_obj_remove_style_all(ctl);
  lv_obj_set_size(ctl, TP_SCREEN_W, CAM_BAR_H);
  lv_obj_set_pos(ctl, 0, CAM_BAR_Y);
  lv_obj_set_style_bg_color(ctl, lv_color_hex(PAL_BG), 0);
  lv_obj_set_style_bg_opa(ctl, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(ctl, 0, 0);
  lv_obj_remove_flag(ctl, LV_OBJ_FLAG_CLICKABLE);

  back = lv_button_create(ctl);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, 32, 32);
  lv_obj_set_pos(back, 6, 4);
  lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(PAL_YELLOW), 0);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(back, lv_color_hex(PAL_FG), 0);
  lv_obj_set_style_text_font(back, TP_FONT_BODY, 0);
  theme_btn_set_text(back, LV_SYMBOL_CLOSE);
  lv_obj_add_event_cb(back, cam_on_back, LV_EVENT_CLICKED, NULL);

  btn = lv_button_create(ctl);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 32, 32);
  lv_obj_set_pos(btn, 44, 4);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(PAL_YELLOW), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(PAL_FG), 0);
  lv_obj_set_style_text_font(btn, TP_ICON_M, 0);
  theme_btn_set_text(btn, IC_ALBUM);
  lv_obj_add_event_cb(btn, cam_on_gallery, LV_EVENT_CLICKED, NULL);

  ai_tag = lv_label_create(ctl);
  lv_label_set_text(ai_tag, "AI就绪");
  lv_obj_set_size(ai_tag, 90, 28);
  lv_obj_set_pos(ai_tag, 184, 6);
  lv_obj_set_style_text_font(ai_tag, TP_FONT_CAPTION, 0);
  lv_obj_set_style_text_color(ai_tag, lv_color_hex(PAL_EYE), 0);
  lv_obj_set_style_text_align(ai_tag, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(ai_tag, lv_color_hex(PAL_PAPER), 0);
  lv_obj_set_style_bg_opa(ai_tag, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(ai_tag, 8, 0);
  lv_obj_set_style_pad_all(ai_tag, 4, 0);
  s_status_label = ai_tag;

  shoot = lv_button_create(ctl);
  lv_obj_remove_style_all(shoot);
  lv_obj_set_size(shoot, 36, 36);
  lv_obj_set_pos(shoot, 142, 2);
  lv_obj_set_style_radius(shoot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(shoot, lv_color_hex(PAL_YELLOW), 0);
  lv_obj_set_style_bg_opa(shoot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(shoot, 3, 0);
  lv_obj_set_style_border_color(shoot, lv_color_hex(PAL_EYE), 0);
  lv_obj_set_style_border_side(shoot, LV_BORDER_SIDE_FULL, 0);
  lv_obj_set_style_text_color(shoot, lv_color_hex(TP_CLR_GREEN), 0);
  lv_obj_set_style_text_font(shoot, TP_ICON_L, 0);
  theme_btn_set_text(shoot, IC_SHUTTER);
  lv_obj_add_event_cb(shoot, cam_on_shoot, LV_EVENT_CLICKED, NULL);

  btn = lv_button_create(ctl);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 32, 32);
  lv_obj_set_pos(btn, 280, 4);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(PAL_YELLOW), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(PAL_FG), 0);
  lv_obj_set_style_text_font(btn, TP_ICON_M, 0);
  theme_btn_set_text(btn, IC_RETRY);
  lv_obj_add_event_cb(btn, cam_on_retry, LV_EVENT_CLICKED, NULL);

  /* v166：把控件的屏幕矩形注册给相机线程（写屏时让位）。
   * 坐标 = 上面控件的局部坐标 + ctl 原点 (0, CAM_BAR_Y)，
   * 改控件位置/尺寸时必须一起改（否则画面会啧掉控件）。 */

  camera_preview_mask_clear();
  camera_preview_mask_add(6,   CAM_BAR_Y + 4, 32, 32, 16);   /* 返回 */
  camera_preview_mask_add(44,  CAM_BAR_Y + 4, 32, 32, 16);   /* 相册 */
  camera_preview_mask_add(142, CAM_BAR_Y + 2, 36, 36, 18);   /* 快门 */
  camera_preview_mask_add(184, CAM_BAR_Y + 6, 90, 28,  8);   /* 状态 */
  camera_preview_mask_add(280, CAM_BAR_Y + 4, 32, 32, 16);   /* 重来 */

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
      /* 高清取景：线程内部配置 2048x1536 JPEG，持续解码/直写 LCD
       * （解码不过即重抓，只显示完整帧）。预览帧同时保留为快门要
       * 上传的原始 JPEG。 */

      camera_preview_start_rgb(CAM_VIEW_X, CAM_VIEW_Y,
                              CAM_VIEW_W, CAM_VIEW_H);

      /* 帧计数仍保留用于调试和预览恢复逻辑，但不在大画面上显示。 */

      s_frame_label = lv_label_create(scr);
      lv_label_set_text(s_frame_label, "帧 0");
      lv_obj_set_style_text_font(s_frame_label, TP_FONT_CAPTION, 0);
      lv_obj_set_style_text_color(s_frame_label, lv_color_hex(0x475569), 0);
      lv_obj_set_pos(s_frame_label, 0, 0);
      lv_obj_add_flag(s_frame_label, LV_OBJ_FLAG_HIDDEN);

      if (s_frame_timer == NULL)
        {
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
          /* Poll publication below the display refresh interval; skip an
           * unchanged sequence as before. Keep all LVGL work on UI. */
          s_frame_period_applied=screen_camera_frame_period_requested();
          s_frame_timer=lv_timer_create(frame_timer_cb,s_frame_period_applied,NULL);
#else
          s_frame_timer = lv_timer_create(frame_timer_cb, 40, NULL);
#endif
        }
    }
  else
    {
      /* camera_init 失败（无摄像头/探测失败）：引脚未交付摄像头，
       * 立即把 IO42/40 交还土壤并恢复轮询 */

#ifdef CONFIG_PLANT_SOIL_SENSOR

      soil_sensor_set_paused(false);
      sensor_service_start(3000, NULL, NULL);
#endif
    }
#endif

  /* 页面销毁（返回）时停止预览线程 */

  lv_obj_add_event_cb(scr, cam_on_delete, LV_EVENT_DELETE, NULL);
  s_cam_page_active = true;

  return scr;
}

/* 外部（CLI/导航）切屏时调用：若拍照页仍激活则正常弹出，
 * 走 cam_on_delete → camera_preview_stop + camera_shutdown。
 * 否则预览线程会继续往新页面上刷相机画面（实测"切首页后画面异常"）。 */

void screen_camera_leave_if_active(void)
{
  if (s_cam_page_active)
    {
      s_cam_page_active = false;

      /* v175：同步冻结再切页 —— 否则预览线程会把相机画面盖到新页面上 */
      camera_preview_take();

      ui_app_pop_screen();
    }
}

bool screen_camera_is_active(void)
{
  return s_cam_page_active;
}

void screen_camera_set_lvgl_mode(bool on)
{
  s_cam_force_lvgl = on;
}

bool screen_camera_get_lvgl_mode(void)
{
  return s_cam_force_lvgl;
}

/* UI-thread business entry shared by physical shutter and voice actions. */
int screen_camera_request_capture(void)
{
  if (!screen_camera_is_active()) return -1;
  if (s_cam_busy || s_shoot_worker_running) return -2;
  if (s_cam_stopped || !camera_preview_is_running() || camera_preview_get_frame_index()<=0) return -3;
  cam_on_shoot(NULL);
  return 0;
}

bool screen_camera_is_visible(void)
{
  return s_cam_page_active && s_frame_label &&
         !lv_obj_has_flag(lv_obj_get_parent(s_frame_label),LV_OBJ_FLAG_HIDDEN);
}
bool screen_camera_capture_busy(void)
{return s_cam_busy || s_shoot_worker_running;}
