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
#include <stdlib.h>
#include <unistd.h>

#include "voice_service.h"
#include "../components/box_aec/box_aec.h"
#include "server_bridge.h"
#include "../ai_module/ai_voice/ai_voice.h"
#include "../components/voice_agent/voice_agent.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_busy;
static bool g_background;
static int g_pending;
struct voice_text_buffers {char pending[401],spoken[1024],heard[401];};
static struct voice_text_buffers *text_buffers;
#define g_pending_text (text_buffers->pending)
static int16_t *g_recorded;
static size_t g_recorded_samples;
#define g_spoken (text_buffers->spoken)
static int g_last_error;
static volatile bool g_cancel;   /* 再点按键=取消（THINKING/窗口期丢弃）*/
static int g_state = VOICE_STATE_IDLE;
static voice_state_cb_t g_state_cb;
static voice_text_cb_t g_text_cb;
static void *g_user_data;
static bool g_inited;
static volatile uint32_t g_gen;        /* 会话代次（看门狗作废旧 worker 用） */
static clock_t  g_phase_tick;           /* 当前状态进入时刻（看门狗计时） */

/* 各阶段硬上限：超了就是"卡死了"，看门狗强制回到空闲（见下方注释） */

#define VOICE_WD_LISTEN_MS   30000    /* 录音（5s 上限 + 8s 硬兜底 + 余量） */
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

static void record_error(uint32_t gen,int error)
{
  pthread_mutex_lock(&g_lock);
  if(gen==g_gen&&!g_cancel)g_last_error=error;
  pthread_mutex_unlock(&g_lock);
}

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
  g_last_error=-ETIMEDOUT;
  g_cancel     = true;
  g_busy       = false;
  g_phase_tick = clock_systime_ticks();
  pthread_mutex_unlock(&g_lock);

  set_state(VOICE_STATE_IDLE);
}


/* The touch page uses the same ASR/provider path as hands-free. Capture
 * bounded PCM locally; the legacy finalize endpoint has an obsolete model. */
