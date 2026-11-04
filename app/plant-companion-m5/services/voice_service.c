#include "../components/voice_wake/voice_wake.h"
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
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_PLANT_SOIL_SENSOR
#  include "sensor_service.h"
#  include "../components/sensor_driver/soil_sensor.h"
#endif
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <unistd.h>

#include "voice_service.h"
#include "voice_ui_result.h"
#include "../components/voice_ns/voice_ns.h"
#include "voice_upload.h"
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
#include "interaction_service.h"
#endif
#include "server_bridge.h"
#include "../ai_module/ai_voice/ai_voice.h"
#include "../components/voice_agent/voice_agent.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_busy;
static bool g_worker_active; /* Retain network ownership after watchdog invalidation. */
static volatile bool g_cancel;   /* 再点按键=取消（THINKING/窗口期丢弃）*/
static int g_state = VOICE_STATE_IDLE;
static voice_state_cb_t g_state_cb;
static voice_text_cb_t g_text_cb;
static voice_text_cb_t g_text_update_cb;
static void *g_user_data;
static bool g_inited;
static char *g_notice;
static volatile uint32_t g_gen;        /* 会话代次（看门狗作废旧 worker 用） */
static clock_t  g_phase_tick;           /* 当前状态进入时刻（看门狗计时） */
static int g_talk_silent_blocks = 12;   /* 0 = fixed-duration diagnostic */
static bool g_dialogue,g_followup_pending,g_wait_for_turn;
static clock_t g_followup_at;
/* Zero keeps the existing scheduler setup; 110 is an explicit diagnostic. */
static int g_diag_priority;
int voice_service_priority_diag(int value)
{
  if(value!=-1 && value!=0 && value!=110)return -EINVAL;
  pthread_mutex_lock(&g_lock);
  int rc=g_diag_priority;
  if(value!=-1)
    {if(g_busy || g_worker_active)rc=-EBUSY;else rc=g_diag_priority=value;}
  pthread_mutex_unlock(&g_lock);return rc;
}
static void voice_apply_priority_diag(void)
{
  int requested=g_diag_priority,rc=0,policy=-1;struct sched_param param={0};
  if(requested){param.sched_priority=requested;rc=pthread_setschedparam(pthread_self(),SCHED_RR,&param);}
  int get_rc=pthread_getschedparam(pthread_self(),&policy,&param);
  printf("[VoiceSched] requested=%d actual=%d policy=%d set_rc=%d get_rc=%d\n",
         requested,get_rc?-1:param.sched_priority,policy,rc,get_rc);
}

#define VOICE_DIALOGUE_IDLE_MS 30000
/* Children may pause between clauses. Match normal talk's 1.5s hangover;
 * 5x125ms prematurely cut a measured 1s within-question pause. */
#define VOICE_DIALOGUE_SILENCE_BLOCKS 12
static int g_ui_error;
static void voice_ui_fail(uint32_t gen,int error)
{
 pthread_mutex_lock(&g_lock);
 if(gen==g_gen && !g_cancel)g_ui_error=error;
 pthread_mutex_unlock(&g_lock);
}
void voice_service_ui_snapshot(struct voice_ui_snapshot *out)
{
 if(!out)return;
 pthread_mutex_lock(&g_lock);
 *out=(struct voice_ui_snapshot){.state=g_state,.error=g_ui_error,
 .generation=g_gen,.busy=g_busy||g_worker_active,.dialogue=g_dialogue,
 .cancelling=g_cancel&&(g_busy||g_worker_active),
 .capture_ready=ai_voice_ui_capture_ready(),.playback_active=ai_voice_ui_playback_active()};
 pthread_mutex_unlock(&g_lock);
}
static int voice_begin(int seconds,int silent_blocks,const char *notice,
                       bool dialogue,bool followup,unsigned fixture_id,bool wake_ack);
#ifdef CONFIG_PLANT_SOIL_SENSOR
static bool g_sensor_paused_by_voice;
#endif

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

