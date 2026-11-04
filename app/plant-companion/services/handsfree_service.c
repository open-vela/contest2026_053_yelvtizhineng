#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include "handsfree_service.h"
#include "power_service.h"
#include "handsfree_audio_gate.h"
#include "handsfree_capture_window.h"
#include "handsfree_interrupt_queue.h"
#include "../components/box_aec/box_aec.h"
#include "voice_service.h"
#include "../hal/hal_i2s.h"
#include "server_bridge.h"
#include "ai_service.h"
#include "../components/box_phrase/box_phrase.h"

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static bool started,enabled=true,ready;
static bool audio_ready;
static bool interrupt_ready;
/* Software-reference full duplex did not pass physical short-command
 * acceptance. Keep the verified turn-taking interaction until hardware
 * echo-reference capture passes its own acceptance. */
static const bool playback_interrupts=false;
static const bool local_playback_recognizer=false;
static unsigned listener_generation;
static _Atomic bool sleeping;
static _Atomic bool hearing_speech;
static _Atomic bool power_interacting;
static _Atomic bool screen_idle_requested;
bool handsfree_service_interacting(void){return atomic_load(&power_interacting);}
static _Atomic bool trace_next_wake;
static _Atomic bool capturing;
static _Atomic bool asr_pending;
static _Atomic bool microphone_enabled=true;
bool handsfree_service_hearing(void){return hearing_speech;}
bool handsfree_service_capturing(void){return atomic_load(&capturing);}
bool handsfree_service_processing(void){return atomic_load(&asr_pending);}
static struct hf_gate gate;
struct hf_text_buffers {char injected[512],announcement[401],heard[512];};
static struct hf_text_buffers *text_buffers;
#define injected (text_buffers->injected)
#define announcement (text_buffers->announcement)
static unsigned recordings,asr_requests,wakes,actions,errors;
static unsigned local_commands;
static enum hf_action last_action;
static const char *hint="小伴正在准备";
static int last_error;
struct capture {const char *transcript;int16_t *pcm;size_t count,capacity,prefix,speech_start,last_offer;uint32_t speech_token;bool overflow;bool monitor,allow_stop,local_keywords,wake_keywords,playback_done,voice_started,utterance_ended;uint32_t quiet_samples,wake_quiet_samples;int local;};
static uint32_t now_ms(void){return (uint32_t)(clock_systime_ticks()*1000/TICK_PER_SEC);}
static void offer_interrupt(struct capture *c,bool final)
{
 if(!c->speech_token||!c->voice_started||c->count<=c->speech_start)return;
 size_t available=c->count-c->speech_start;
 if(available<1600 || (!final && (available<32000 || c->count-c->last_offer<32000)))return;
 size_t n=available>48000?48000:available;char spoken[1024];
 voice_service_copy_spoken(spoken,sizeof(spoken));
 if(hf_interrupt_offer(c->speech_token,c->pcm+c->count-n,n,spoken)==0)c->last_offer=c->count;
}
static void collect(const int16_t *pcm,size_t count,void *arg)
{
 struct capture *c=arg;
 if(c->speech_token && voice_service_speaking_token()==c->speech_token){
  int confirmed=hf_interrupt_take(c->speech_token);
  if(confirmed){c->local=confirmed;ai_voice_background_finish();return;}
 }

 if(voice_service_background_should_stop()){ai_voice_background_finish();return;}
 pthread_mutex_lock(&lock);bool stop=!enabled||injected[0]||announcement[0];pthread_mutex_unlock(&lock);
 if(stop||!microphone_enabled){ai_voice_background_finish();return;}
 if(count>c->capacity-c->count){c->overflow=true;ai_voice_background_finish();return;}
 memcpy(c->pcm+c->count,pcm,count*2);c->count+=count;
 if(!c->monitor && !hearing_speech && count){
  uint64_t energy=0;for(size_t i=0;i<count;i++){int32_t v=pcm[i];energy+=(uint64_t)(v*v);}
  if(energy/count>=90000u)hearing_speech=true;
 }

 /* Switch back to ordinary command capture as soon as speech ends. Do
  * not spend another utterance window recognizing our own last reply. */
 if(c->allow_stop&&!voice_service_busy()){
  c->playback_done=true;ai_voice_background_finish();return;
 }
 if((c->monitor||c->wake_keywords)&&!c->allow_stop&&c->local==1){
  if(hf_wake_tail_update(&c->wake_quiet_samples,pcm,count))ai_voice_background_finish();
  return;
 }
 /* A command candidate is provisional until a quiet boundary: keep feeding
  * the rest of the utterance so a following cancellation can veto it. */
 if((c->monitor||c->wake_keywords)&&(!c->local||box_phrase_command_text(c->local))){
  int step=hf_audio_gate_update(&c->voice_started,&c->quiet_samples,pcm,count,c->allow_stop);
  if(step&HF_AUDIO_BEGIN){
   hearing_speech=true;
   size_t retain=c->allow_stop?8000:16000;
   size_t n=c->count<retain?c->count:retain;
   c->speech_start=c->count-n;
   if(!c->allow_stop||c->local_keywords)c->local=box_phrase_stream_feed(c->pcm+c->count-n,n,c->allow_stop);
  }else if((step&HF_AUDIO_FEED)&&(!c->allow_stop||c->local_keywords))c->local=box_phrase_stream_feed(pcm,count,c->allow_stop);
  if(!c->allow_stop&&box_phrase_command_text(c->local)&&c->quiet_samples>=4800u)step|=HF_AUDIO_END;
  if(step&HF_AUDIO_END)c->utterance_ended=true;
  if(c->allow_stop&&!c->local_keywords)offer_interrupt(c,(step&HF_AUDIO_END)!=0);
  if((c->local&&c->allow_stop) || (step&HF_AUDIO_END))ai_voice_background_finish();
  if(hf_capture_quiet_deadline(c->count,c->prefix,!c->monitor||c->allow_stop,c->voice_started))ai_voice_background_finish();
 }
}
/* ASR and its wait listener share the existing PCM allocation. The HTTP
 * layer grants ownership only after sending the old request in full. */
