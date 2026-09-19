/****************************************************************************
 * apps/plant-companion/ui/screens/screen_voice.c
 *
 * ⑥ 语音对话 — 和小绿聊天
 *   顶部：返回 + 「语音聊天」
 *   中部：对话气泡列表（滚动）
 *   底部：🎙️ 说话大按钮（voice_service 录音→AI→TTS）
 *
 * 线程模型：voice_service 回调在 voice 线程触发，这里只写入
 * 待处理缓冲区；lv_timer（500ms，ui_task 上下文）负责真正上屏，
 * 满足「LVGL 对象只在 ui_task 操作」约束。
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <string.h>

#include "screen_voice.h"
#include "../ui_app.h"
#include "../theme/theme_plant.h"
#include "../assets/fonts/zh_font.h"

#ifdef CONFIG_PLANT_AI_VOICE
#  include "../../services/voice_service.h"
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_msg_list;
static lv_obj_t *s_status;
static lv_timer_t *s_ui_timer;   /* ⚠️ 必须随页面销毁，否则 UAF */

/* 跨线程待处理消息（voice 线程写，ui_task 读） */

#define VOICE_PENDING_MAX 4

/* 2026-09-11 修复：服务器回复显示不全。
 * 上游 voice_service 的 reply 缓冲是 1024 字节，实测一条回复
 * 328 字节（含转写前缀 + 回答），这里原来只有 160 字节 → 被
 * strlcpy 截掉大半，屏幕上只看到开头一小段。改成与上游同宽。 */

#define VOICE_PENDING_TEXT_MAX 1024

static char s_pending_text[VOICE_PENDING_MAX][VOICE_PENDING_TEXT_MAX];
static bool s_pending_from_user[VOICE_PENDING_MAX];
static volatile int s_pending_head;
static volatile int s_pending_tail;
static int s_last_state = -1;

/* 波形动画竖线 */

#define WAVE_BARS 7