static void voice_sensor_resume(void)
{
#ifdef CONFIG_PLANT_SOIL_SENSOR
  pthread_mutex_lock(&g_lock);
  if (g_sensor_paused_by_voice && !g_busy && !g_worker_active)
    {
      soil_sensor_set_paused(false);
      int ret = sensor_service_start(3000, NULL, NULL);
      /* The outgoing voice worker still owns its stack. Retry from the
       * idle UI watchdog tick if the sampling stack cannot fit yet.
       */
      if (ret == 0) g_sensor_paused_by_voice = false;
      else printf("[Voice] sensor resume deferred rc=%d\n", ret);
    }
  pthread_mutex_unlock(&g_lock);
#endif
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

static void session_complete(uint32_t gen,bool again,bool failed)
{
  bool mine,keep_wake_paused;
  pthread_mutex_lock(&g_lock);
  mine=(gen==g_gen);
  if(mine)
    {
      /* Publish the terminal phase while ownership still blocks new starts. */
      g_state=VOICE_STATE_IDLE;g_phase_tick=clock_systime_ticks();
      g_busy=false;
      g_followup_pending=g_dialogue && again && !g_cancel;
      if(g_followup_pending)
        {g_followup_at=clock_systime_ticks()+MSEC2TICK(350);
         printf("[Dialogue] %s; followup pending\n",failed?"reply failed, retry listening":"reply complete");}
      else if(g_dialogue)
        {g_dialogue=false;printf("[Dialogue] ended; wake required\n");}
      g_cancel=false;
      if(!g_dialogue)ai_voice_fixture_stop();
    }
  pthread_mutex_unlock(&g_lock);
  if(mine && g_state_cb)g_state_cb(g_user_data,VOICE_STATE_IDLE);
  pthread_mutex_lock(&g_lock);
  keep_wake_paused=g_followup_pending && voice_ns_probe_requested() && voice_ns_probe_get_mode()==3;
  /* resume() only stores flags. Finish the old RX lease before allowing a
   * new claim; a watchdog-invalidated worker also hands back its lease here. */
  if(!keep_wake_paused)voice_wake_resume();
  g_worker_active=false;
  pthread_mutex_unlock(&g_lock);
  /* This helper rechecks ownership under g_lock before starting sensors. */
  voice_sensor_resume();
}

static void session_finish(uint32_t gen){session_complete(gen,false,false);}

/* 看门狗本体：由 ui_task 的 1s 定时器调用（见 ui/ui_app.c）。
 * 为什么不用独立线程：实测这块板子上"独立 pthread + 定时等待"的看门狗
 * 线程跑一轮就消失了（ps 里线程没了，只触发一次），而 ui_task 的定时器
 * 整场演示都稳。看门狗本来就是要把界面从"聆听"里救回来，放在 ui_task
 * 里最对症，也省掉一个线程。 */

void voice_service_watchdog_poll(void)
{
  if(ai_voice_fixture_expired())
    {printf("[Fixture] lifetime expired; abort synthetic dialogue\n");voice_service_abort();}
  int      st;
  uint32_t el;
  uint32_t limit;
  bool waiting;clock_t observed_phase;uint32_t observed_gen;

  pthread_mutex_lock(&g_lock);
  if (!g_busy)
    {
      pthread_mutex_unlock(&g_lock);
      voice_sensor_resume();
      return;
    }

  st = g_state;observed_phase=g_phase_tick;observed_gen=g_gen;
  waiting=g_wait_for_turn;
  el = (uint32_t)((clock_systime_ticks() - g_phase_tick) * 1000 /
                  TICK_PER_SEC);
  pthread_mutex_unlock(&g_lock);

  limit = (st == VOICE_STATE_LISTENING) ? (waiting?VOICE_DIALOGUE_IDLE_MS+(AI_VOICE_DIALOGUE_MAX_SECONDS+8)*1000:VOICE_WD_LISTEN_MS) :
          (st == VOICE_STATE_THINKING)  ? VOICE_WD_THINK_MS :
          (st == VOICE_STATE_SPEAKING)  ? VOICE_WD_SPEAK_MS :
                                          VOICE_WD_IDLE_MS;

  if (el < limit)
    {
      return;
    }

  printf("[Voice] ⚠ 看门狗：状态 %d 卡了 %u ms（上限 %u），强制回到空闲"
         "（再点一次即可重新开始）\n", st, (unsigned)el, (unsigned)limit);

  pthread_mutex_lock(&g_lock);
  if(!g_busy || g_gen!=observed_gen || g_phase_tick!=observed_phase || g_state!=st)
    {pthread_mutex_unlock(&g_lock);return;}
  ai_voice_stream_cancel();ai_voice_stream_finish();
  g_gen++;
  g_cancel     = true;
  g_busy       = false;
  g_dialogue=false;g_followup_pending=false;ai_voice_fixture_stop();
  g_state=VOICE_STATE_IDLE;
  g_phase_tick = clock_systime_ticks();
  pthread_mutex_unlock(&g_lock);

  if(g_state_cb)g_state_cb(g_user_data,VOICE_STATE_IDLE);
  /* Keep wake parked while the old owner is draining. session_complete()
   * restores it only after all old capture/network children have joined. */
  voice_sensor_resume();
}


/* The recording callback only appends immutable PCM. A separate network
 * worker consumes published bytes; HTTP latency cannot block the capture IO
 * queue. Join that worker before releasing the PCM or completing this turn.
 */

struct voice_capture_buf_s
{
  uint8_t *data;
  size_t   capacity;
  size_t   length;
  size_t   dropped;
  uint32_t peak;
  uint64_t sum_sq;
  uint32_t nonzero;
  struct voice_upload *uploader;
  uint32_t gen;
  bool session_ready;
};

static bool voice_upload_cancelled(void *arg)
{
  struct voice_capture_buf_s *capture=arg;
  pthread_mutex_lock(&g_lock);
  bool cancelled=g_cancel || capture->gen!=g_gen;
  pthread_mutex_unlock(&g_lock);
  return cancelled;
}

static int voice_upload_session(void *arg)
{
  struct voice_capture_buf_s *capture=arg;
  if(capture->session_ready)return 0;
  uint32_t start=clock_systime_ticks();
  int rc=server_bridge_session_begin();
  printf("[Perf][Voice] session_begin=%u ms overlap=1\n",
         (unsigned)((clock_systime_ticks()-start)*1000/TICK_PER_SEC));
  if(rc>=0)capture->session_ready=true;
  return rc;
}

static int voice_upload_pcm(const uint8_t *data,size_t length,void *arg)
{
  (void)arg;
  return server_bridge_upload((const int16_t *)data,length/sizeof(int16_t));
}

static void voice_capture_cb(FAR const int16_t *pcm16, size_t samples,
                             void *arg)
{
  FAR struct voice_capture_buf_s *capture = arg;
  size_t bytes = samples * sizeof(int16_t);

  if (capture == NULL)
    {
      return;
    }

  if (capture->length + bytes > capture->capacity)
    {
      bytes = capture->capacity - capture->length;
    }

  if (bytes != 0)
    {
      memcpy(capture->data + capture->length, pcm16, bytes);
      capture->length += bytes;

      size_t i;
      size_t count = bytes / sizeof(int16_t);

      for (i = 0; i < count; i++)
        {
          int32_t value = pcm16[i];
          uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);

          if (magnitude > capture->peak)
            {
              capture->peak = magnitude;
            }

          capture->sum_sq += (uint64_t)magnitude * magnitude;
          if (magnitude != 0)
            {
              capture->nonzero++;
            }
        }
    }

  if (bytes != samples * sizeof(int16_t))
    {
      capture->dropped += samples - bytes / sizeof(int16_t);
    }
  voice_upload_publish(capture->uploader,capture->length);
}

