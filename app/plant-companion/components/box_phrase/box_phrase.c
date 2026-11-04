#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
static pthread_mutex_t model_lock=PTHREAD_MUTEX_INITIALIZER;
#include "box_phrase.h"
#include "box_local_commands.h"
#include "vendor/esp_mn_iface.h"
#include "vendor/esp_mn_speech_commands.h"
#include "vendor/puff.h"
extern const unsigned char box_phrase_blob[];
extern const size_t box_phrase_blob_size;
extern const esp_mn_iface_t esp_sr_multinet5_quantized8;
extern void *srmodel_load(const void *root);
extern void xtensa_coproc_enable(int cpset);
static const esp_mn_iface_t *mn=&esp_sr_multinet5_quantized8;
static model_iface_data_t *model;
static void *raw;
static int chunk,last_id,last_probability;
static unsigned calls,hits,failures,last_ms;
static int16_t *stream_frame;
static size_t stream_fill;
static int stream_hit;
static bool stream_active;
static bool command_mode,stream_pending,stream_blocked;
static int command_probability;
static clock_t stream_started;
static uint64_t detect_cycles;
static unsigned detect_frames;
/* Normal sound recognition can wake without a cloud-text lock. Keep its
 * original threshold; threshold-20 fallback stays an ASR-checked weak hit. */
static int threshold_percent=40;
static bool rx_guard,rx_yielded;
static unsigned rx_yields;
extern uint32_t hal_i2s_rx_pending_samples(void);
extern void hal_i2s_rx_discard_pending(void);
/* RX holds 65536 mono samples at 24kHz. Return to draining before it fills.
 * Offline file tests retain their complete multi-phase coverage. */
