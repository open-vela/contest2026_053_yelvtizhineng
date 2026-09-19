/****************************************************************************
 * apps/plant-companion/services/voice_service.c
 *
 * 语音业务服务：录音 → AI 回复 → TTS 播放 状态机
 *
 * 3C-2 阶段实现：
 *   - talk() 启动一次性 worker：录音（ai_voice_record）→ 本地规则生成
 *     回复文本 → 文本回调 → TTS 播放（ai_voice_play）
 *   - 接入 MiMo 全模态时，把本地回复替换为 AI 文本即可
 *   - 回调在 voice 线程触发，UI 端必须投递到 ui_task
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "voice_service.h"
#include "server_bridge.h"
#include "../ai_module/ai_voice/ai_voice.h"
#include "../components/voice_agent/voice_agent.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_busy;
static volatile bool g_cancel;   /* 再点按键=取消（THINKING/窗口期丢弃）*/
static int g_state = VOICE_STATE_IDLE;
static voice_state_cb_t g_state_cb;
static voice_text_cb_t g_text_cb;
static void *g_user_data;
static bool g_inited;
static volatile uint32_t g_gen;        /* 会话代次（看门狗作废旧 worker 用） */
static clock_t  g_phase_tick;           /* 当前状态进入时刻（看门狗计时） */

/* 各阶段硬上限：超了就是"卡死了"，看门狗强制回到空闲（见下方注释） */