/* 流式播放的收数据停滞超时（多久收不到新数据算断，不是总时长）与
 * 防御上限（正常一条回复几百 KB；4MB 起就是服务器出问题了）。 */

#define VOICE_STREAM_STALL_MS    20000
#define VOICE_STREAM_MAX_BYTES   (4 * 1024 * 1024)

/* 边下边播：把 /voice/audio 的响应体直接喂给流式播放器（不落 SD）。
 * 返回服务器交付的字节数；负值失败（-ECANCELED = 用户点了停止）。 */

struct tts_push_metrics {
  uint32_t count,max_ms,last_return,hash;size_t bytes;
};
static struct tts_push_metrics g_tts_push;

static int voice_tts_stream_cb(FAR const uint8_t *data, size_t len,
                               void *arg)
{
  (void)arg;
  uint32_t begin=clock_systime_ticks();
  int rc=ai_voice_stream_play_push(data,len);
  uint32_t end=clock_systime_ticks();
  unsigned elapsed=(unsigned)((end-begin)*1000/CLOCKS_PER_SEC);
  if(elapsed>g_tts_push.max_ms)g_tts_push.max_ms=elapsed;
  g_tts_push.count++;g_tts_push.last_return=end;
  if(rc==0)
    {g_tts_push.bytes+=len;
     for(size_t i=0;i<len;i++)g_tts_push.hash=(g_tts_push.hash^data[i])*16777619u;}
  return rc;
}

/* Camera rendering and LCD transfers can outrun the small TX window on
 * the single-core build. Real camera-on ABAB: default100 starved30times;
 * RR110 delivered the identical PCM without interior queue starvation.
 * Apply only around playback, then restore the caller's exact policy/priority.
 * No camera freeze, PCM modification, larger queue or capture-policy change. */
struct voice_playback_priority_s
{
  struct sched_param saved;
  int policy;
  bool raised;
};
static void voice_playback_priority_enter(struct voice_playback_priority_s *p)
{
  memset(p,0,sizeof(*p));
  int rc=pthread_getschedparam(pthread_self(),&p->policy,&p->saved);
  if(rc || p->saved.sched_priority>=110)
    {
      printf("[VoicePlaySched] raise=0 saved=%d get_rc=%d\n",
             rc?-1:p->saved.sched_priority,rc);
      return;
    }
  struct sched_param playback={.sched_priority=110};
  rc=pthread_setschedparam(pthread_self(),SCHED_RR,&playback);
  p->raised=rc==0;
  printf("[VoicePlaySched] raise=%d saved=%d target=110 set_rc=%d\n",
         p->raised,p->saved.sched_priority,rc);
}
static void voice_playback_priority_leave(struct voice_playback_priority_s *p)
{
  if(!p->raised)return;
  int rc=pthread_setschedparam(pthread_self(),p->policy,&p->saved);
  printf("[VoicePlaySched] restore=%d policy=%d rc=%d\n",
         p->saved.sched_priority,p->policy,rc);
  p->raised=false;
}

#include "voice_jitter.h"