static bool phrase_rx_should_yield(void)
{
 if(!rx_guard)return false;
 if(rx_yielded)return true;
 uint32_t pending=hal_i2s_rx_pending_samples();
 if(pending<48000u)return false;
 rx_yielded=true;rx_yields++;last_id=-EAGAIN;
 printf("[HFWake] RX priority yield pending=%lu\n",(unsigned long)pending);
 return true;
}
void box_phrase_rx_guard(int enable)
{
 pthread_mutex_lock(&model_lock);
 rx_guard=enable!=0;rx_yielded=false;
 pthread_mutex_unlock(&model_lock);
}
int box_phrase_rx_was_yielded(void)
{pthread_mutex_lock(&model_lock);int result=rx_yielded;pthread_mutex_unlock(&model_lock);return result;}
static uint32_t phrase_cycles(void)
{
#ifdef __XTENSA__
 uint32_t value;__asm__ __volatile__("rsr %0, ccount":"=a"(value));return value;
#else
 return 0;
#endif
}
esp_mn_iface_t *esp_mn_handle_from_name(char *name)
{return name&&!strcmp(name,"mn5q8_cn")?(esp_mn_iface_t *)mn:NULL;}
char *esp_mn_language_from_name(char *name){(void)name;return "cn";}
static uint32_t u32(const unsigned char *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t crc32(const unsigned char *p,size_t count)
{uint32_t crc=~0u;while(count--){crc^=*p++;for(int i=0;i<8;i++)crc=(crc>>1)^(0xedb88320u&-(crc&1));}return ~crc;}
static int phrase_grammar_unlocked(bool commands)
{
 struct {int id;const char *phrase;} words[]={
  {1,"ni hao xiao ban"},{2,"ting yi xia"},{2,"ting xia"},{2,"ting zhi"},{2,"bie shuo le"},
  {20,"ni hao xiao zhi"},{21,"ha lou xiao ban"},{22,"ni hao lao ban"},
  {23,"ni hao xiao wang"},{25,"wo xiang ting gu shi"},
  {26,"bu yao pai zhao"},{27,"kan kan shu ju"},{28,"jin tian tian qi zen me yang"}};
 int ret=esp_mn_commands_clear();
 for(size_t i=0;!ret&&i<sizeof(words)/sizeof(words[0]);i++){
  if(commands&&(words[i].id==26||words[i].id==27))continue;
  ret=esp_mn_commands_add(words[i].id,words[i].phrase);
 }
 if(commands)for(size_t i=0;!ret&&i<sizeof(box_local_phrases)/sizeof(box_local_phrases[0]);i++)
  ret=esp_mn_commands_add(box_local_phrases[i].id,box_local_phrases[i].phonemes);
 if(!ret&&esp_mn_commands_update())ret=-EINVAL;
 return ret;
}
static bool phrase_ambiguous(const esp_mn_results_t *r)
{
 return command_mode&&r&&r->num>1&&box_local_command_text(r->command_id[0])&&
  r->command_id[0]!=r->command_id[1]&&r->prob[0]-r->prob[1]<0.10f;
}
static int phrase_accept(const esp_mn_results_t *r,int allow_stop)
{
 if(!r||r->num<1)return 0;
 int id=r->command_id[0];
 if(id==1)return 1; /* Keep native wake acceptance, including 397/1000. */
 if(id==2&&(allow_stop||command_mode))return 2;
 /* Physical microphone capture of a correct shutter command ranks 449;
  * an extra 60% filter discarded it after native detection had passed.
  * Keep the native 40% gate with ambiguity/negation checks. Readouts
  * tolerate the native ranking edge (386 observed) as wake already does. */
 float minimum=id>=43&&id<=46?0.35f:0.40f;
 if(command_mode&&box_local_command_text(id)&&r->prob[0]>=minimum&&!phrase_ambiguous(r))return id;
 return 0;
}
int box_phrase_command_mode(int awake)
{
 pthread_mutex_lock(&model_lock);int ret=0;bool commands=awake!=0;
 if(!model)ret=-EAGAIN;
 else if(command_mode!=commands){
  mn->clean(model);stream_active=false;stream_fill=0;
  /* Keep one resident grammar. Rebuilding the vendor language model on
   * every wake/sleep cycle loses heap on this SDK. Only the action gate
   * changes; the original acoustic wake threshold stays unchanged. */
  command_mode=commands;
 }
 pthread_mutex_unlock(&model_lock);return ret;
}
const char *box_phrase_command_text(int id){return box_local_command_text(id);}
int box_phrase_command_probability(void){return command_probability;}
static int phrase_init_unlocked(void)
{
 if(model)return 0;
 xtensa_coproc_enable(1<<3);
 const unsigned char *blob=box_phrase_blob;
 if(box_phrase_blob_size<32||memcmp(blob,"PBWAKE01",8))return -EINVAL;
 unsigned long packed=u32(blob+8),length=u32(blob+12);
 if(packed>box_phrase_blob_size-32||length!=2203819)return -EINVAL;
 int ret;
 if(!raw){
 raw=malloc(length);if(!raw)return -ENOMEM;
 ret=puff(raw,&length,blob+32,&packed);
 if(ret||crc32(raw,length)!=u32(blob+16)){free(raw);raw=NULL;return -EIO;}
 if(!srmodel_load(raw))return -EINVAL;
 }
 model=mn->create("mn5q8_cn",6000);if(!model)return -ENOMEM;
 mn->set_det_threshold(model,threshold_percent/100.0f);
 ret=phrase_grammar_unlocked(true);command_mode=false;
 chunk=mn->get_samp_chunksize(model);
 if(ret||mn->get_samp_rate(model)!=16000||chunk<=0||chunk>1024){mn->destroy(model);model=NULL;return -EINVAL;}
 printf("[HFWake] local ready phrase=你好小伴 frame=%d CRC=%08lx\n",chunk,(unsigned long)u32(blob+16));
 printf("[HFLocal] resident grammar=1 awake_only=1\n");
 return 0;
}
static int phrase_detect_unlocked(const int16_t *pcm,size_t samples,int allow_stop)
{
 if(!model||!pcm||!samples)return 0;
 xtensa_coproc_enable(1<<3);
 int16_t *frame=memalign(16,chunk*2);if(!frame){failures++;return 0;}
 mn->clean(model);clock_t start=clock_systime_ticks();int accepted=0;
 calls++;last_id=0;last_probability=0;command_probability=0;
 for(size_t offset=0;offset<samples+16000;offset+=(size_t)chunk){
  if(phrase_rx_should_yield())break;
  memset(frame,0,chunk*2);
  if(offset<samples){size_t n=samples-offset;if(n>(size_t)chunk)n=chunk;memcpy(frame,pcm+offset,n*2);}
  esp_mn_state_t state=mn->detect(model,frame);
  if(state==ESP_MN_STATE_TIMEOUT)mn->clean(model);
  if(state==ESP_MN_STATE_DETECTED){
   esp_mn_results_t *r=mn->get_results(model);
   if(r&&r->num>0){
    last_id=r->command_id[0];last_probability=(int)(r->prob[0]*1000);
    if(command_mode&&(box_local_command_blocked(last_id)||phrase_ambiguous(r))){accepted=0;break;}
    int found=phrase_accept(r,allow_stop);
    if(found){accepted=found;command_probability=last_probability;if(found==1||(found==2&&allow_stop))break;}
   }
   mn->clean(model);
  }
 }
 last_ms=(unsigned)((clock_systime_ticks()-start)*1000/TICK_PER_SEC);
 mn->clean(model);free(frame);if(accepted)hits++;
 printf("[HFWake] candidate=%d probability=%d/1000 accepted=%d elapsed=%u ms\n",last_id,last_probability,accepted,last_ms);
 return accepted;
}
int box_phrase_detect(const int16_t *pcm,size_t samples,int allow_stop)
{
 pthread_mutex_lock(&model_lock);
 int found=phrase_detect_unlocked(pcm,samples,allow_stop);
 pthread_mutex_unlock(&model_lock);return found;
}
/* This is only a local preselection, never permission to wake. The caller
 * must obtain tolerant phonetic confirmation from ASR before tone/UI.
 * Preserve normal threshold and grammar, and do not recheck a known negative.
 * One unchanged view only; no gain, time stretch, or extra PCM allocation. */
int box_phrase_weak_candidate(const int16_t *pcm,size_t samples,size_t onset)
{
 pthread_mutex_lock(&model_lock);
 int found=0;
 if(model&&pcm&&onset<samples&&samples-onset>=3200&&last_id==0&&
    threshold_percent>20&&!phrase_rx_should_yield()){
  mn->set_det_threshold(model,0.20f);
  /* Preserve the model-frame grid without rescanning seconds of old quiet. */
  size_t begin=chunk>0?onset/(size_t)chunk*(size_t)chunk:0;
  found=phrase_detect_unlocked(pcm+begin,samples-begin,0);
  mn->set_det_threshold(model,threshold_percent/100.0f);mn->clean(model);
  printf("[HFWake] weak candidate=%d ASR confirmation required\n",found==1);
 }
 pthread_mutex_unlock(&model_lock);return found==1?3:0;
}
int box_phrase_stream_begin(void)
{
 pthread_mutex_lock(&model_lock);
 if(!model){pthread_mutex_unlock(&model_lock);return -EAGAIN;}
 if(!stream_frame)stream_frame=memalign(16,chunk*2);
 if(!stream_frame){failures++;pthread_mutex_unlock(&model_lock);return -ENOMEM;}
 xtensa_coproc_enable(1<<3);mn->clean(model);
 stream_fill=0;stream_hit=0;stream_pending=false;stream_blocked=false;
 stream_active=true;stream_started=clock_systime_ticks();
 calls++;last_id=0;last_probability=0;command_probability=0;
 pthread_mutex_unlock(&model_lock);return 0;
}
/* DETECTED already passed the configured model threshold. Its reported
 * result-ranking probability can be slightly below that threshold (397/1000
 * observed on hardware); filtering it a second time loses a valid wake. */
static void stream_frame_unlocked(int allow_stop)
{
 uint32_t began=phrase_cycles();
 esp_mn_state_t state=mn->detect(model,stream_frame);
 detect_cycles+=(uint32_t)(phrase_cycles()-began);detect_frames++;
 if(state==ESP_MN_STATE_TIMEOUT)mn->clean(model);
 if(state==ESP_MN_STATE_DETECTED){
  esp_mn_results_t *r=mn->get_results(model);
  if(r&&r->num>0){
   last_id=r->command_id[0];last_probability=(int)(r->prob[0]*1000);
   if(command_mode&&(box_local_command_blocked(last_id)||phrase_ambiguous(r))){
    stream_hit=0;stream_pending=false;stream_blocked=true;
   }
   int found=stream_blocked?0:phrase_accept(r,allow_stop);
   if(command_mode)printf("[HFLocal] candidate=%d probability=%d accepted=%d blocked=%d\n",last_id,last_probability,found,stream_blocked);
   if(found){
    stream_hit=found;stream_pending=found!=1&&!(found==2&&allow_stop);
    command_probability=last_probability;if(!stream_pending)hits++;
    last_ms=(unsigned)((clock_systime_ticks()-stream_started)*1000/TICK_PER_SEC);
    printf("[HFWake] stream candidate=%d probability=%d/1000 accepted=%d elapsed=%u ms\n",last_id,last_probability,stream_hit,last_ms);
   }
  }
  if(!stream_hit||stream_pending)mn->clean(model);
 }
 stream_fill=0;
}
int box_phrase_stream_feed(const int16_t *pcm,size_t samples,int allow_stop)
{
 pthread_mutex_lock(&model_lock);
 if(!stream_active||!model||(!pcm&&samples)){pthread_mutex_unlock(&model_lock);return 0;}
 xtensa_coproc_enable(1<<3);
 while(samples&&(!stream_hit||stream_pending)&&!stream_blocked){
  if(phrase_rx_should_yield())break;
  size_t n=(size_t)chunk-stream_fill;if(n>samples)n=samples;
  memcpy(stream_frame+stream_fill,pcm,n*2);stream_fill+=n;pcm+=n;samples-=n;
  if(stream_fill==(size_t)chunk)stream_frame_unlocked(allow_stop);
 }
 int result=stream_hit;pthread_mutex_unlock(&model_lock);return result;
}
static int stream_end_impl(int allow_stop,bool live)
{
 pthread_mutex_lock(&model_lock);
 if(!stream_active||!model){pthread_mutex_unlock(&model_lock);return 0;}
 xtensa_coproc_enable(1<<3);
 /* Supply the same bounded trailing silence used by the file decoder. */
 /* Live playback must return to RX immediately. The file/ordinary-wake
  * decoder retains its original trailing-silence context. */
 size_t trailing=live?(stream_fill?(size_t)chunk-stream_fill:0):16000;
 for(size_t n=0;n<trailing&&(!stream_hit||stream_pending)&&!stream_blocked;n+=(size_t)chunk){
  if(phrase_rx_should_yield())break;
  memset(stream_frame+stream_fill,0,((size_t)chunk-stream_fill)*2);
  stream_frame_unlocked(allow_stop);
 }
 int result=stream_hit;stream_active=false;stream_fill=0;mn->clean(model);
 pthread_mutex_unlock(&model_lock);return result;
}
int box_phrase_stream_end(int allow_stop)
{return stream_end_impl(allow_stop,false);}
int box_phrase_stream_end_live(int allow_stop)
{return stream_end_impl(allow_stop,true);}
/* A live onset gate can choose a less favorable frame boundary than the
 * complete captured utterance. Recheck a missed wake locally, with the same
 * threshold and grammar. Never replay while a speaker needs continuous RX. */
int box_phrase_finish_window(const int16_t *pcm,size_t samples,size_t onset,int allow_stop,int replay_miss)
{
 int found=stream_end_impl(allow_stop,allow_stop!=0);
 if(!found && replay_miss && !allow_stop && pcm && onset<samples && chunk>0&&
    !phrase_rx_should_yield()){
  /* VAD's onset is not a model-frame boundary. Replaying from that same
   * offset repeats a phase-sensitive miss. Align to the original buffer's
   * frame grid, retaining at most one extra frame of quiet context.
   * Real failed clip: onset42116 failed; aligned41984 passed at threshold40. */
  size_t begin=onset/(size_t)chunk*(size_t)chunk;
  found=begin==onset?0:box_phrase_detect(pcm+begin,samples-begin,0);
  printf("[HFWake] complete utterance recheck result=%d samples=%lu onset=%lu replay_start=%lu\n",found,(unsigned long)(samples-begin),(unsigned long)onset,(unsigned long)begin);
  /* A second actual microphone miss was sensitive to both frame phase
   * and leading quiet context: 98196/46124, start46080 failed, start46464
   * passed at the same threshold. Only recheck an unmatched candidate,
   * with existing pre-roll, and never while playing audio. */
  /* Actual cold-boot clip 160000/110188 also misses at phase 0 and
   * 3/4 but passes at phase 1/4 (start110208). Keep that bounded third
   * view at the same threshold. Stop as soon as any known negative word
   * is detected; never create more views during speaker playback. */
  const unsigned phases[]={3,1};
  for(unsigned i=0;!found&&last_id==0&&i<2&&!phrase_rx_should_yield();i++){
   size_t shift=(size_t)chunk*phases[i]/4;
   if(!shift||samples-begin<=shift)continue;
   size_t alternate=begin+shift;
   found=box_phrase_detect(pcm+alternate,samples-alternate,0);
   printf("[HFWake] alternate phase=%u/4 recheck result=%d samples=%lu onset=%lu replay_start=%lu\n",phases[i],found,(unsigned long)(samples-alternate),(unsigned long)onset,(unsigned long)alternate);
  }
 }
 return found;
}
int box_phrase_finish_utterance(const int16_t *pcm,size_t samples,int allow_stop,int replay_miss)
{return box_phrase_finish_window(pcm,samples,0,allow_stop,replay_miss);}
void box_phrase_status(void)
{printf("[HFWake] ready=%d local=1 phrase=你好小伴 calls=%u hits=%u errors=%u last_id=%d probability=%d elapsed_ms=%u\n",model!=NULL,calls,hits,failures,last_id,last_probability,last_ms);
 printf("[HFWake] threshold_percent=%d rx_yields=%u\n",threshold_percent,rx_yields);
 printf("[HFWake] detect_mean_us=%lu frames=%u\n",(unsigned long)(detect_cycles/240/(detect_frames?detect_frames:1)),detect_frames);}
int box_phrase_test_file_at(const char *path,size_t onset)
{
 if(box_phrase_init()<0)return -EAGAIN;
 FILE *f=fopen(path,"rb");if(!f)return -errno;
 int16_t *pcm=malloc(16000*12*2);if(!pcm){fclose(f);return -ENOMEM;}
 /* Bound each FAT/SD read instead of requesting one large DMA transfer. */
 size_t n=0;
 while(n<16000*12){size_t block=16000*12-n;if(block>512)block=512;
  size_t got=fread(pcm+n,2,block,f);n+=got;if(got<block)break;
 }
 int bad=ferror(f);fclose(f);
 if(!n||bad){free(pcm);printf("[HFWakeTest] input read failed\n");return -EIO;}
 if(onset>=n){free(pcm);return -EINVAL;}
 uint32_t hash=2166136261u;const uint8_t *bytes=(const uint8_t *)pcm;
 for(size_t i=0;i<n*sizeof(*pcm);i++)hash=(hash^bytes[i])*16777619u;
 printf("[HFWakeTest] input_fnv=%08lx samples=%lu onset=%lu\n",(unsigned long)hash,(unsigned long)n,(unsigned long)onset);
 int result=box_phrase_detect(pcm,n,1),stream=0;
 if(box_phrase_stream_begin()==0){
  for(size_t offset=0;offset<n&&!stream;offset+=2004){size_t count=n-offset;if(count>2004)count=2004;stream=box_phrase_stream_feed(pcm+offset,count,1);}
  stream=box_phrase_stream_end(1);
 }
 int wake=0;
 if(box_phrase_stream_begin()==0){
  for(size_t offset=onset;offset<n&&!wake;offset+=2004){size_t count=n-offset;if(count>2004)count=2004;wake=box_phrase_stream_feed(pcm+offset,count,0);}
  printf("[HFWakeTest] gated_stream_before_finish=%d onset=%lu\n",wake,(unsigned long)onset);
  wake=box_phrase_finish_window(pcm,n,onset,0,1);
 }
 printf("[HFWakeTest] wake_pipeline_result=%d samples=%lu\n",wake,(unsigned long)n);
 int weak=wake?0:box_phrase_weak_candidate(pcm,n,onset);
 printf("[HFWakeTest] weak_candidate_result=%d samples=%lu\n",weak,(unsigned long)n);
 free(pcm);
 printf("[HFWakeTest] stream_result=%d samples=%lu\n",stream,(unsigned long)n);
 printf("[HFWakeTest] result=%d samples=%lu\n",result,(unsigned long)n);return result;
}
int box_phrase_test_file(const char *path){return box_phrase_test_file_at(path,0);}
int box_phrase_test_command_file(const char *path)
{
 FILE *f=fopen(path,"rb");if(!f)return -errno;
 int ret=box_phrase_command_mode(1);if(ret<0){fclose(f);return ret;}
 ret=box_phrase_stream_begin();int16_t frame[512];size_t n,total=0;
 while(ret>=0&&(n=fread(frame,2,512,f))>0){
  total+=n;box_phrase_stream_feed(frame,n,0);
 }
 if(ferror(f))ret=-EIO;
 fclose(f);
 int id=ret<0?ret:box_phrase_stream_end(0);
 printf("[HFLocalTest] id=%d probability=%d samples=%lu text=%s\n",id,
  box_phrase_command_probability(),(unsigned long)total,box_phrase_command_text(id)?box_phrase_command_text(id):"-");
 box_phrase_reset();box_phrase_command_mode(0);return id<0?id:0;
}

int box_phrase_init(void)
{pthread_mutex_lock(&model_lock);int ret=phrase_init_unlocked();pthread_mutex_unlock(&model_lock);return ret;}
void box_phrase_reset(void)
{pthread_mutex_lock(&model_lock);if(model)mn->clean(model);stream_active=false;stream_fill=0;stream_pending=stream_blocked=false;pthread_mutex_unlock(&model_lock);}
int box_phrase_ready(void){return model!=NULL;}
int box_phrase_set_threshold(int percent)
{
 if(percent<20||percent>60)return -EINVAL;
 pthread_mutex_lock(&model_lock);threshold_percent=percent;
 if(model){mn->set_det_threshold(model,percent/100.0f);mn->clean(model);}
 pthread_mutex_unlock(&model_lock);
 printf("[HFWake] threshold_percent=%d\n",percent);return 0;
}