struct asr_listener {struct capture audio;unsigned epoch;size_t slice_start;bool begun,complete;int error;};
static bool asr_listener_cancelled(struct asr_listener *l)
{
 pthread_mutex_lock(&lock);
 bool stop=!enabled||!microphone_enabled||l->epoch!=listener_generation||injected[0]||announcement[0];
 pthread_mutex_unlock(&lock);return stop||voice_service_busy();
}
/* A short noise spike can open VAD without forming a new utterance.
 * Keep the pending result unless ordinary audio has at least the existing
 * 200ms speech evidence. Strong local wake keeps its separate fast path. */
static bool asr_ordinary_complete(const struct capture *c)
{
 return c->voice_started&&c->utterance_ended&&!c->overflow&&
   c->count>c->speech_start+1600u&&
   hf_followup_has_speech(c->pcm+c->speech_start,c->count-c->speech_start);
}
static void asr_collect(const int16_t *pcm,size_t count,void *arg)
{
 struct asr_listener *l=arg;struct capture *c=&l->audio;
 if(asr_listener_cancelled(l)){l->error=-ECANCELED;ai_voice_background_finish();return;}
 collect(pcm,count,c);
 if((c->local==1&&c->wake_quiet_samples>=8000u)||
    asr_ordinary_complete(c))l->complete=true;
 /* Check the socket at least once per 250ms of newly captured audio. */
 if(l->complete||c->utterance_ended||c->overflow||c->count-l->slice_start>=4008u)ai_voice_background_finish();
}
static int asr_listen(void *arg)
{
 struct asr_listener *l=arg;struct capture *c=&l->audio;
 if(asr_listener_cancelled(l))return l->error=-ECANCELED;
 if(!l->begun){
  int ret=box_phrase_stream_begin();if(ret<0)return l->error=ret;
  l->begun=true;capturing=true;hearing_speech=false;
 }
 if(c->utterance_ended||c->count+16000u>c->capacity){
  if(l->complete||c->local==1){l->complete=true;return 1;}
  /* Bound quiet/ordinary speech storage without allocating another clip.
   * Retain onset context at the hard capacity boundary. */
  size_t keep=c->utterance_ended?0:16000u;
  if(keep)memmove(c->pcm,c->pcm+c->count-keep,keep*2);
  int16_t *buffer=c->pcm;size_t capacity=c->capacity;
  *c=(struct capture){.pcm=buffer,.capacity=capacity,.count=keep,.monitor=true};
  box_phrase_reset();box_phrase_stream_begin();hearing_speech=false;
 }
 l->slice_start=c->count;
 int ret=voice_service_background_monitor(asr_collect,l,1);
 if(l->error)return l->error;
 if(ret<0)return l->error=ret;
 if(c->overflow)return l->error=-EOVERFLOW;
 return l->complete?1:0;
}
static bool asr_listen_finish(struct asr_listener *l)
{
 /* A response arriving halfway through a new wake must not hand the mic
  * straight to a spoken reply. Finish that live phrase, bounded by time. */
 uint32_t start=now_ms();
 while(l->begun&&!l->complete&&!l->error&&l->audio.voice_started&&
       !l->audio.utterance_ended&&(uint32_t)(now_ms()-start)<(l->audio.local==1?3000u:8000u)){
  if(asr_listen(l))break;
 }
 if(l->begun&&!l->error&&!l->audio.local&&l->audio.voice_started)
  l->audio.local=box_phrase_stream_end_live(0);
 capturing=false;hearing_speech=false;
 return !l->error&&(l->audio.local==1||
   asr_ordinary_complete(&l->audio));
}
static void say(const char *text)
{
 char clean[401];snprintf(clean,sizeof(clean),"%s",text);
 for(char *p=clean;*p;p++)if((unsigned char)*p<32)*p=' ';
 size_t length=strlen(clean);
 for(size_t i=0;i<length;){unsigned char lead=clean[i];size_t step=lead<128?1:lead<224?2:lead<240?3:4;if(i+step>length){clean[i]=0;break;}i+=step;}
 if(!clean[0])return;
 if(!audio_ready){printf("[HF] feedback unavailable: audio not ready\n");return;}
 int ret=voice_service_say(clean);
 if(ret<0){last_error=ret;errors++;printf("[HF] feedback failed=%d\n",ret);}
}
static bool echo_of_speaker(const char *heard)
{
 char spoken[1024],a[1024],b[512];voice_service_copy_spoken(spoken,sizeof(spoken));
 hf_normalize(spoken,a,sizeof(a));hf_normalize(heard,b,sizeof(b));
 return b[0]&&strstr(a,b)!=NULL;
}
static void dispatch(struct hf_command *command,const struct capture *audio)
{
 if(command->action==HF_IGNORE)return;
 box_power_activity();
 last_action=command->action;
 if(command->action!=HF_SLEEP&&command->action!=HF_STOP)box_phrase_reset();
 if(command->woke){wakes++;sleeping=false;}
 if(command->action==HF_SLEEP){sleeping=true;pthread_mutex_lock(&lock);announcement[0]=0;pthread_mutex_unlock(&lock);}
 printf("[HF] dispatch=%s value=%d woke=%d\n",hf_action_name(command->action),command->value,command->woke);
 if(command->action==HF_WAKE){say(server_bridge_configured()?"我在呢，你想做什么？":"我在呢，网络恢复后就可以和我说话啦。");return;}
 if(command->action==HF_ASK){
  if(audio&&(audio->count||audio->transcript)){
   struct hf_command talk={HF_TALK,0,false,{0}};char reply[401];
   int ui=ui_app_voice_action(&talk,reply,sizeof(reply));
   if(ui<0){last_error=ui;errors++;say("刚才没有操作成功，请再说一次。");return;}
   if(ui>0){say(reply);return;}
   int ret=audio->transcript?voice_service_reply_text(audio->transcript):voice_service_reply_pcm(audio->pcm,audio->count);
   if(ret<0){last_error=ret;errors++;say("刚才没听清，可以再说一次吗？");}
  }else say("这是文字指令测试。真实提问请直接对我说话。");
  return;
 }
 if(command->action==HF_STOP){char reply[401];if(!command->value)voice_service_abort();ui_app_voice_action(command,reply,sizeof(reply));hal_i2s_rx_discard_pending();return;}
 char reply[401]={0};int ret=ui_app_voice_action(command,reply,sizeof(reply));
 if(ret<0){last_error=ret;errors++;say("刚才没有操作成功，请再说一次。");return;}
 actions++;say(reply);
}
/* Only known acoustic command IDs can use the offline path. Unknown audio
 * and free questions retain the ordinary cloud transcription path. */