static int voice_play_audio_stream(const char *override_path)
{
  char path[192];
  int n;
  int ret;

  if(override_path)strlcpy(path,override_path,sizeof(path));
  else if (server_bridge_voice_audio_path(path, sizeof(path)) < 0)
    {
      return -ENOTCONN;
    }

  ret = ai_voice_stream_play_begin();
  if (ret < 0)
    {
      printf("[Voice] 流式播放初始化失败: %d\n", ret);
      return ret;
    }

  struct voice_playback_priority_s playback_priority;
  voice_playback_priority_enter(&playback_priority);
  memset(&g_tts_push,0,sizeof(g_tts_push));g_tts_push.hash=2166136261u;
  bool error_notice=!strcmp(path,"/api/v1/voice/retry");
  n = voice_buffered_get_stream(path, voice_tts_stream_cb, NULL,
                               error_notice?1000:VOICE_STREAM_STALL_MS,
                               error_notice?192000:VOICE_STREAM_MAX_BYTES);
  uint32_t network_end=clock_systime_ticks();
  ai_voice_stream_play_end();
  voice_playback_priority_leave(&playback_priority);
  uint32_t playback_end=clock_systime_ticks();
  printf("[Perf][TTS] callbacks=%u bytes=%u hash=%08x max_push_ms=%u eof_after_push_ms=%u final_flush_ms=%u\n",
         g_tts_push.count,(unsigned)g_tts_push.bytes,g_tts_push.hash,g_tts_push.max_ms,
         g_tts_push.count?(unsigned)((network_end-g_tts_push.last_return)*1000/CLOCKS_PER_SEC):0,
         (unsigned)((playback_end-network_end)*1000/CLOCKS_PER_SEC));

  if (n < 0)
    {
      printf("[Voice] 流式 GET 失败: %d\n", n);
    }

  return n;
}