static lv_obj_t *s_wave_bars[WAVE_BARS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void voice_on_back(lv_event_t *e)
{
  (void)e;
  ui_app_pop_screen();
}

/* ⚠️ 2026-08-31 PANIC 根因修复：页面销毁（返回）时删除 UI 定时器。
 * 此前 lv_timer_create 后从不删除 → 页面 lv_obj_delete 后 timer 仍每
 * 500ms 访问已释放的 s_status/s_wave_bars/s_msg_list → use-after-free，
 * 崩溃在 LVGL get_selector_style_prop（实测 PC=0x42053845）。
 * 与 screen_camera 的 cam_on_delete 同款修复。 */

static void voice_on_delete(lv_event_t *e)
{
  (void)e;

#ifdef CONFIG_PLANT_AI_VOICE
  /* 页面销毁：立即中止进行中的语音会话（录音/播放/等待），
   * 防止 voice worker 还在跑但 UI 已销毁（回调已无页面可写）。 */
  voice_service_abort();
#endif

  if (s_ui_timer != NULL)
    {
      lv_timer_delete(s_ui_timer);
      s_ui_timer = NULL;
    }

  /* 清静态指针，防止残留引用已释放对象 */

  s_msg_list = NULL;
  s_status = NULL;
  memset(s_wave_bars, 0, sizeof(s_wave_bars));

  /* 清待处理消息（voice 线程可能还在写，但页面已销毁，丢弃即可） */

  s_pending_head = 0;
  s_pending_tail = 0;
  s_last_state = -1;
}

/* voice_service 状态回调（voice 线程）：直接写状态（atomic） */

#ifdef CONFIG_PLANT_AI_VOICE
static void voice_state_handler(void *user_data, int state)
{
  (void)user_data;
  s_last_state = state;
}

/* voice_service 文本回调（voice 线程）：写入环形待处理区 */

static void voice_text_handler(void *user_data, const char *text, bool from_user)
{
  int next;
  (void)user_data;

  next = (s_pending_head + 1) % VOICE_PENDING_MAX;
  if (next == s_pending_tail)
    {
      return;   /* 队列满，丢弃 */
    }

  if (text != NULL && strlen(text) >= sizeof(s_pending_text[0]))
    {
      printf("[VoiceUI] 回复超长被截断: %u 字节 (缓冲 %u)\n",
             (unsigned)strlen(text),
             (unsigned)sizeof(s_pending_text[0]));
    }

  strlcpy(s_pending_text[s_pending_head],
          text != NULL ? text : "", sizeof(s_pending_text[0]));
  printf("[VoiceUI] 消息上屏: %u 字节 from_user=%d\n",
         (unsigned)strlen(s_pending_text[s_pending_head]),
         (int)from_user);
  s_pending_from_user[s_pending_head] = from_user;
  s_pending_head = next;
}
#endif /* CONFIG_PLANT_AI_VOICE */

/* lv_timer：ui_task 上下文处理待处理消息 + 状态文字 + 波形动画 */

static void voice_ui_timer_cb(lv_timer_t *timer)
{
  static const char *const state_text[] =
  {
    "点一下开始说话", "正在听你说...", "AI思考中...", "正在说话..."
  };
  int i;
  (void)timer;

  /* 状态文字 */

  if (s_status != NULL && s_last_state >= VOICE_STATE_IDLE &&
      s_last_state <= VOICE_STATE_SPEAKING)
    {
      lv_label_set_text(s_status, state_text[s_last_state]);
    }

  /* 波形动画：LISTENING/SPEAKING 时摆动，其余静止 */

  for (i = 0; i < WAVE_BARS; i++)
    {
      if (s_wave_bars[i] != NULL)
        {
          bool active = (s_last_state == VOICE_STATE_LISTENING ||
                         s_last_state == VOICE_STATE_SPEAKING);

          if (active)
            {
              lv_obj_set_height(s_wave_bars[i],
                                8 + (lv_rand(0, 100) * 20) / 100);
            }
          else
            {
              lv_obj_set_height(s_wave_bars[i], 8);
            }
        }
    }

  /* 待处理消息上屏 */

  while (s_pending_tail != s_pending_head)
    {
      const char *text = s_pending_text[s_pending_tail];

      if (s_pending_from_user[s_pending_tail])
        {
          screen_voice_add_user_msg(NULL, text);
        }
      else
        {
          screen_voice_add_ai_reply(NULL, text);
        }

      s_pending_tail = (s_pending_tail + 1) % VOICE_PENDING_MAX;
    }
}

/* 🎙️ 说话：触发 voice_service 一轮对话 */

static void voice_on_talk(lv_event_t *e)
{
  (void)e;

#ifdef CONFIG_PLANT_AI_VOICE
  {
    int ret;

    ret = voice_service_talk(3);
    if (ret < 0 && s_status != NULL)
      {
        lv_label_set_text(s_status, "正在忙，稍等一下...");
      }
  }
#else
  /* 语音模块未启用：演示一问一答 */

  screen_voice_add_user_msg(NULL, "小绿绿今天需要浇水吗？");
  screen_voice_add_ai_reply(NULL, "让我看看... 💧 土壤水分62%，还很滋润呢！3天后再浇就好啦~");
#endif
}

/* 创建波形动画条（底部说话按钮上方） */

static void wave_create(lv_obj_t *parent)
{
  lv_obj_t *wave = lv_obj_create(parent);
  int i;

  lv_obj_remove_style_all(wave);
  lv_obj_set_size(wave, 120, 24);
  lv_obj_set_pos(wave, 180, 320 - 150);
  lv_obj_set_flex_flow(wave, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(wave, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (i = 0; i < WAVE_BARS; i++)
    {
      lv_obj_t *bar = lv_obj_create(wave);

      lv_obj_remove_style_all(bar);
      lv_obj_set_size(bar, 8, 8);
      lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(bar, lv_color_hex(TP_CLR_VOICE_PINK), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

      s_wave_bars[i] = bar;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *screen_voice_create(void)
{
  lv_obj_t *scr = lv_obj_create(lv_screen_active());
  lv_obj_t *back;
  lv_obj_t *title;
  lv_obj_t *talk;

  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 320);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xfdf2f8), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(scr, lv_color_hex(TP_CLR_BG_GRAD_A), 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

  /* 顶部：返回 + 标题 */

  back = lv_button_create(scr);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, 96, TP_TOP_BAR_H);
  lv_obj_set_pos(back, 0, 0);
  lv_obj_set_style_text_color(back, lv_color_hex(TP_CLR_VOICE_PINK), 0);
  lv_obj_set_style_text_font(back, TP_FONT_BODY, 0);
  theme_btn_set_text(back, LV_SYMBOL_LEFT " 返回");
  lv_obj_add_event_cb(back, voice_on_back, LV_EVENT_CLICKED, NULL);

  title = lv_label_create(scr);
  lv_label_set_text(title, "和小绿聊天");
  lv_obj_set_style_text_font(title, TP_FONT_TITLE, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(TP_CLR_VOICE_PINK), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  /* 对话气泡列表（滚动区） */

  s_msg_list = lv_obj_create(scr);
  lv_obj_remove_style_all(s_msg_list);
  lv_obj_set_pos(s_msg_list, 0, TP_TOP_BAR_H);
  lv_obj_set_size(s_msg_list, 480, 320 - TP_TOP_BAR_H - 150);
  lv_obj_set_style_pad_all(s_msg_list, 12, 0);
  lv_obj_set_style_pad_row(s_msg_list, 10, 0);
  lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_msg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);

  /* 初始欢迎气泡 */

  {
    lv_obj_t *bubble = lv_obj_create(s_msg_list);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, TP_RADIUS_SMALL, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    /* 2026-09-11 修复横向只显示 2/3：原写死 300px，聊天区可用 456px。 */
    lv_obj_set_width(bubble, LV_PCT(100));

    lv_obj_t *txt = lv_label_create(bubble);
    lv_label_set_text(txt, "你好呀！我是小绿绿 🌱 点一下下面的按钮，跟我说说话吧");
    lv_obj_set_style_text_font(txt, TP_FONT_BODY, 0);
    lv_obj_set_width(txt, LV_PCT(100));
  }

  /* 波形动画 */

  wave_create(scr);

  /* 底部：状态文字 + 🎙️ 说话大按钮 */

  s_status = lv_label_create(scr);
  lv_label_set_text(s_status, "点一下开始说话");
  lv_obj_set_style_text_font(s_status, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(s_status, lv_color_hex(TP_CLR_TEXT_SUB), 0);
  lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -104);

  talk = lv_button_create(scr);
  lv_obj_remove_style_all(talk);
  lv_obj_set_size(talk, 88, 88);
  lv_obj_align(talk, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_set_style_radius(talk, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(talk, lv_color_hex(TP_CLR_VOICE_PINK), 0);
  lv_obj_set_style_bg_opa(talk, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(talk, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(talk, &lv_font_montserrat_20, 0);
  theme_btn_set_text(talk, LV_SYMBOL_AUDIO);
  lv_obj_add_event_cb(talk, voice_on_talk, LV_EVENT_CLICKED, NULL);

  /* 启动 voice_service（若未启动）并注册回调 */

#ifdef CONFIG_PLANT_AI_VOICE
  voice_service_start(voice_state_handler, voice_text_handler, NULL);
#endif

  /* ui_task 定时器：处理跨线程消息 */

  /* 2026-09-11 用户要求：把聊天对话框（消息区）提到本页最上层，
   * 避免被页内其它控件压住（消息区与底部按钮不重叠，不影响交互）。 */

  lv_obj_move_foreground(s_msg_list);

  s_ui_timer = lv_timer_create(voice_ui_timer_cb, 500, NULL);

  /* ⚠️ 页面销毁必须删 timer（否则 UAF 崩溃），见 voice_on_delete */

  lv_obj_add_event_cb(scr, voice_on_delete, LV_EVENT_DELETE, NULL);

  return scr;
}

void screen_voice_add_ai_reply(lv_obj_t *scr, const char *text)
{
  lv_obj_t *bubble;
  lv_obj_t *txt;
  (void)scr;

  if (s_msg_list == NULL)
    {
      return;
    }

  bubble = lv_obj_create(s_msg_list);
  lv_obj_remove_style_all(bubble);
  lv_obj_set_style_bg_color(bubble, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bubble, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_pad_all(bubble, 12, 0);
  lv_obj_set_width(bubble, LV_PCT(100));

  txt = lv_label_create(bubble);
  zh_label_set_text_safe(txt, text);
  lv_obj_set_style_text_font(txt, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(txt, lv_color_hex(TP_CLR_VOICE_PINK), 0);
  lv_obj_set_width(txt, LV_PCT(100));

  /* 先跑一次布局，保证气泡高度已算好，再滚动到可见，
   * 否则长回复的最后几行会停在可视区外（看起来像被截断）。 */

  lv_obj_update_layout(s_msg_list);
  lv_obj_scroll_to_view(bubble, LV_ANIM_OFF);
}

void screen_voice_add_user_msg(lv_obj_t *scr, const char *text)
{
  lv_obj_t *bubble;
  lv_obj_t *txt;
  (void)scr;

  if (s_msg_list == NULL)
    {
      return;
    }

  bubble = lv_obj_create(s_msg_list);
  lv_obj_remove_style_all(bubble);
  lv_obj_set_style_bg_color(bubble, lv_color_hex(TP_CLR_VOICE_PINK), 0);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bubble, TP_RADIUS_SMALL, 0);
  lv_obj_set_style_pad_all(bubble, 12, 0);
  lv_obj_set_width(bubble, LV_PCT(100));
  lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, 0, 0);

  txt = lv_label_create(bubble);
  zh_label_set_text_safe(txt, text);
  lv_obj_set_style_text_font(txt, TP_FONT_BODY, 0);
  lv_obj_set_style_text_color(txt, lv_color_hex(0xffffff), 0);
  lv_obj_set_width(txt, LV_PCT(100));

  lv_obj_update_layout(s_msg_list);
  lv_obj_scroll_to_view(bubble, LV_ANIM_OFF);
}