#define VOICE_WD_LISTEN_MS   20000    /* 录音（5s 上限 + 8s 硬兜底 + 余量） */
#define VOICE_WD_THINK_MS   110000    /* AI 思考（finalize 网络上限 90s） */
#define VOICE_WD_SPEAK_MS   240000    /* TTS 播放（长回复也不超过 4 分钟） */
#define VOICE_WD_IDLE_MS     30000    /* 会话建立等窗口期 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void set_state(int state)
{
  pthread_mutex_lock(&g_lock);
  g_state = state;
  g_phase_tick = clock_systime_ticks();   /* 看门狗计时锚点 */
  pthread_mutex_unlock(&g_lock);

  if (g_state_cb != NULL)
    {
      g_state_cb(g_user_data, state);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * ⚠️ 2026-09-16 三修：会话收尾 + 看门狗（"界面永远停在聆听"最终保险）
 *
 * 现场：点"说话"后界面一直停在"正在听你说"，再点按钮也没反应，只能
 *       重启板卡（g_busy 一直为真、状态卡在 LISTENING）。
 * 本轮已把各处等待都加上限（connect 有超时 / IO 队列丢块保采集 /
 * ioq_stop 有界收工），但只要有任意一条没预料到的路径（DMA、信号量、
 * 驱动、网络栈），症状就会复发。这里再加最后一道保险：
 *   - 任何阶段超过硬上限 → 强制清会话（状态回 IDLE、g_busy 复位），
 *     用户再点一次就能重新开始，不用重启板卡；
 *   - 同时把"会话代次"加一，被作废的旧 worker 之后的回写全部作废，
 *     不会把新会话的状态/气泡覆盖回去。
 * ════════════════════════════════════════════════════════════════════ */

static void session_finish(uint32_t gen)
{
  bool mine;

  pthread_mutex_lock(&g_lock);
  mine = (gen == g_gen);
  if (mine)
    {
      g_busy   = false;
      g_cancel = false;
    }

  pthread_mutex_unlock(&g_lock);

  /* 被看门狗作废的那一代：不再碰状态（新会话可能已经在跑） */

  if (mine)
    {
      set_state(VOICE_STATE_IDLE);
    }
}

/* 看门狗本体：由 ui_task 的 1s 定时器调用（见 ui/ui_app.c）。
 * 为什么不用独立线程：实测这块板子上"独立 pthread + 定时等待"的看门狗
 * 线程跑一轮就消失了（ps 里线程没了，只触发一次），而 ui_task 的定时器
 * 整场演示都稳。看门狗本来就是要把界面从"聆听"里救回来，放在 ui_task
 * 里最对症，也省掉一个线程。 */

void voice_service_watchdog_poll(void)
{
  int      st;
  uint32_t el;
  uint32_t limit;

  pthread_mutex_lock(&g_lock);
  if (!g_busy)
    {
      pthread_mutex_unlock(&g_lock);
      return;
    }

  st = g_state;
  el = (uint32_t)((clock_systime_ticks() - g_phase_tick) * 1000 /
                  TICK_PER_SEC);
  pthread_mutex_unlock(&g_lock);

  limit = (st == VOICE_STATE_LISTENING) ? VOICE_WD_LISTEN_MS :
          (st == VOICE_STATE_THINKING)  ? VOICE_WD_THINK_MS :
          (st == VOICE_STATE_SPEAKING)  ? VOICE_WD_SPEAK_MS :
                                          VOICE_WD_IDLE_MS;

  if (el < limit)
    {
      return;
    }

  printf("[Voice] ⚠ 看门狗：状态 %d 卡了 %u ms（上限 %u），强制回到空闲"
         "（再点一次即可重新开始）\n", st, (unsigned)el, (unsigned)limit);

  ai_voice_stream_cancel();
  ai_voice_stream_finish();

  pthread_mutex_lock(&g_lock);
  g_gen++;
  g_cancel     = true;
  g_busy       = false;
  g_phase_tick = clock_systime_ticks();
  pthread_mutex_unlock(&g_lock);

  set_state(VOICE_STATE_IDLE);
}


/* 流式录音上传回调（voice worker 线程）：每 ~100ms 一块，POST 给服务器 */

static void voice_upload_cb(FAR const int16_t *pcm16, size_t samples,
                            void *arg)
{
  (void)arg;
  server_bridge_upload(pcm16, samples);
}

/* 流式播放的收数据停滞超时（多久收不到新数据算断，不是总时长）与
 * 防御上限（正常一条回复几百 KB；4MB 起就是服务器出问题了）。 */

#define VOICE_STREAM_STALL_MS    20000
#define VOICE_STREAM_MAX_BYTES   (4 * 1024 * 1024)

/* 边下边播：把 /voice/audio 的响应体直接喂给流式播放器（不落 SD）。
 * 返回服务器交付的字节数；负值失败（-ECANCELED = 用户点了停止）。 */

static int voice_tts_stream_cb(FAR const uint8_t *data, size_t len,
                               void *arg)
{
  (void)arg;
  return ai_voice_stream_play_push(data, len);
}

static int voice_play_audio_stream(void)
{
  char path[192];
  int n;
  int ret;

  if (server_bridge_voice_audio_path(path, sizeof(path)) < 0)
    {
      return -ENOTCONN;
    }

  ret = ai_voice_stream_play_begin();
  if (ret < 0)
    {
      printf("[Voice] 流式播放初始化失败: %d\n", ret);
      return ret;
    }

  n = server_bridge_get_stream(path, voice_tts_stream_cb, NULL,
                               VOICE_STREAM_STALL_MS,
                               VOICE_STREAM_MAX_BYTES);
  ai_voice_stream_play_end();

  if (n < 0)
    {
      printf("[Voice] 流式 GET 失败: %d\n", n);
    }

  return n;
}

static void *voice_worker(void *arg)
{
  char reply[1024];
  bool has_audio = false;
  bool cancelled = false;
  int session;
  int ms;
  int ret;
  uint32_t gen;
  (void)arg;

  pthread_mutex_lock(&g_lock);
  gen = g_gen;   /* 本代会话号：被看门狗作废后不再回写界面/状态 */
  pthread_mutex_unlock(&g_lock);

  /* 1. 开始服务器会话（未配置服务器 → 降级：提示 + 本地规则） */

  session = server_bridge_session_begin();
  if (session < 0)
    {
      snprintf(reply, sizeof(reply), "还没有配置服务器，请在 NSH 执行 plant voice server <ip> 🛠");
      if (g_text_cb != NULL)
        {
          g_text_cb(g_user_data, reply, false);
        }

      session_finish(gen);
      return NULL;
    }

  /* ⚠️ 2026-08-31 时序漏洞：取消可能发生在 session_begin 网络等待期间，
   * 而 ai_voice_stream_record() 开头会把 g_stream_cancel 重置为 false →
   * 取消标志被清掉、录音照常开始。这里在录音前再查一次 g_cancel。 */

  pthread_mutex_lock(&g_lock);
  cancelled = g_cancel || (gen != g_gen);
  pthread_mutex_unlock(&g_lock);
  if (cancelled)
    {
      printf("[Voice] 已取消（录音前），本次会话丢弃\n");
      session_finish(gen);
      return NULL;
    }

  /* 2. 流式录音（≤5s，静音 1.2s 自动停，再点按键=取消）→ 攒批上传
   * ⚠️ 2026-09-16：静音 15 块(1.5s)→12 块(1.2s)，配合自适应门限，
   * 说完很快就收（用户反馈"停了还在听、不自动结束"）。 */

  set_state(VOICE_STATE_LISTENING);
  ms = ai_voice_stream_record(voice_upload_cb, NULL, 5, 12);
  if (ms < 0)
    {
      printf("[Voice] stream record failed: %d\n", ms);
      session_finish(gen);
      return NULL;
    }

  if (ms < 300)
    {
      /* 录音太短（几乎静音 / 说完前就点了）→ 不打扰，静默回 IDLE */

      session_finish(gen);
      return NULL;
    }

  /* ⚠️ 2026-08-31 交互修正：录音中再点按键 = 「说完」→ 保留已录内容
   * 正常发送（ai_voice_stream_finish 提前结束录音，这里不再丢弃！）。
   * 只有 ms<300（没录到内容）才静默放弃。
   * 例外：voice_service_abort()（页面销毁）会设 g_cancel —— 此时
   * 丢弃不上传（页面没了，别浪费服务器请求）。「说完」只设 finish
   * 不设 g_cancel，所以此检查不会误伤正常说完。 */

  pthread_mutex_lock(&g_lock);
  cancelled = g_cancel || (gen != g_gen);
  pthread_mutex_unlock(&g_lock);
  if (cancelled)
    {
      printf("[Voice] 页面已销毁，丢弃本次录音\n");
      session_finish(gen);
      return NULL;
    }

  printf("[Voice] 录音 %d ms，发送给服务器...\n", ms);

  /* 3. finalize：服务器攒 WAV → MiMo 理解 + TTS → 回文本 */

  set_state(VOICE_STATE_THINKING);
  ret = server_bridge_finalize(reply, sizeof(reply), &has_audio);
  if (ret < 0)
    {
      printf("[Voice] finalize failed: %d\n", ret);
      snprintf(reply, sizeof(reply), "服务器连接失败，请稍后再试 🙏");
    }

  /* ⚠️ 2026-08-31 微信式随放随停：THINKING 网络等待中再点按键 = 取消，
   * 服务器结果回来后不再上屏、不再播放（请求无法中断，但结果丢弃）。
   * 注意：播放中取消由 ai_voice_play_file 内部 g_stream_cancel 中断。 */

  pthread_mutex_lock(&g_lock);
  cancelled = g_cancel || (gen != g_gen);
  pthread_mutex_unlock(&g_lock);
  if (cancelled)
    {
      printf("[Voice] 已取消（等待 AI 回复期间），结果丢弃\n");
      session_finish(gen);
      return NULL;
    }

  /* 4. 文本上屏（AI 回复，from_user=false） */

  if (g_text_cb != NULL)
    {
      g_text_cb(g_user_data, reply, false);
    }

  /* ⚠️ 2026-09-10 诊断：设备"不播放语音回复"到底卡在哪一步，
   * 这一行 + 下面分支的打印即可定位。 */

  printf("[Voice] finalize 回执: has_audio=%d text=\"%.60s\"\n",
         (int)has_audio, reply);

  /* 5. TTS 音频：边下边播（2026-09-16 时延修复）
   *
   * 旧路径是「先把整条音频下载到 SD 卡，再打开播放」——实测 795KB 的
   * 音频写 SD 只有 23.5KB/s（写卡把下载整个拖住），光下载就 34s，是
   * 「说完话等 43s 才出声」的最大来源；同一文件不写卡只要 4.6s。
   * 现在网络字节直接喂 I2S，攒够一批就出声，全程不碰 SD。
   *
   * 流式失败（服务器没给音频 / 中间设备干扰）时退回老路径，保住
   * 「能响」这条底线。状态先切 SPEAKING：下载与播放已经合一，这段
   * 等待里用户再点就是「停止播放」，与旧语义一致。 */

  if (has_audio)
    {
      int n;

      {
        uint32_t t_dl = clock_systime_ticks();

        set_state(VOICE_STATE_SPEAKING);
        ai_voice_stream_reset_cancel();
        n = voice_play_audio_stream();
        printf("[Voice] TTS 边下边播 %d B，耗时=%u ticks(10ms/tick)\n",
               n, (unsigned)(clock_systime_ticks() - t_dl));
      }

      pthread_mutex_lock(&g_lock);
      cancelled = g_cancel || (gen != g_gen);
      pthread_mutex_unlock(&g_lock);

      if (cancelled || ai_voice_stream_cancel_requested())
        {
          printf("[Voice] 已取消（TTS 播放期间），本轮不播放\n");
        }
      else if (n <= 0)
        {
          /* 老路径兜底：下载到 SD 再播 */

          int f;

          printf("[Voice] 流式播放失败(%d)，退回「下载到 SD 再播」\n", n);
          f = server_bridge_fetch_audio("/mnt/sd/tts_tmp.wav");
          printf("[Voice] TTS 音频下载 %d B\n", f);

          if (f > 0)
            {
              int pret;

              ai_voice_stream_reset_cancel();
              pret = ai_voice_play_file("/mnt/sd/tts_tmp.wav");
              if (pret < 0)
                {
                  printf("[Voice] 播放失败: %d（文件可能损坏或采样率不支持）\n",
                         pret);
                }
            }
        }
      else
        {
          printf("[Voice] TTS 播放完成（%d B）\n", n);
        }
    }
  else
    {
      printf("[Voice] 服务器未返回 TTS 音频，本轮只有文字回复（喇叭不出声）\n");
    }

  session_finish(gen);

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int voice_service_start(voice_state_cb_t state_cb,
                        voice_text_cb_t text_cb, void *user_data)
{
  int ret;

  pthread_mutex_lock(&g_lock);
  if (g_inited)
    {
      pthread_mutex_unlock(&g_lock);
      return 0;
    }

  ret = ai_voice_init();
  if (ret == 0)
    {
      g_state_cb = state_cb;
      g_text_cb = text_cb;
      g_user_data = user_data;
      g_inited = true;
    }

  pthread_mutex_unlock(&g_lock);
  return ret;
}

void voice_service_stop(void)
{
  pthread_mutex_lock(&g_lock);
  g_inited = false;
  g_state_cb = NULL;
  g_text_cb = NULL;
  pthread_mutex_unlock(&g_lock);
}

int voice_service_get_state(void)
{
  int state;

  pthread_mutex_lock(&g_lock);
  state = g_state;
  pthread_mutex_unlock(&g_lock);

  return state;
}

/* 是否有一轮会话正在跑（控制台 plant voice talk 回归 / 自动化用：
 * 只看 state 不够 —— worker 还没起来时 state 也是 IDLE）。 */

int voice_service_busy(void)
{
  int busy;

  pthread_mutex_lock(&g_lock);
  busy = g_busy ? 1 : 0;
  pthread_mutex_unlock(&g_lock);

  return busy;
}

/* ⚠️ 2026-08-31 页面销毁时强制中止会话（区别于 talk() 的「再点=说完」）。
 * 设取消标志 + 中断录音/播放循环，worker 各退出路径会复位 busy。 */

void voice_service_abort(void)
{
  pthread_mutex_lock(&g_lock);
  g_cancel = true;
  pthread_mutex_unlock(&g_lock);

  ai_voice_stream_cancel();   /* 中断录音循环 + 播放循环 */
  ai_voice_stream_finish();   /* 也中断 finish 分支（录音提前返回） */
}

int voice_service_talk(int seconds)
{
  pthread_t tid;
  pthread_attr_t attr;
  int ret;

  if (seconds < 1)
    {
      seconds = 1;
    }

  if (seconds > AI_VOICE_MAX_SECONDS)
    {
      seconds = AI_VOICE_MAX_SECONDS;
    }

  pthread_mutex_lock(&g_lock);
  if (g_busy)
    {
      /* ⚠️ 2026-08-31 微信式「再点」语义（按当前状态分派）：
       *  - LISTENING（录音中）：再点 = 说完 → ai_voice_stream_finish()
       *    提前结束录音，已录内容保留并发送给 AI（不是取消！）；
       *  - SPEAKING（TTS 播放中）：再点 = 停止播放（随放随停）；
       *  - THINKING / 窗口期：再点 = 取消，丢弃本次结果。
       * 不再是无脑取消整个会话。 */
      int st = g_state;

      if (st == VOICE_STATE_LISTENING)
        {
          ai_voice_stream_finish();
        }
      else if (st == VOICE_STATE_SPEAKING)
        {
          ai_voice_stream_cancel();
        }
      else
        {
          g_cancel = true;
          ai_voice_stream_cancel();
        }

      pthread_mutex_unlock(&g_lock);
      return 0;
    }

  g_busy = true;
  g_cancel = false;
  g_phase_tick = clock_systime_ticks();   /* 含会话建立期的计时锚点 */
  pthread_mutex_unlock(&g_lock);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  /* 语音 worker 栈加大到 8KB：含 reply[1024] + 上传回调栈 + play_file 局部
   * （默认 4KB 可能溢出 → 栈溢出破坏内存，表现类似黑屏/死机） */

  pthread_attr_setstacksize(&attr, 8192);

  ret = pthread_create(&tid, &attr, voice_worker, (void *)(intptr_t)seconds);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      pthread_mutex_lock(&g_lock);
      g_busy = false;
      g_cancel = false;
      pthread_mutex_unlock(&g_lock);
      return -ret;
    }

  return 0;
}