static void *voice_worker(void *arg)
{
  voice_apply_priority_diag();
#if defined(CONFIG_SMP) && CONFIG_SMP_NCPUS > 1
  cpu_set_t task_cpu_set;
  CPU_ZERO(&task_cpu_set);CPU_SET(0,&task_cpu_set);
  int task_affinity_rc=sched_setaffinity(0,sizeof(task_cpu_set),&task_cpu_set);
  printf("[CoreMap] task=voice target_cpu=0 actual_cpu=%d rc=%d\n",sched_getcpu(),task_affinity_rc);
#endif

  char reply[1024];
  bool has_audio = false;
  bool cancelled = false;
  bool turn_ok=false,wait_for_turn;
  bool fixture_capture=((uintptr_t)arg & 0x10000u)!=0;
  bool wake_ack=((uintptr_t)arg & 0x20000u)!=0;
  int session = -1;
  int ms;
  int ret;
  int max_seconds = (int)((uintptr_t)arg & 0xffffu);
  uint32_t gen;
  struct voice_capture_buf_s capture;

  if (max_seconds < 1)
    {
      max_seconds = 5;
    }

  pthread_mutex_lock(&g_lock);
  gen = g_gen;   /* 本代会话号：被看门狗作废后不再回写界面/状态 */
  wait_for_turn=g_wait_for_turn;
  pthread_mutex_unlock(&g_lock);

  char *notice=g_notice;g_notice=NULL;
  if(notice)
    {
      strlcpy(reply,notice,sizeof(reply));
      set_state(VOICE_STATE_THINKING);
      ret=server_bridge_notice(reply);
      free(notice);
      has_audio=ret==0;
      pthread_mutex_lock(&g_lock);cancelled=g_cancel||gen!=g_gen;pthread_mutex_unlock(&g_lock);
      if(cancelled){session_finish(gen);return NULL;}
      if(ret<0)printf("[Voice] notice synthesis failed rc=%d\n",ret);
      goto deliver_reply;
    }

  /* 1. 开始服务器会话（未配置服务器 → 降级：提示 + 本地规则） */

  if(!wait_for_turn){
    uint32_t t0 = clock_systime_ticks();

    session = server_bridge_session_begin();
    printf("[Perf][Voice] session_begin=%u ms\n",
           (unsigned)((clock_systime_ticks() - t0) * 1000 /
                      CLOCKS_PER_SEC));
  }

  if (!wait_for_turn && session < 0)
    {
      voice_ui_fail(gen,VOICE_UI_NETWORK);
      snprintf(reply, sizeof(reply), "网络还在准备，请稍等一下再叫我。");
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

  memset(&capture, 0, sizeof(capture));
  capture.capacity = (size_t)max_seconds * AI_VOICE_AI_RATE * sizeof(int16_t);
  capture.data = malloc(capture.capacity);
  if (capture.data == NULL)
    {
      voice_ui_fail(gen,VOICE_UI_SERVICE);
      printf("[Voice] capture buffer alloc failed: %u B\n",
             (unsigned)capture.capacity);
      session_finish(gen);
      return NULL;
    }

  /* 2. 流式录音（≤5s，静音 1.2s 自动停，再点按键=取消）→ 本地 PCM
   * ⚠️ 2026-09-16：静音 15 块(1.5s)→12 块(1.2s)，配合自适应门限，
   * 说完很快就收（用户反馈"停了还在听、不自动结束"）。 */

  set_state(VOICE_STATE_LISTENING);
  if(wait_for_turn && !wake_ack)usleep(250000); /* Speaker tail before resetting RX. */
  pthread_mutex_lock(&g_lock);cancelled=g_cancel||gen!=g_gen;pthread_mutex_unlock(&g_lock);
  if(cancelled){free(capture.data);session_finish(gen);return NULL;}
  capture.gen=gen;
  capture.session_ready=!wait_for_turn && session>=0;
  capture.uploader=voice_upload_start(capture.data,capture.capacity,
                                      voice_upload_session,voice_upload_pcm,
                                      voice_upload_cancelled,&capture);
  bool upload_overlap=capture.uploader!=NULL;
  uint32_t capture_start=clock_systime_ticks();
  printf("[Voice] upload mode=%s\n",upload_overlap?"overlap":"batch-fallback");
  ms = fixture_capture?
       ai_voice_stream_record_fixture_wait(voice_capture_cb,&capture,max_seconds,
                              g_talk_silent_blocks,wait_for_turn?VOICE_DIALOGUE_IDLE_MS:0,gen):
       wake_ack ? ai_voice_stream_record_memory_wake_wait(voice_capture_cb,&capture,max_seconds,
                              g_talk_silent_blocks,VOICE_DIALOGUE_IDLE_MS):
       ai_voice_stream_record_memory_wait(voice_capture_cb,&capture,max_seconds,
                              g_talk_silent_blocks,wait_for_turn?VOICE_DIALOGUE_IDLE_MS:0);
  if(upload_overlap)
    {
      size_t uploaded=0;
      size_t expected=ms>0?(size_t)ms*AI_VOICE_AI_RATE/1000*sizeof(int16_t):0;
      bool discard=ms<300 || capture.dropped || capture.length+32<expected ||
                   voice_upload_cancelled(&capture);
      uint32_t tail_start=clock_systime_ticks();
      int uploaded_rc=voice_upload_finish(capture.uploader,discard,&uploaded);
      if(uploaded==SIZE_MAX)
        {
          /* Retain PCM and ownership if this platform cannot join the child.
           * Never free a buffer still in use or start a competing session. */
          printf("[Voice] uploader join failed rc=%d; retain PCM and network ownership\n",uploaded_rc);
          return NULL;
        }
      capture.uploader=NULL;
      server_bridge_upload_release(); /* Worker joined: no live borrowed fd. */
      printf("[Voice] upload %u/%u B, %u ms\n",(unsigned)uploaded,
             (unsigned)capture.length,
             (unsigned)((clock_systime_ticks()-tail_start)*1000/TICK_PER_SEC));
      printf("[Perf][Voice] upload_overlap total=%u ms rc=%d\n",
             (unsigned)((clock_systime_ticks()-capture_start)*1000/TICK_PER_SEC),uploaded_rc);
      if(uploaded_rc<0 || (!discard && uploaded!=capture.length))
        {
          int ui_error=voice_ui_upload_failure_reason(ms,discard,
             capture.dropped || (ms>=300 && capture.length+32<expected));
          if(ui_error!=VOICE_UI_OK)voice_ui_fail(gen,ui_error);
          free(capture.data);session_finish(gen);return NULL;
        }
      session=capture.session_ready?0:-1;
    }
  if (ms < 0)
    {
      int ui_error=voice_ui_upload_failure_reason(ms,true,false);
      if(ui_error!=VOICE_UI_OK)voice_ui_fail(gen,ui_error);
      printf("[Voice] stream record failed: %d\n", ms);
      free(capture.data);
      session_finish(gen);
      return NULL;
    }

  if (ms < 300)
    {
      /* 录音太短（几乎静音 / 说完前就点了）→ 不打扰，静默回 IDLE */

      free(capture.data);
      session_finish(gen);
      return NULL;
    }

  /* A clean DMA counter does not prove the I/O queue delivered all PCM.
   * Refuse incomplete recordings instead of spending cloud latency on
   * clipped speech. Millisecond duration rounding loses at most 16 samples. */
  size_t expected_pcm=(size_t)ms*AI_VOICE_AI_RATE/1000*sizeof(int16_t);
  if(capture.dropped || capture.length+32<expected_pcm)
    {
      voice_ui_fail(gen,VOICE_UI_MIC);
      printf("[Voice] PCM integrity failed expected>=%u actual=%u dropped=%u\n",
             (unsigned)expected_pcm,(unsigned)capture.length,(unsigned)capture.dropped);
      free(capture.data);session_finish(gen);return NULL;
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
      free(capture.data);
      session_finish(gen);
      return NULL;
    }

  set_state(VOICE_STATE_THINKING);
  if(wait_for_turn && !upload_overlap)
    {
      uint32_t t0=clock_systime_ticks();
      session=server_bridge_session_begin();
      printf("[Perf][Voice] session_begin=%u ms\n",
             (unsigned)((clock_systime_ticks()-t0)*1000/TICK_PER_SEC));
      pthread_mutex_lock(&g_lock);cancelled=g_cancel||gen!=g_gen;pthread_mutex_unlock(&g_lock);
      if(session<0||cancelled){voice_ui_fail(gen,VOICE_UI_NETWORK);free(capture.data);session_finish(gen);return NULL;}
    }

  {
    size_t total = capture.length / sizeof(int16_t);

    printf("[Voice] 录音 %d ms，本地 PCM %u B（溢出丢 %u 采样）\n",
           ms, (unsigned)capture.length, (unsigned)capture.dropped);
    printf("[Voice] 采集统计: 峰值=%u 均方=%u 非零=%u/%u (%u%%)\n",
           (unsigned)capture.peak,
           (unsigned)(total ? capture.sum_sq / total : 0),
           (unsigned)capture.nonzero, (unsigned)total,
           (unsigned)(total ? capture.nonzero * 100 / total : 0));
  }

  printf("[Voice] 开始上传...\n");

  /* 3. Upload the completed PCM in 100 ms blocks. Capture has already
   * stopped, so a slow remote server can no longer starve I2S RX. */

  if(!upload_overlap)
  {
    const size_t upload_bytes = 65536;
    size_t offset = 0;
    int upload_failed = 0;
    uint32_t t_up = clock_systime_ticks();

    while (offset < capture.length)
      {
        size_t now = capture.length - offset;
        int uploaded;

        if (now > upload_bytes)
          {
            now = upload_bytes;
          }

        uploaded = server_bridge_upload(
                     (const int16_t *)(capture.data + offset), now / 2);
        if (uploaded < 0)
          {
            printf("[Voice] upload failed at %u/%u B: %d\n",
                   (unsigned)offset, (unsigned)capture.length, uploaded);
            upload_failed = 1;
            break;
          }

        offset += now;

        pthread_mutex_lock(&g_lock);
        cancelled = g_cancel || (gen != g_gen);
        pthread_mutex_unlock(&g_lock);
        if (cancelled)
          {
            break;
          }
      }

    printf("[Voice] upload %u/%u B, %u ms\n", (unsigned)offset,
           (unsigned)capture.length,
           (unsigned)((clock_systime_ticks() - t_up) * 1000 / TICK_PER_SEC));
    free(capture.data);

    if (cancelled)
      {
        printf("[Voice] 已取消（上传期间），结果丢弃\n");
        session_finish(gen);
        return NULL;
      }

    if (upload_failed)
      {
        voice_ui_fail(gen,VOICE_UI_NETWORK);
        session_finish(gen);
        return NULL;
      }
  }

  else free(capture.data);

  /* ASR/FAQ first. The server composes an uncached answer during this ack.
   * This audio path never replaces the current conversation session ID. */
  {
    bool thinking=false;char audio_path[192];uint32_t t0=clock_systime_ticks();
    int prepared=server_bridge_voice_prepare(&thinking,audio_path,sizeof(audio_path));
    if(prepared>=0)ai_voice_fixture_event(1);
    printf("[Perf][Voice] prepare=%u ms thinking=%d rc=%d\n",
           (unsigned)((clock_systime_ticks()-t0)*1000/TICK_PER_SEC),thinking,prepared);
    pthread_mutex_lock(&g_lock);cancelled=g_cancel||gen!=g_gen;pthread_mutex_unlock(&g_lock);
    if(cancelled){session_finish(gen);return NULL;}
    if(prepared==0 && thinking)
      {
        if(g_text_cb)g_text_cb(g_user_data,"让我好好想一想。",false);
        set_state(VOICE_STATE_SPEAKING);ai_voice_stream_reset_cancel();
        int played=voice_play_audio_stream(audio_path);
        printf("[Voice] thinking acknowledgement=%d B\n",played);
        pthread_mutex_lock(&g_lock);cancelled=g_cancel||gen!=g_gen;pthread_mutex_unlock(&g_lock);
        if(cancelled||ai_voice_stream_cancel_requested()){session_finish(gen);return NULL;}
        set_state(VOICE_STATE_THINKING);
      }
  }
  /* 4. finalize：服务器攒 WAV → MiMo 理解 + TTS → 回文本 */

  {
    uint32_t t0 = clock_systime_ticks();

    ret = server_bridge_finalize(reply, sizeof(reply), &has_audio);
    if(ret>=0 && server_bridge_voice_known_no_speech())voice_ui_fail(gen,VOICE_UI_NO_SPEECH);
    printf("[Perf][Voice] finalize(理解+TTS)=%u ms\n",
           (unsigned)((clock_systime_ticks() - t0) * 1000 /
                      CLOCKS_PER_SEC));
  }

  if (ret < 0)
    {
      voice_ui_fail(gen,VOICE_UI_SERVICE);
      printf("[Voice] finalize failed: %d\n",ret);
      snprintf(reply,sizeof(reply),"这次没有答上来，请再说一次。");
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

  if(ret<0)
    {
      /* A failure notice is never a successful answer or a command. The
       * authenticated endpoint only serves a precached WAV: no new model
       * or synthesis work, no replay of this failed question. */
      if(g_text_cb)g_text_cb(g_user_data,reply,false);
      set_state(VOICE_STATE_SPEAKING);ai_voice_stream_reset_cancel();
      int played=voice_play_audio_stream("/api/v1/voice/retry");
      pthread_mutex_lock(&g_lock);cancelled=g_cancel||gen!=g_gen;pthread_mutex_unlock(&g_lock);
      printf("[Voice] turn_failed rc=%d error_notice=%d B retry_listening=%d\n",
             ret,played,!cancelled && !ai_voice_stream_cancel_requested());
      session_complete(gen,!cancelled && !ai_voice_stream_cancel_requested(),true);
      return NULL;
    }

deliver_reply:
  turn_ok=ret>=0 && reply[0]!=0;
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
        n = voice_play_audio_stream(NULL);
        printf("[Voice] TTS 边下边播 %d B，耗时=%u ticks(10ms/tick)\n",
               n, (unsigned)(clock_systime_ticks() - t_dl));
      }

      pthread_mutex_lock(&g_lock);
      cancelled = g_cancel || (gen != g_gen);
      pthread_mutex_unlock(&g_lock);

      if (cancelled || ai_voice_stream_cancel_requested())
        {
          turn_ok=false;
          printf("[Voice] 已取消（TTS 播放期间），本轮不播放\n");
        }
      else if (n <= 0 && server_bridge_voice_reply_pending())
        {
          turn_ok=false;
          printf("[Voice] sentence stream incomplete; do not replay heard prefix\n");
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
                  turn_ok=false;
                  printf("[Voice] 播放失败: %d（文件可能损坏或采样率不支持）\n",
                         pret);
                }
            }
          else turn_ok=false;
        }
      else
        {
          printf("[Voice] TTS 播放完成（%d B）\n", n);
          if(server_bridge_voice_reply_pending())
            {
              bool refreshed_audio=false;
              int refreshed=server_bridge_finalize(reply,sizeof(reply),&refreshed_audio);
              printf("[Perf][Voice] complete_reply_refresh rc=%d pending=%d\n",
                     refreshed,server_bridge_voice_reply_pending());
              pthread_mutex_lock(&g_lock);
              cancelled=g_cancel||gen!=g_gen;
              pthread_mutex_unlock(&g_lock);
              if(refreshed==0 && !cancelled && g_text_update_cb)
                g_text_update_cb(g_user_data,reply,false);
            }
        }
    }
  else
    {
      printf("[Voice] 服务器未返回 TTS 音频，本轮只有文字回复（喇叭不出声）\n");
    }

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  pthread_mutex_lock(&g_lock);
  cancelled=g_cancel || gen!=g_gen;
  pthread_mutex_unlock(&g_lock);
  if (!cancelled)
    {
      char action[24],arg[64],id[64];
      if(server_bridge_take_action(action,sizeof(action),arg,sizeof(arg),id,sizeof(id))>0)
        {printf("[Interaction] accepted=%s arg=%s rc=%d\n",action,arg,interaction_submit(action,arg,id));ai_voice_fixture_event(3);}
    }