struct voice_capture {int16_t *pcm;size_t count,capacity;bool overflow;};
static void voice_collect_cb(FAR const int16_t *pcm,size_t samples,void *arg)
{
 struct voice_capture *c=arg;
 if(samples>c->capacity-c->count){c->overflow=true;ai_voice_stream_finish();return;}
 memcpy(c->pcm+c->count,pcm,samples*sizeof(*pcm));c->count+=samples;
}
static bool voice_obsolete(uint32_t gen)
{
 pthread_mutex_lock(&g_lock);bool obsolete=g_cancel||gen!=g_gen;
 pthread_mutex_unlock(&g_lock);return obsolete;
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

/* Cancellation and clearing the low-level stop flag must be one transition.
 * Otherwise a back/stop between the two can be accidentally cleared. */
static uint32_t speaking_serial;
uint32_t voice_service_speaking_token(void)
{
  pthread_mutex_lock(&g_lock);uint32_t token=g_busy&&!g_cancel&&g_state==VOICE_STATE_SPEAKING?speaking_serial:0;pthread_mutex_unlock(&g_lock);return token;
}
bool voice_service_abort_playback(uint32_t token)
{
  pthread_mutex_lock(&g_lock);
  if(!token||token!=speaking_serial||!g_busy||g_cancel||g_state!=VOICE_STATE_SPEAKING){pthread_mutex_unlock(&g_lock);return false;}
  g_cancel=true;
  ai_voice_background_finish();ai_voice_stream_cancel();ai_voice_stream_finish();
  pthread_mutex_unlock(&g_lock);return true;
}
static int start_speaking(uint32_t gen)
{
  pthread_mutex_lock(&g_lock);
  if(g_cancel||gen!=g_gen){pthread_mutex_unlock(&g_lock);return -ECANCELED;}
  ai_voice_stream_reset_cancel();
  if(++speaking_serial==0)++speaking_serial;
  g_state=VOICE_STATE_SPEAKING;g_phase_tick=clock_systime_ticks();
  pthread_mutex_unlock(&g_lock);
  /* Keep the short DMA queue fed while local recognition uses the CPU.
   * A same-priority decoder can otherwise hold an entire RR time slice,
   * much longer than the queued audio, and leave audible gaps.
   */
 #ifdef __NuttX__
  {
    struct sched_param priority;
    int policy;
    if(pthread_getschedparam(pthread_self(),&policy,&priority)==0&&
       priority.sched_priority<140){
      priority.sched_priority=140;
      int ret=pthread_setschedparam(pthread_self(),policy,&priority);
      if(ret)printf("[Voice] playback priority unavailable=%d\n",ret);
    }
  }
 #endif
  if(g_state_cb)g_state_cb(g_user_data,VOICE_STATE_SPEAKING);
  return 0;
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
 (void)arg;uint32_t gen;struct voice_capture c={.capacity=16000*12};
 pthread_mutex_lock(&g_lock);gen=g_gen;c.pcm=g_recorded;c.count=g_recorded_samples;
 g_recorded=NULL;g_recorded_samples=0;pthread_mutex_unlock(&g_lock);
 int ret=0;
 if(!c.pcm){
  c.pcm=malloc(c.capacity*sizeof(*c.pcm));
  if(!c.pcm){record_error(gen,-ENOMEM);session_finish(gen);return NULL;}
  if(voice_obsolete(gen)){free(c.pcm);session_finish(gen);return NULL;}
  set_state(VOICE_STATE_LISTENING);ret=ai_voice_stream_record_local(voice_collect_cb,&c,12,12);
 }
 if(voice_obsolete(gen)){free(c.pcm);session_finish(gen);return NULL;}
 if(ret<0||c.overflow||c.count<4800){
  if(ret>=0)ret=c.overflow?-EOVERFLOW:-ENOMSG;
  record_error(gen,ret);free(c.pcm);
  if(g_text_cb)g_text_cb(g_user_data,"没听清你的声音，请再说一次。",false);
  session_finish(gen);return NULL;
 }
 set_state(VOICE_STATE_THINKING);char heard[401],reply[401];
 ret=server_bridge_transcribe(c.pcm,c.count,heard,sizeof(heard));free(c.pcm);
 if(voice_obsolete(gen)){session_finish(gen);return NULL;}
 if(ret>=0){
  pthread_mutex_lock(&g_lock);snprintf(text_buffers->heard,sizeof(text_buffers->heard),"%s",heard);pthread_mutex_unlock(&g_lock);
  if(g_text_cb)g_text_cb(g_user_data,heard,true);
  ret=server_bridge_box_answer(heard,reply,sizeof(reply));
 }
 if(voice_obsolete(gen)){session_finish(gen);return NULL;}
 int answer_error=ret;
 if(ret<0){record_error(gen,ret);snprintf(reply,sizeof(reply),"现在还没有得到回答，请稍后再问我一次。");}
 else printf("[Voice] touch AI answer ready bytes=%lu\n",(unsigned long)strlen(reply));
 pthread_mutex_lock(&g_lock);snprintf(g_spoken,sizeof(g_spoken),"%s",reply);pthread_mutex_unlock(&g_lock);
 if(g_text_cb)g_text_cb(g_user_data,reply,false);
 ret=server_bridge_notice(reply);
 if(!voice_obsolete(gen)&&ret==0){ret=start_speaking(gen);if(ret==0)ret=voice_play_audio_stream();}
 bool cancelled=voice_obsolete(gen);
 if(!cancelled&&(ret<0||answer_error<0))record_error(gen,ret<0?ret:answer_error);
 printf("[Voice] status summary result=%d cancelled=%d\n",ret,cancelled);
 session_finish(gen);return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void voice_service_copy_heard(char *text,size_t capacity)
{
 if(!text||!capacity)return;
 pthread_mutex_lock(&g_lock);
 if(text_buffers)snprintf(text,capacity,"%s",text_buffers->heard);else text[0]=0;
 pthread_mutex_unlock(&g_lock);
}
int voice_service_start(voice_state_cb_t state_cb,
                        voice_text_cb_t text_cb, void *user_data)
{
  int ret;

  pthread_mutex_lock(&g_lock);
  if(!text_buffers){
    text_buffers=calloc(1,sizeof(*text_buffers));
    if(!text_buffers){pthread_mutex_unlock(&g_lock);return -ENOMEM;}
  }
  if (g_inited)
    {
      if(g_busy){pthread_mutex_unlock(&g_lock);return -EBUSY;}
      g_state_cb=state_cb;g_text_cb=text_cb;g_user_data=user_data;
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
  busy = (g_busy || g_pending) ? 1 : 0;
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

  ai_voice_background_finish();
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

  if(g_background){
    g_pending=1;pthread_mutex_unlock(&g_lock);
    ai_voice_background_finish();return 0;
  }
  g_busy = true;
  g_last_error=0;
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

int voice_service_last_error(void)
{
  int e;pthread_mutex_lock(&g_lock);e=g_last_error;pthread_mutex_unlock(&g_lock);return e;
}
struct notice_request {uint32_t generation; bool question; char text[401];};
static void *notice_worker(void *arg)
{
  struct notice_request *q=arg;
  uint32_t gen=q->generation;
  set_state(VOICE_STATE_THINKING);
  int answer_error=0;
  if(q->question){
    char answer[401];answer_error=server_bridge_box_answer(q->text,answer,sizeof(answer));
    pthread_mutex_lock(&g_lock);bool obsolete=g_cancel||gen!=g_gen;pthread_mutex_unlock(&g_lock);
    if(obsolete){free(q);session_finish(gen);return NULL;}
    if(answer_error<0){
      printf("[Voice] AI answer failed=%d\n",answer_error);
      snprintf(q->text,sizeof(q->text),"现在还没有得到回答，请稍后再问我一次。");
    }else{
      snprintf(q->text,sizeof(q->text),"%s",answer);
      printf("[Voice] AI answer ready bytes=%lu\n",(unsigned long)strlen(answer));
    }
  }
  pthread_mutex_lock(&g_lock);snprintf(g_spoken,sizeof(g_spoken),"%s",q->text);pthread_mutex_unlock(&g_lock);
  uint32_t hash=2166136261u;
  for(const unsigned char *p=(const unsigned char *)q->text;*p;p++)hash=(hash^*p)*16777619u;
  char cache[64];snprintf(cache,sizeof(cache),"/mnt/sd/hf_normal_v1/%08lx.wav",(unsigned long)hash);
  bool cached=access(cache,R_OK)==0;
  int ret=cached?0:server_bridge_notice(q->text);
  pthread_mutex_lock(&g_lock);
  bool cancelled=g_cancel||gen!=g_gen;
  pthread_mutex_unlock(&g_lock);
  if(!cancelled&&ret==0){
    if(start_speaking(gen)==0)ret=cached?ai_voice_play_file(cache):voice_play_audio_stream();
    else ret=-ECANCELED;
  }
  /* A readable cache entry can still have a truncated WAV or fail while
   * reading its payload. Do not report silent success or alter the RO SD.
   * Retry once from the healthy server, unless this turn was cancelled. */
  if(cached&&ret<0&&ret!=-ECANCELED&&!voice_obsolete(gen)){
    printf("[Voice] cached audio unavailable=%d; requesting fresh speech\n",ret);
    set_state(VOICE_STATE_THINKING);ret=server_bridge_notice(q->text);
    if(!voice_obsolete(gen)&&ret==0){ret=start_speaking(gen);if(ret==0)ret=voice_play_audio_stream();}
  }
  free(q);
  pthread_mutex_lock(&g_lock);
  cancelled=g_cancel||gen!=g_gen;
  if(!cancelled&&(ret<0||answer_error<0))g_last_error=ret<0?ret:answer_error;
  pthread_mutex_unlock(&g_lock);
  if(!cancelled&&ret<0){
    printf("[Voice] status summary failed=%d\n",ret);
  }
  printf("[Voice] status summary result=%d cancelled=%d\n",ret,cancelled);
  session_finish(gen);return NULL;
}
static int submit_notice(const char *text,bool question)
{
  if(!text||strlen(text)>400)return -EINVAL;
  int ret=voice_service_start(NULL,NULL,NULL);
  if(ret<0)return ret;
  if(voice_agent_get_dac_volume()==0)return 0;
  struct notice_request *q=malloc(sizeof(*q));
  if(!q)return -ENOMEM;
  pthread_mutex_lock(&g_lock);
  if(g_busy){pthread_mutex_unlock(&g_lock);free(q);return -EBUSY;}
  if(g_background){
    g_pending=question?3:2;snprintf(g_pending_text,sizeof(g_pending_text),"%s",text);
    pthread_mutex_unlock(&g_lock);free(q);ai_voice_background_finish();return 0;
  }
  g_busy=true;g_cancel=false;g_last_error=0;
  g_phase_tick=clock_systime_ticks();q->generation=g_gen;
  q->question=question;snprintf(q->text,sizeof(q->text),"%s",text);
  pthread_mutex_unlock(&g_lock);
  pthread_t tid;pthread_attr_t a;pthread_attr_init(&a);
  pthread_attr_setstacksize(&a,8192);
  pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);
  ret=pthread_create(&tid,&a,notice_worker,q);pthread_attr_destroy(&a);
  if(ret){uint32_t gen=q->generation;free(q);session_finish(gen);return -ret;}
  return 0;
}

int voice_service_say(const char *text)
{
  if(!text||strlen(text)>400)return -EINVAL;
  int ret=voice_service_start(NULL,NULL,NULL);
  if(ret<0)return ret;
  if(voice_agent_get_dac_volume()==0)return 0;
  return submit_notice(text,false);
}
int voice_service_reply_text(const char *text)
{return submit_notice(text,true);}

int voice_service_background_should_stop(void)
{
  int stop;pthread_mutex_lock(&g_lock);stop=g_pending||g_cancel;
  pthread_mutex_unlock(&g_lock);return stop;
}
static int background_record_impl(ai_voice_stream_cb_t cb,void *arg,int seconds,int silent_blocks)
{
  pthread_mutex_lock(&g_lock);
  if(g_background||g_pending||(g_busy&&g_state!=VOICE_STATE_SPEAKING)){
    pthread_mutex_unlock(&g_lock);return -EBUSY;
  }
  g_background=true;if(!g_busy)g_cancel=false;
  bool speaker=g_busy&&g_state==VOICE_STATE_SPEAKING;
  pthread_mutex_unlock(&g_lock);
  box_aec_set_speaker_active(speaker);
  int ret=ai_voice_background_record(cb,arg,seconds,silent_blocks);
  char text[401];int pending;bool cancelled;
  pthread_mutex_lock(&g_lock);g_background=false;pending=g_pending;g_pending=0;
  cancelled=g_cancel;snprintf(text,sizeof(text),"%s",g_pending_text);
  pthread_mutex_unlock(&g_lock);
  if(pending==1)voice_service_talk(5);
  if(pending==2)voice_service_say(text);
  if(pending==3)voice_service_reply_text(text);
  return pending||cancelled?-ECANCELED:ret;
}
int voice_service_background_record(ai_voice_stream_cb_t cb,void *arg,int seconds)
{return background_record_impl(cb,arg,seconds,8);}
int voice_service_background_monitor(ai_voice_stream_cb_t cb,void *arg,int seconds)
{return background_record_impl(cb,arg,seconds,0);}
void voice_service_copy_spoken(char *text,size_t capacity)
{
  if(!text||!capacity)return;
  pthread_mutex_lock(&g_lock);
  if(text_buffers)snprintf(text,capacity,"%s",g_spoken);else text[0]=0;
  pthread_mutex_unlock(&g_lock);
}
int voice_service_reply_pcm(const int16_t *pcm,size_t samples)
{
  if(!pcm||samples<3200||samples>16000*12)return -EINVAL;
  int init=voice_service_start(NULL,NULL,NULL);if(init<0)return init;
  int16_t *copy=malloc(samples*2);if(!copy)return -ENOMEM;
  memcpy(copy,pcm,samples*2);
  pthread_mutex_lock(&g_lock);
  if(g_busy||g_background||g_pending){pthread_mutex_unlock(&g_lock);free(copy);return -EBUSY;}
  g_recorded=copy;g_recorded_samples=samples;g_busy=true;g_cancel=false;g_last_error=0;
  g_phase_tick=clock_systime_ticks();pthread_mutex_unlock(&g_lock);
  pthread_t tid;pthread_attr_t attr;pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr,8192);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
  int ret=pthread_create(&tid,&attr,voice_worker,NULL);pthread_attr_destroy(&attr);
  if(ret){pthread_mutex_lock(&g_lock);g_recorded=NULL;g_recorded_samples=0;g_busy=false;pthread_mutex_unlock(&g_lock);free(copy);return -ret;}
  return 0;
}