static bool dispatch_local(int id,bool awake,const struct capture *audio)
{
 const char *text=box_phrase_command_text(id);
 if(!awake||!text||!audio->utterance_ended)return false;
 struct hf_command c=hf_parse(&gate,text,now_ms());
 if(c.action==HF_IGNORE||c.action==HF_ASK||c.action==HF_WAKE)return false;
 local_commands++;
 printf("[HFLocal] dispatch=%s id=%d probability=%d/1000 samples=%lu asr_bypassed=1\n",
   hf_action_name(c.action),id,box_phrase_command_probability(),(unsigned long)audio->count);
 dispatch(&c,NULL);return true;
}
static void *worker(void *arg)
{
 (void)arg;
 /* Network RX and LVGL run at 100. Recognition must yield to both;
  * otherwise their same-priority RR slices starve streamed playback. */
 {
  struct sched_param priority;int policy;
  if(pthread_getschedparam(pthread_self(),&policy,&priority)==0&&priority.sched_priority>90){
   priority.sched_priority=90;int ret=pthread_setschedparam(pthread_self(),policy,&priority);
   printf("[HF] listener priority=90 result=%d\n",ret);
  }
 }
 usleep(1000000);
 int ret=box_phrase_init();
 if(ret<0){last_error=ret;errors++;hint="语音准备失败，请大人帮忙";printf("[HF] startup failed=%d\n",ret);return NULL;}
 int16_t *pcm=malloc(16000*12*2);
 if(!pcm){last_error=-ENOMEM;hint="语音暂时没准备好";return NULL;}

 printf("[HF] local recognizer loaded; waiting for audio\n");
 size_t carry=0;uint32_t retry_at=0,interrupt_retry_at=0;unsigned retries=0;
 bool speaker_tail=false;uint32_t listen_after=0;
 for(;;){
  if(atomic_exchange(&screen_idle_requested,false)){
   gate.awake=false;sleeping=true;hearing_speech=false;power_interacting=false;
   carry=0;box_phrase_reset();hal_i2s_rx_discard_pending();
  }
  pthread_mutex_lock(&lock);bool run=enabled;unsigned epoch=listener_generation;
  char text[512]={0},notice[401]={0};
  if(run&&audio_ready&&!voice_service_busy()){
   if(announcement[0]){snprintf(notice,sizeof(notice),"%s",announcement);announcement[0]=0;}
   else if(injected[0]){snprintf(text,sizeof(text),"%s",injected);injected[0]=0;}
  }
  pthread_mutex_unlock(&lock);
  if(!run){hf_interrupt_set_turn(0);carry=0;hint="语音唤醒已关闭";usleep(200000);continue;}
  if(notice[0]){hf_keep_awake(&gate,now_ms());say(notice);continue;}
  if(text[0]){struct hf_command c=hf_parse(&gate,text,now_ms());struct capture input={.transcript=text};dispatch(&c,&input);continue;}
  if(!audio_ready){
   if((int32_t)(now_ms()-retry_at)>=0){
    ret=voice_service_start(NULL,NULL,NULL);
    if(ret<0){last_error=ret;errors++;hint="麦克风未就绪，正在重试";retries++;
     retry_at=now_ms()+(retries<3?2000:10000);printf("[HF] audio retry=%u error=%d\n",retries,ret);
    }else{box_aec_init();hal_i2s_rx_discard_pending();audio_ready=ready=true;last_error=0;printf("[HF] ready wake=你好小伴 local=1\n");}
   }
   if(!audio_ready){usleep(100000);continue;}
  }
  if(playback_interrupts&&!interrupt_ready&&!voice_service_busy()&&(int32_t)(now_ms()-interrupt_retry_at)>=0){
   int error=hf_interrupt_start();interrupt_ready=error==0;interrupt_retry_at=now_ms()+10000;
   printf("[HF] interrupt worker ready=%d error=%d\n",interrupt_ready,error);
  }
  int state=voice_service_get_state();bool busy=voice_service_busy();bool speaking=busy&&state==VOICE_STATE_SPEAKING;
  if(busy&&!sleeping)hf_keep_awake(&gate,now_ms());
  if(!microphone_enabled){carry=0;hearing_speech=false;hf_interrupt_set_turn(0);hint="正在测试，请稍等";usleep(20000);continue;}
  /* Only a current awake playback token may enter the AEC/interrupt
   * path. Other busy states keep the microphone owned by their worker. */
  if(busy && !(playback_interrupts&&speaking&&!sleeping&&interrupt_ready&&hf_is_awake(&gate,now_ms())&&box_aec_ready())){
   hf_interrupt_set_turn(0);hearing_speech=false;carry=0;
   if(speaking){speaker_tail=true;listen_after=now_ms()+400;}
   if(state!=VOICE_STATE_LISTENING)hal_i2s_rx_discard_pending();
   hint=speaking?"小伴说完，就轮到你啦":"小伴正在准备回答";
   usleep(20000);continue;
  }
  if(speaker_tail&&!speaking){
   hal_i2s_rx_discard_pending();carry=0;
   if((int32_t)(now_ms()-listen_after)<0){usleep(20000);continue;}
   speaker_tail=false;box_phrase_reset();
  }
  bool awake=hf_is_awake(&gate,now_ms());
  box_phrase_command_mode(awake&&!speaking);
  atomic_store(&power_interacting,awake&&!speaking);
  uint32_t playback_token=speaking?voice_service_speaking_token():0;
  uint32_t speech_token=awake?playback_token:0;
  hf_interrupt_set_turn(speech_token);
  hint=speaking?"可以说停一下":awake?"我在听，直接说吧":"说你好，小伴";
  hearing_speech=false;
  struct capture capture={.pcm=pcm,.count=carry,.prefix=carry,
      .capacity=16000*12,.monitor=!awake||speaking,.allow_stop=speaking,.wake_keywords=awake&&!speaking,
      .voice_started=false,.speech_token=speech_token,.local_keywords=local_playback_recognizer&&awake&&box_aec_ready()};carry=0;
  if((capture.monitor||capture.wake_keywords)&&(!speaking||capture.local_keywords)){
   ret=box_phrase_stream_begin();
   if(ret<0){last_error=ret;errors++;usleep(250000);continue;}
  }
  if((capture.monitor||capture.wake_keywords)&&!speaking&&capture.count){
   int step=hf_audio_gate_update(&capture.voice_started,&capture.quiet_samples,pcm,capture.count,false);
   if(step&HF_AUDIO_BEGIN){hearing_speech=true;capture.local=box_phrase_stream_feed(pcm,capture.count,false);}
  }
  int seconds=hf_capture_seconds(capture.capacity,capture.count,awake);
  if(seconds<1){carry=0;continue;}
  capturing=true;
  ret=capture.monitor?voice_service_background_monitor(collect,&capture,seconds):
      voice_service_background_record(collect,&capture,seconds);capturing=false;recordings++;
  pthread_mutex_lock(&lock);bool interrupted=!enabled||!microphone_enabled||epoch!=listener_generation||injected[0]||announcement[0];pthread_mutex_unlock(&lock);
  if(interrupted||ret==-ECANCELED||ret==-EBUSY){carry=0;if(capture.monitor||capture.wake_keywords)box_phrase_reset();usleep(50000);continue;}
  if(capture.playback_done&&!capture.local){
   /* Buffered microphone audio still contains our final spoken words.
    * Start the next turn with fresh samples, not a replay of that tail. */
   box_phrase_reset();carry=0;hal_i2s_rx_discard_pending();
   speaker_tail=true;listen_after=now_ms()+400;continue;
  }
  /* Quiet VAD rejection must not discard the model's final partial frame.
   * A known real microphone miss is recognized when its complete recording
   * is replayed. Keep this bounded local second pass off the speaking path. */
  /* Ordinary wake frames were decoded while RX was being drained.
   * Bound only the optional trailing/phase rechecks against RX backlog. */
process_capture:
  box_phrase_rx_guard(capture.monitor&&!speaking&&!awake);
  int local=capture.local;
  /* Awake capture keeps its foreground VAD. Flush only the partial model
   * frame; do not add whole-clip/phase replays before ordinary commands. */
  if(capture.wake_keywords && local!=1 && capture.voice_started && (ret>=0||ret==-ENODATA))
   local=box_phrase_stream_end_live(0);
  if(capture.monitor && (!speaking||capture.local_keywords) && !local && capture.voice_started && (ret>=0||ret==-ENODATA))
   local=(hint="小伴正在想一想",box_phrase_finish_window(pcm,capture.count,capture.speech_start,speaking,!awake&&!speaking&&ret>=0&&hf_replay_has_speech(pcm,capture.count)));
  if(!awake&&!speaking&&!local&&capture.monitor&&capture.voice_started&&ret>=0&&
     !capture.overflow&&hf_replay_has_speech(pcm,capture.count))
   local=box_phrase_weak_candidate(pcm,capture.count,capture.speech_start);
  bool rx_yielded=box_phrase_rx_was_yielded();
  box_phrase_rx_guard(0);
  if(speaking && awake && !capture.local_keywords && !local && capture.voice_started && (ret>=0||ret==-ENODATA))offer_interrupt(&capture,true);
  /* Opt-in, one-shot maintenance capture. Normal use never writes speech.
   * Save only after recognition so file I/O cannot affect this decision. */
  if(!speaking&&(capture.monitor?capture.voice_started:(ret>=0&&capture.count>0))&&atomic_exchange(&trace_next_wake,false)){
   FILE *f=fopen("/data/hf_wake.pcm","wb");int error=f?0:-errno;
   if(f){if(fwrite(pcm,sizeof(*pcm),capture.count,f)!=capture.count)error=-EIO;
    if(fflush(f)!=0)error=-EIO;if(fclose(f)!=0)error=-EIO;}
   uint32_t hash=2166136261u;const uint8_t *bytes=(const uint8_t *)pcm;
   for(size_t i=0;i<capture.count*sizeof(*pcm);i++)hash=(hash^bytes[i])*16777619u;
   printf("[HFTrace] result=%d samples=%lu onset=%lu local=%d fnv=%08lx\n",error,(unsigned long)capture.count,(unsigned long)capture.speech_start,local,(unsigned long)hash);
  }
  if(!local && speech_token)local=hf_interrupt_take(speech_token);
  if(ret==-ENODATA&&!local){
   if(capture.monitor||capture.wake_keywords)box_phrase_reset();
   carry=hf_capture_keep_tail(pcm,capture.count,awake,capture.utterance_ended);
   usleep(20000);continue;
  }
  if((ret<0&&!(ret==-ENODATA&&local))||capture.overflow){if(capture.monitor||capture.wake_keywords)box_phrase_reset();last_error=ret;errors++;usleep(250000);continue;}
  pthread_mutex_lock(&lock);interrupted=!enabled||epoch!=listener_generation||injected[0]||announcement[0];pthread_mutex_unlock(&lock);
  if(interrupted)continue;
  if(speaking){
   if(local==2){if(echo_of_speaker("停一下")||echo_of_speaker("停下")||echo_of_speaker("停止")||echo_of_speaker("别说了"))continue;if(!voice_service_abort_playback(playback_token))continue;struct hf_command stop={HF_STOP,1,false,{0}};dispatch(&stop,NULL);hf_keep_awake(&gate,now_ms());continue;}
   if(local!=1||echo_of_speaker("你好小伴"))continue;
   if(!voice_service_abort_playback(playback_token))continue;
   for(int i=0;i<100&&voice_service_busy();i++)usleep(50000);
   if(voice_service_busy())continue;
   /* A fresh wake during playback starts a clean turn. Do not transcribe
    * a newer microphone window after an older queued wake was confirmed. */
   struct hf_command wake=hf_parse(&gate,"你好小伴",now_ms());dispatch(&wake,NULL);continue;
  }
  if(dispatch_local(local,awake,&capture)){carry=0;continue;}
  bool weak_wake=local==3;
  bool confirm_wake=local==1||weak_wake;
  if(!awake&&local!=1&&!weak_wake){
   carry=(capture.utterance_ended||rx_yielded)?0:(capture.count<32000?capture.count:32000);
   memmove(pcm,pcm+capture.count-carry,carry*2);
   continue; /* Upload only after a strong wake or a local weak candidate. */
  }
  /* A normal-threshold acoustic hit wakes locally. Cloud ASR understands
   * the following command; spelling or network errors cannot veto wake. */
  if(local==1){
   sleeping=false;hf_keep_awake(&gate,now_ms());box_phrase_reset();
   box_power_activity();ai_voice_play_tone(880,100);
   printf("[HF] acoustic wake accepted; ASR is command-only\n");
  }
  if(!server_bridge_configured()){
   if(local==1){struct hf_command wake=hf_parse(&gate,"你好小伴",now_ms());dispatch(&wake,NULL);}
   else if(!confirm_wake)say("我在呢，网络恢复后就可以和我说话啦。");
   continue;
  }
  char transcript[512];hint="小伴在理解你的话";asr_requests++;
  asr_pending=true;
  if(awake&&!confirm_wake){
   struct asr_listener listener={.audio={.pcm=pcm,.capacity=16000*12,.monitor=true},.epoch=epoch};
   ret=server_bridge_transcribe_idle(pcm,capture.count,transcript,sizeof(transcript),asr_listen,&listener);
   bool new_audio=asr_listen_finish(&listener);
   asr_pending=false;
   box_phrase_reset();
   if(new_audio){
    bool new_wake=listener.audio.local==1;
    printf(new_wake?"[HF] acoustic wake during ASR wait; discard previous result\n":
           "[HF] new command during ASR wait; discard previous result\n");
    capture=listener.audio;capture.monitor=false;capture.wake_keywords=new_wake;ret=0;
    hf_keep_awake(&gate,now_ms());
    goto process_capture;
   }
   if(listener.error){last_error=listener.error;errors++;continue;}
  }else ret=server_bridge_transcribe(pcm,capture.count,transcript,sizeof(transcript));
  asr_pending=false;
  if(ret<0){
   last_error=ret;errors++;
   pthread_mutex_lock(&lock);interrupted=!enabled||epoch!=listener_generation||injected[0]||announcement[0];pthread_mutex_unlock(&lock);
   if(interrupted||voice_service_busy())continue;
   if(local==1){struct hf_command wake=hf_parse(&gate,"你好小伴",now_ms());dispatch(&wake,NULL);}
   else if(!confirm_wake)say("刚才没听清，可以再说一次吗？");
   continue;
  }
  pthread_mutex_lock(&lock);
  snprintf(text_buffers->heard,sizeof(text_buffers->heard),"%s",transcript);
  for(char *p=text_buffers->heard;*p;p++)if((unsigned char)*p<32)*p=' ';
  pthread_mutex_unlock(&lock);
  pthread_mutex_lock(&lock);interrupted=!enabled||epoch!=listener_generation||injected[0]||announcement[0];pthread_mutex_unlock(&lock);
  if(interrupted||voice_service_busy())continue;
  if(speaking&&echo_of_speaker(transcript))continue;
  if(weak_wake){
   if(!hf_weak_wake_text_matches(transcript)){
    printf("[HF] weak wake not confirmed; remain asleep\n");continue;
   }
   sleeping=false;hf_keep_awake(&gate,now_ms());box_phrase_reset();
   printf("[HF] weak wake confirmed by tolerant phonetics\n");
   ai_voice_play_tone(880,100);
  }
  if(local==1||weak_wake)hf_canonicalize_local_wake(transcript,sizeof(transcript));
  struct hf_command c=hf_parse(&gate,transcript,now_ms());
  if(local==1||weak_wake)c.woke=true;
  capture.transcript=transcript;dispatch(&c,&capture);
  usleep(150000);
 }
 return NULL;
}
int handsfree_service_start(void)
{
 pthread_mutex_lock(&lock);if(started){pthread_mutex_unlock(&lock);return 0;}
 text_buffers=calloc(1,sizeof(*text_buffers));if(!text_buffers){pthread_mutex_unlock(&lock);return -ENOMEM;}
 started=true;pthread_mutex_unlock(&lock);
 pthread_t tid;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,12288);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
 int ret=pthread_create(&tid,&attr,worker,NULL);pthread_attr_destroy(&attr);return ret?-ret:0;
}
void handsfree_service_enable(int on)
{pthread_mutex_lock(&lock);listener_generation++;enabled=on!=0;if(!enabled)hearing_speech=false;if(!enabled){gate.awake=false;injected[0]=0;announcement[0]=0;}pthread_mutex_unlock(&lock);ai_voice_background_finish();if(on)hal_i2s_rx_discard_pending();}
int handsfree_service_enabled(void){return enabled;}
const char *handsfree_service_hint(void){return hint;}
int handsfree_service_inject(const char *text)
{
 if(!text||!text_buffers||strlen(text)>=sizeof(injected))return -EINVAL;
 if(!enabled)return -EAGAIN;
 pthread_mutex_lock(&lock);if(injected[0]){pthread_mutex_unlock(&lock);return -EBUSY;}
 snprintf(injected,sizeof(injected),"%s",text);pthread_mutex_unlock(&lock);ai_voice_background_finish();return 0;
}
void handsfree_service_announce(const char *text)
{
 if(!text||sleeping)return;pthread_mutex_lock(&lock);snprintf(announcement,sizeof(announcement),"%s",text);pthread_mutex_unlock(&lock);ai_voice_background_finish();
}
void handsfree_service_status(void)
{
 printf("[HF] enabled=%d ready=%d awake=%d recordings=%u asr=%u wakes=%u actions=%u errors=%u last=%s error=%d capturing=%d barge_enabled=%d mic_enabled=%d asr_pending=%d\n",enabled,ready,hf_is_awake(&gate,now_ms()),recordings,asr_requests,wakes,actions,errors,hf_action_name(last_action),last_error,capturing,playback_interrupts,microphone_enabled,asr_pending);
 box_phrase_status();
 printf("[HFLocal] commands=%u offline_commands=1\n",local_commands);
 box_aec_dump();
 hf_interrupt_queue_status();
}
/* Explicit maintenance readback only. Keep the last ASR result in RAM;
 * do not print or persist children's speech during ordinary operation. */
void handsfree_service_dump_heard(void)
{
 char text[512]={0};pthread_mutex_lock(&lock);
 if(text_buffers)snprintf(text,sizeof(text),"%s",text_buffers->heard);
 pthread_mutex_unlock(&lock);printf("[HF] heard=%s\n",text);
}
void handsfree_service_trace_wake(int once)
{atomic_store(&trace_next_wake,once!=0);printf("[HFTrace] armed=%d\n",once!=0);}
/* Maintenance only, never persisted; normal startup always enables mic. */
void handsfree_service_screen_idle(void)
{
 pthread_mutex_lock(&lock);listener_generation++;screen_idle_requested=true;pthread_mutex_unlock(&lock);
 /* Cancel only background listening, never the current spoken acknowledgement. */
 ai_voice_background_finish();
}
void handsfree_service_microphone(int enabled)
{pthread_mutex_lock(&lock);listener_generation++;microphone_enabled=enabled!=0;if(!enabled){hearing_speech=false;power_interacting=false;}pthread_mutex_unlock(&lock);ai_voice_background_finish();}