#endif
  session_complete(gen,turn_ok && !ai_voice_stream_cancel_requested(),false);

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void voice_service_set_text_update_cb(voice_text_cb_t update_cb)
{
  pthread_mutex_lock(&g_lock);
  g_text_update_cb=update_cb;
  pthread_mutex_unlock(&g_lock);
}

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
  voice_service_abort();
  pthread_mutex_lock(&g_lock);
  g_inited = false;
  g_state_cb = NULL;
  g_text_cb = NULL;
  g_text_update_cb = NULL;
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
  busy = (g_busy || g_worker_active) ? 1 : 0;
  pthread_mutex_unlock(&g_lock);

  return busy;
}

/* ⚠️ 2026-08-31 页面销毁时强制中止会话（区别于 talk() 的「再点=说完」）。
 * 设取消标志 + 中断录音/播放循环，worker 各退出路径会复位 busy。 */

void voice_service_abort(void)
{
  pthread_mutex_lock(&g_lock);
  g_cancel = true;
  g_dialogue=false;g_followup_pending=false;ai_voice_fixture_stop();
  ai_voice_stream_cancel();ai_voice_stream_finish();
  if(!g_busy && !g_worker_active)voice_wake_resume();
  pthread_mutex_unlock(&g_lock);
}

static int voice_begin(int seconds, int silent_blocks,const char *notice,
                       bool dialogue,bool followup,unsigned fixture_id,bool wake_ack)
{
  pthread_t tid;
  pthread_attr_t attr;
  int ret;
  bool fixture_required=false;

  if (seconds < 1)
    {
      seconds = 1;
    }

  int record_limit=(dialogue||followup)?AI_VOICE_DIALOGUE_MAX_SECONDS:AI_VOICE_MAX_SECONDS;
  if (seconds > record_limit)
    {
      seconds = record_limit;
    }

  pthread_mutex_lock(&g_lock);
  if(followup && (!g_dialogue || !g_followup_pending))
    {pthread_mutex_unlock(&g_lock);return -ECANCELED;}
  if(fixture_id && (!g_inited || g_busy || g_worker_active || g_dialogue || ai_voice_fixture_active()))
    {int frc=!g_inited?-EAGAIN:-EBUSY;pthread_mutex_unlock(&g_lock);return frc;}
  if(!g_busy && !fixture_id && !notice && !followup && ai_voice_fixture_active())
    {pthread_mutex_unlock(&g_lock);return -EBUSY;}
  if (g_busy)
    {
      if(notice||followup){pthread_mutex_unlock(&g_lock);return -EBUSY;}
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

  if(g_worker_active){pthread_mutex_unlock(&g_lock);return -EBUSY;}
  if(fixture_id)
    {int frc=ai_voice_fixture_arm(fixture_id,g_gen);if(frc<0){pthread_mutex_unlock(&g_lock);return frc;}}
  fixture_required=ai_voice_fixture_active();
  if(notice){g_notice=strdup(notice);if(!g_notice){pthread_mutex_unlock(&g_lock);return -ENOMEM;}}
  g_ui_error=VOICE_UI_OK;
  g_busy = true;
  g_worker_active = true;
  g_cancel = false;
  if(!notice && !followup)g_dialogue=dialogue;
  g_followup_pending=false;
  g_wait_for_turn=!notice && g_dialogue;
  g_talk_silent_blocks = silent_blocks > 0 ? silent_blocks : 0;
  g_phase_tick = clock_systime_ticks();   /* 含会话建立期的计时锚点 */
  pthread_mutex_unlock(&g_lock);

  bool neural_shadow=!notice && voice_ns_probe_requested() && voice_ns_probe_get_mode()==3;
  ret = neural_shadow?voice_wake_pause_release():voice_wake_pause();
  if (ret < 0)
    {
      pthread_mutex_lock(&g_lock);free(g_notice);g_notice=NULL;g_busy=false;g_dialogue=false;ai_voice_fixture_stop();voice_wake_resume();g_worker_active=false;pthread_mutex_unlock(&g_lock);
      voice_sensor_resume();return ret;
    }

#ifdef CONFIG_PLANT_SOIL_SENSOR
  /* Claim the voice session first. Busy-button actions must not acquire
   * a fresh sensor pause that the current worker no longer owns.
   */
  if (sensor_service_is_running())
    {
      sensor_service_stop();
      soil_sensor_set_paused(true);
      g_sensor_paused_by_voice = true;
    }
#endif

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  /* 语音 worker 栈加大到 8KB：含 reply[1024] + 上传回调栈 + play_file 局部
   * （默认 4KB 可能溢出 → 栈溢出破坏内存，表现类似黑屏/死机） */

  /* The SDK NS AnalyzeCore frame alone needs 5792 bytes, before recorder,
   * FFT/math and libc frames. Keep ordinary turns at their existing size;
   * an explicitly requested diagnostic gets 16KB. The probe also checks
   * its actual stack, guarding a trace toggle racing this snapshot. */
  bool ns_trace=voice_ns_probe_requested();
  pthread_attr_setstacksize(&attr, ns_trace?16384:8192);
  if(ns_trace)printf("[Voice] worker_stack=16384 ns_trace=1\n");

  ret=0;
#ifdef CONFIG_SMP
  /* Set affinity before the child can inherit the UI core and run. */
  cpu_set_t initial_cpu_set;CPU_ZERO(&initial_cpu_set);CPU_SET(0,&initial_cpu_set);
  ret=pthread_attr_setaffinity_np(&attr,sizeof(initial_cpu_set),&initial_cpu_set);
#endif
  uintptr_t worker_arg=(uintptr_t)(unsigned)seconds | (fixture_required?0x10000u:0u) | (wake_ack?0x20000u:0u);
  if(ret==0)ret = pthread_create(&tid, &attr, voice_worker, (void *)worker_arg);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      pthread_mutex_lock(&g_lock);
      free(g_notice);g_notice=NULL;
      g_busy = false;
      voice_wake_resume();
      g_worker_active = false;
      g_cancel = false;
      g_dialogue=false;g_followup_pending=false;ai_voice_fixture_stop();
      pthread_mutex_unlock(&g_lock);
      voice_sensor_resume();
      return -ret;
    }

  return 0;
}

int voice_service_talk(int seconds)
{
  return voice_service_talk_ex(seconds, 12);
}

int voice_service_talk_ex(int seconds,int silent_blocks)
{return voice_begin(seconds,silent_blocks,NULL,false,false,0,false);}
int voice_service_fixture_start(unsigned program)
{
  if(program<1 || program>3)return -EINVAL;
  return voice_begin(AI_VOICE_DIALOGUE_MAX_SECONDS,5,NULL,true,false,program,false);
}
int voice_service_wake_start(void)
{return voice_begin(AI_VOICE_DIALOGUE_MAX_SECONDS,VOICE_DIALOGUE_SILENCE_BLOCKS,NULL,true,false,0,true);}
int voice_service_dialogue_start(void)
{return voice_begin(AI_VOICE_DIALOGUE_MAX_SECONDS,VOICE_DIALOGUE_SILENCE_BLOCKS,NULL,true,false,0,false);}
bool voice_service_dialogue_active(void)
{pthread_mutex_lock(&g_lock);bool active=g_dialogue;pthread_mutex_unlock(&g_lock);return active;}
void voice_service_dialogue_poll(bool blocked)
{
  pthread_mutex_lock(&g_lock);
  bool next=g_dialogue && g_followup_pending && !g_busy && !g_worker_active &&
            (int32_t)(clock_systime_ticks()-g_followup_at)>=0;
  pthread_mutex_unlock(&g_lock);
  if(next && !blocked)
    {int rc=voice_begin(AI_VOICE_DIALOGUE_MAX_SECONDS,VOICE_DIALOGUE_SILENCE_BLOCKS,NULL,false,true,0,false);printf("[Dialogue] followup rc=%d\n",rc);}
}
int voice_service_say(const char *text)
{
  if(!text||!*text||strlen(text)>720)return -EINVAL;
  return voice_begin(1,0,text,false,false,0,false);
}
