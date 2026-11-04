/* Local Chinese phrase recognizer. MultiNet is evaluated here as a
 * Energy-gated MultiNet phrase detection, not a custom-trained WakeNet.
 * Microphone audio stays on the device until an ordinary voice session.
 */
#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <sched.h>
#include <nuttx/mtd/mtd.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "voice_wake.h"
#include "../camera_capture/camera_capture.h"
#include "vendor/esp_mn_iface.h"
#include "vendor/esp_mn_speech_commands.h"
#include "vendor/puff.h"
#include "../../ai_module/ai_voice/ai_voice.h"
#include "../../hal/hal_i2s.h"
#include "../../services/voice_service.h"

#define MODEL_OFFSET 0xe10000u
#define MODEL_REGION 0x1f0000u
#define WAKE_PREROLL_FRAMES 24u /* 768 ms: same-SDK RAM replay recovered missed soft onset. */
#define WAKE_DECODER_TAIL_FRAMES 32u /* 1024 ms permits a natural comma pause. */
extern struct mtd_dev_s *esp32s3_spiflash_mtd(void);
extern void *xtensa_imm_malloc(size_t size);
extern void xtensa_imm_free(void *p);
extern void *srmodel_load(const void *root);
extern void xtensa_coproc_enable(int cpset);
extern const esp_mn_iface_t esp_sr_multinet5_quantized8;
/* Limit the linked implementation to the model actually shipped. */
esp_mn_iface_t *esp_mn_handle_from_name(char *name)
{
  return name && !strcmp(name,"mn5q8_cn") ?
    (esp_mn_iface_t *)&esp_sr_multinet5_quantized8 : NULL;
}
char *esp_mn_language_from_name(char *name) { (void)name;return "cn"; }

static _Atomic unsigned g_lag_ms, g_resyncs, g_skipped;
static int g_infer_priority = 95;
static unsigned g_preview_infer_batch;
static _Atomic bool g_started, g_running, g_pause;
static _Atomic bool g_release_requested, g_release_done;
static _Atomic bool g_enabled = true;
static _Atomic bool g_quiet = true, g_event, g_ready;
static _Atomic unsigned g_threshold=20;
static _Atomic unsigned g_inferred, g_audio_drops, g_energy;
static _Atomic unsigned g_frames, g_hits, g_errors, g_last_us;
static _Atomic unsigned g_local_cmd, g_local_applied; /* requested bit0, reset bit1 */
static void *g_blob;
static const esp_mn_iface_t *g_mn = &esp_sr_multinet5_quantized8;
static model_iface_data_t *g_model;
static pthread_mutex_t g_start_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint32_t crc32(const uint8_t *p,size_t n)
{
  uint32_t c=~0u;
  while(n--) { c^=*p++; for(int j=0;j<8;j++)c=(c>>1)^(0xedb88320u&-(c&1)); }
  return ~c;
}
static int flash_read(uint32_t offset,uint8_t *out,size_t n)
{
  struct mtd_dev_s *mtd=esp32s3_spiflash_mtd();
  uint8_t *tmp=xtensa_imm_malloc(4096);
  if(!mtd || !tmp) { if(tmp)xtensa_imm_free(tmp);return -ENOMEM; }
  int ret=0;
  while(n)
    {
      size_t part=n>4096?4096:n;
      if(MTD_READ(mtd,offset,part,tmp)!=(ssize_t)part){ret=-EIO;break;}
      memcpy(out,tmp,part);out+=part;offset+=part;n-=part;
    }
  xtensa_imm_free(tmp);return ret;
}
static int load_blob(void)
{
  uint8_t head[32];
  int ret=flash_read(MODEL_OFFSET,head,sizeof(head));
  if(ret<0)return ret;
  if(memcmp(head,"PBWAKE01",8)||u32(head+20)!=1)return -ENODATA;
  unsigned long packed_len=u32(head+8),raw_len=u32(head+12);
  if(packed_len>MODEL_REGION-32 || raw_len!=2203819)return -EINVAL;
  uint8_t *packed=malloc(packed_len),*raw=malloc(raw_len);
  if(!packed||!raw){free(packed);free(raw);return -ENOMEM;}
  ret=flash_read(MODEL_OFFSET+32,packed,packed_len);
  if(!ret)ret=puff(raw,&raw_len,packed,&packed_len);
  free(packed);
  if(ret || raw_len!=u32(head+12)||crc32(raw,raw_len)!=u32(head+16))
    {free(raw);return ret<0?ret:-EIO;}
  /* Container indices/names come from the CRC-checked, pinned model bundle. */
  g_blob=raw;
  if(!srmodel_load(raw)) {free(raw);g_blob=NULL;return -EINVAL;}
  printf("[Wake] model blob CRC verified, %lu bytes in PSRAM\n",raw_len);
  return 0;
}
struct local_phrase {int id;const char *pinyin;};
static int configure_grammar(unsigned mode)
{
  static const struct local_phrase normal[]={
    {1,"ha lou xiao ban"},{2,"ha lou xiao wang"},{3,"ha lou xiao ai"},{4,"ni hao xiao ban"}};
  static const struct local_phrase commands[]={
    {10,"da kai xiang ji"},{10,"kai qi xiang ji"},{10,"neng bang wo da kai xiang ji ma"},
    {11,"pai zhao"},{11,"pai yi zhang zhao"},{11,"pai ge zhao"},
    {12,"kan kan zhe shi shen me"},{12,"zhe shi shen me"},
    {13,"guan bi xiang ji"},{14,"xiang zuo zhuan"},{14,"qing wang zuo zhuan yi xia"},
    {15,"xiang you zhuan"},{16,"tai tou"},{17,"di tou"},{18,"tan suo yi xia"},{19,"hui zheng"},
    {100,"bu yao pai zhao"},{100,"bie pai zhao"},
    {101,"bu yao da kai xiang ji"},{101,"bu yao kai xiang ji"},
    {102,"bu yao tan suo"},{102,"ting zhi tan suo"},{103,"bu yao zhuan tou"},{103,"bu yao xiang zuo zhuan"},
    {104,"bu yao shi bie"},{200,"ni hao"},{201,"ni shi shui"},{202,"jin tian tian qi zen me yang"},
    {203,"wo xiang ting gu shi"},{204,"wo bu kai xin"},{205,"bu yao"},{206,"bu"},{207,"wu"},
    {208,"hao"},{209,"ha lou xiao ban"},{210,"wei shen me yao pai zhao"},
    {211,"gu shi li you yi ge xiang ji"},{212,"wo gang cai pai le yi zhang zhao"}};
  const struct local_phrase *phrases=mode?commands:normal;
  unsigned count=mode?sizeof(commands)/sizeof(commands[0]):sizeof(normal)/sizeof(normal[0]);
  g_mn->clean(g_model);
  int rc=esp_mn_commands_clear();
  for(unsigned i=0;!rc && i<count;i++)rc=esp_mn_commands_add(phrases[i].id,phrases[i].pinyin);
  if(!rc && esp_mn_commands_update())rc=-EINVAL;
  if(rc)return rc;
  g_mn->set_det_threshold(g_model,g_threshold/100.0f);
  g_local_applied=mode;
  printf("[LocalCmd] grammar=%u phrases=%u dispatch=0\n",mode,count);
  return 0;
}

/* Both the ESP-SR binaries and NuttX Xtensa use the 68-byte windowed
 * jmp_buf layout. The implementation must issue SYS_flush_context (3),
 * never newlib's bare SYS 0, which NuttX treats as save-context-to-A3. */
#include <setjmp.h>
_Static_assert(sizeof(jmp_buf) == 68, "ESP-SR/NuttX jmp_buf ABI mismatch");
static __attribute__((noinline)) void wake_context_jump(jmp_buf env, unsigned depth)
{
  volatile uint32_t keep[8];
  keep[0] = depth;
  if (depth) wake_context_jump(env, depth - 1);
  else longjmp(env, 7);
  keep[0]++;
}
static int wake_context_abi_test(void)
{
  static bool passed;
  if (passed) return 0;
  struct { volatile uint32_t pre[4]; jmp_buf env; volatile uint32_t post[4]; } check;
  for (volatile unsigned round = 0; round < 16; ++round)
    {
      for (unsigned j = 0; j < 4; ++j)
        { check.pre[j] = 0x51ab9c27u; check.post[j] = 0xa3c45e19u; }
      int code = setjmp(check.env);
      if (code == 0) wake_context_jump(check.env, round % 8);
      if (code != 7) return -EFAULT;
      for (unsigned j = 0; j < 4; ++j)
        if (check.pre[j] != 0x51ab9c27u || check.post[j] != 0xa3c45e19u)
          return -EFAULT;
    }
  passed = true;
  printf("[ContextABI] implementation=nuttx-Xtensa buffer_bytes=%u cases=16 canary=ok\n", (unsigned)sizeof(jmp_buf));
  return 0;
}

static int create_model(void)
{
  int ret;
  if((ret=wake_context_abi_test())<0)return ret;
  if(!g_blob && (ret=load_blob())<0)return ret;
  g_model=g_mn->create("mn5q8_cn",6000);
  if(!g_model)return -ENOMEM;
  g_mn->set_det_threshold(g_model,g_threshold/100.0f);
  ret=configure_grammar(g_local_cmd&1u);
  if(ret){g_mn->destroy(g_model);g_model=NULL;return ret;}
  g_mn->set_det_threshold(g_model,g_threshold/100.0f);
  int chunk=g_mn->get_samp_chunksize(g_model);
  if(g_mn->get_samp_rate(g_model)!=16000 || chunk<=0 || chunk>1024 || chunk%2)
    {g_mn->destroy(g_model);g_model=NULL;return -EINVAL;}
  printf("[Wake] local phrase=哈喽，小伴 pinyin=ha lou xiao ban chunk=%d threshold=%u/100\n",chunk,g_threshold);
  return chunk;
}
static void destroy_model(void)
{
  g_ready=false;g_local_applied=0;
  if(g_model){g_mn->destroy(g_model);g_model=NULL;}
}
static esp_mn_state_t infer_frame(int16_t *frame)
{
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  /* Equal-priority RR uses a 200 ms quantum on this build. Sharing the
   * preview priority can stall a 32 ms audio decoder for an entire quantum.
   * Stay above preview; blocking on fresh I2S input gives it the spare CPU. */
  int priority = 95;
  if (priority != g_infer_priority)
    {
      struct sched_param policy = {.sched_priority = priority};
      if (pthread_setschedparam(pthread_self(), SCHED_RR, &policy) == 0)
        g_infer_priority = priority;
    }
#endif
  clock_t before = clock_systime_ticks();
  esp_mn_state_t state = g_mn->detect(g_model, frame);
  g_last_us = (clock_systime_ticks() - before) * 1000000 / CLOCKS_PER_SEC;
  g_inferred++;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  /* Share the preview's RR priority and hand over at a frame boundary.
   * A fixed sleep wasted a full scheduler tick even when preview was blocked
   * on its next DMA frame, which accumulated stale wake audio. */
  if (camera_preview_is_running())
    {
      /* Audio advances every 32 ms; preview needs fewer updates while the
       * keyword is being decoded. Avoid one expensive display conversion
       * for every audio frame, which prevents inference keeping up. */
      if (++g_preview_infer_batch >= 5)
        {g_preview_infer_batch = 0; sched_yield();}
    }
  else g_preview_infer_batch = 0;
#endif
  return state;
}

#include "wake_match.h"
#include "wake_capture.h"

static bool consume_detection(esp_mn_state_t state, const char *source)
{
  if (state != ESP_MN_STATE_DETECTED) return false;
  esp_mn_results_t *r = g_mn->get_results(g_model);
  if(g_local_applied)
    {
      int count=r && r->num>0?(r->num<ESP_MN_RESULT_MAX_NUM?r->num:ESP_MN_RESULT_MAX_NUM):0;
      for(int i=0;i<count;i++)
        printf("[LocalCmd] rank=%d id=%d probability=%u/1000 drops=%u lag_ms=%u frame=%u source=%s dispatch=0\n",
          i+1,r->command_id[i],(unsigned)(r->prob[i]*1000),g_audio_drops,g_lag_ms,g_frames,source);
      g_mn->clean(g_model);return false;
    }
  float competitor=0;bool ambiguous=false;
  bool accepted=wake_match_accept(r,g_threshold/100.0f,&competitor,&ambiguous);
  if(ambiguous)
    printf("[Wake] ambiguous top=%u competitor=%u competitor_max_top_percent=90 dispatch=0\n",
           (unsigned)(r->prob[0]*1000),(unsigned)(competitor*1000));
  if (r && r->num > 0)
    printf("[Wake] candidate id=%d probability=%u/1000 drops=%u source=%s\n",
           r->command_id[0], (unsigned)(r->prob[0] * 1000), g_audio_drops, source);
  if (r && r->num > 0)
    {
      /* Diagnose confusable complete phrases without changing acceptance.
       * Result arrays have a fixed vendor-defined bound. */
      int count=r->num<ESP_MN_RESULT_MAX_NUM?r->num:ESP_MN_RESULT_MAX_NUM;
      for(int i=0;i<count;i++)
        printf("[Wake] rank=%d id=%d probability=%u/1000\n",
               i+1,r->command_id[i],(unsigned)(r->prob[i]*1000));
    }
  if (accepted)
    {
      if(wake_capture_suppressed())
        {
          wake_capture_detected((unsigned)(r->prob[0]*1000));
          g_mn->clean(g_model);return false;
        }
      g_hits++; g_event = true;
      printf("[Wake] detected 哈喽，小伴 probability=%u/1000 capture_drops=%u source=%s\n",
             (unsigned)(r->prob[0] * 1000), g_audio_drops, source);
    }
  g_mn->clean(g_model);
  return accepted;
}

static void *wake_worker(void *arg)
{
#if defined(CONFIG_SMP) && CONFIG_SMP_NCPUS > 1
  cpu_set_t task_cpu_set;
  CPU_ZERO(&task_cpu_set);CPU_SET(0,&task_cpu_set);
  int task_affinity_rc=sched_setaffinity(0,sizeof(task_cpu_set),&task_cpu_set);
  printf("[CoreMap] task=wake target_cpu=0 actual_cpu=%d rc=%d\n",sched_getcpu(),task_affinity_rc);
#endif

  int16_t *input = NULL, *frame = NULL, *history = NULL;
  int chunk = 0, hp = 0, prev = 0;
  unsigned applied_threshold = 0, history_pos = 0, history_count = 0;
  unsigned hold = 0, noise = 90000, drop_base = 0;
  unsigned settle = 0, calibration_min = UINT32_MAX;
  bool parked = false;
  g_infer_priority = 95;
  g_preview_infer_batch = 0;
  (void)arg;
  /* NuttX preserves the CP3 vector registers on context switches. */
  xtensa_coproc_enable(1 << 3);

  while (g_running)
    {
      /* Publish intent before inspecting pause. Otherwise pause() can see
       * an old quiet=true after we passed its check, return microphone
       * ownership, and race the parked decoder's FIFO reset below.
       */
      g_quiet = false;
      if (!g_enabled || (g_pause && g_release_requested))
        {
          destroy_model();
          free(input); free(frame); free(history);
          input = frame = history = NULL;
          parked = false;
          if(g_release_requested)g_release_done=true;
          g_quiet = true;
          usleep(20000);
          continue;
        }
      if (g_pause || voice_service_busy() || g_event)
        {
          /* Model tensors are in PSRAM. Keep them resident through an
           * ordinary dialogue: pausing audio consumption, not rebuilding
           * the model, makes return-to-listen quick and avoids heap churn.
           * Explicit off still releases all model allocations. */
          if (!parked && g_model) g_mn->clean(g_model);
          parked = true;
          g_ready = false;
          g_quiet = true;
          usleep(20000);
          continue;
        }

      if (!g_model)
        {
          chunk = create_model();
          if (chunk > 0 && ai_voice_init() >= 0)
            {
              input = memalign(16, chunk * 3);
              frame = memalign(16, chunk * 2);
              history = memalign(16, chunk * 2 * WAKE_PREROLL_FRAMES);
              applied_threshold = g_threshold;
              if (!input || !frame || !history) chunk = -ENOMEM;
            }
          else if (chunk > 0) chunk = -EIO;

          if (chunk <= 0)
            {
              printf("[Wake] disabled after init failure rc=%d\n", chunk);
              g_errors++; g_enabled = false;
              continue;
            }
          hp = prev = 0;
          history_pos = history_count = hold = 0;
          noise = 90000;
          hal_i2s_rx_discard_pending();
          drop_base = hal_i2s_rx_drop_count();
          settle = 16;
          calibration_min = UINT32_MAX;
          g_ready = false;
          parked = false;
        }
      unsigned capture_replay=atomic_exchange(&g_wake_capture_replay,0);
      bool capture_reset=atomic_exchange(&g_wake_capture_reset,false);
      if(capture_replay || capture_reset)
        {
          g_ready=false;
          if(capture_replay)wake_capture_run_replay(capture_replay);
          g_mn->clean(g_model);hal_i2s_rx_discard_pending();
          hp=prev=0;history_pos=history_count=hold=0;
          settle=8;calibration_min=UINT32_MAX;drop_base=hal_i2s_rx_drop_count();
        }
      if (parked && g_model)
        {
          g_mn->clean(g_model);
          hal_i2s_rx_discard_pending();
          hp = prev = 0;
          history_pos = history_count = hold = 0;
          settle = 8;  /* Drain the speaker tail before accepting a new phrase. */
          calibration_min = UINT32_MAX;
          drop_base = hal_i2s_rx_drop_count();
          parked = false;
        }
      if (g_pause || voice_service_busy()) continue;
      /* Only this worker owns graph mutation and inference. NSH requests a
       * change; no action/ASR path consumes these diagnostic detections. */
      unsigned command_control=g_local_cmd;
      if(g_local_applied!=(command_control&1u) || (command_control&2u))
        {
          if(g_local_applied!=(command_control&1u))
            {
              int rc=configure_grammar(command_control&1u);
              if(rc)
                {
                  printf("[LocalCmd] grammar_failed=%d dispatch=0\n",rc);
                  g_local_cmd=0;
                  if(configure_grammar(0)<0){g_enabled=false;continue;}
                }
            }
          g_mn->clean(g_model);hal_i2s_rx_discard_pending();
          hp=prev=0;history_pos=history_count=hold=0;settle=8;
          calibration_min=UINT32_MAX;g_ready=false;
          atomic_fetch_and(&g_local_cmd,~2u);
          printf("[LocalCmd] reset grammar=%u frame=%u dispatch=0\n",g_local_applied,g_frames);
        }
      /* A ready decoder must consume current sound. Continuous room noise
       * can outrun inference and fill the 2.73 s FIFO with stale audio.
       * Preserve a short newest tail, reset discontinuous decoder context,
       * and settle normally; never reset the ordinary recording path. */
      unsigned pending = hal_i2s_rx_pending_samples();
      g_lag_ms = pending / 24;
      if (pending > 48000)  /* More than 2 seconds at the native 24 kHz. */
        {
          unsigned skipped = hal_i2s_rx_keep_latest(6000);
          g_skipped += skipped;
          g_resyncs++;
          g_mn->clean(g_model);
          hp = prev = 0;
          history_pos = history_count = hold = 0;
          settle = 8;
          calibration_min = UINT32_MAX;
          g_ready = false;
          printf("[Wake] resync lag_ms=%u skipped=%u total=%u\n",
                 g_lag_ms, skipped, g_resyncs);
        }
      int got = hal_i2s_read_slot(input, chunk * 3, 0);
      if (got != chunk * 3)
        {
          g_errors++;
          printf("[Wake] capture failed rc=%d\n", got);
          g_enabled = false;
          continue;
        }
      for (int i = 0; i < chunk / 2; i++)
        {
          int vals[2] = {input[i * 3],
                        ((int)input[i * 3 + 1] + input[i * 3 + 2]) / 2};
          for (int j = 0; j < 2; j++)
            {
              hp += vals[j] - prev; hp -= hp >> 6; prev = vals[j];
              frame[i * 2 + j] = (int16_t)hp;
            }
        }
      g_frames++;
      unsigned drops = hal_i2s_rx_drop_count();
      if (drops < drop_base) drop_base = drops;
      g_audio_drops = drops - drop_base;

      uint64_t square = 0;
      unsigned peak = 0;
      for (int i = 0; i < chunk; i++)
        {
          int value = frame[i];
          if (value < 0) value = -value;
          if ((unsigned)value > peak) peak = value;
          square += (int64_t)frame[i] * frame[i];
        }
      unsigned mean = square / chunk;
      g_energy = mean;
      unsigned trigger = noise * 4;
      if (trigger < 160000) trigger = 160000;

      if (settle)
        {
          /* Calibrate against actual quiet input rather than an assumed
           * loud noise floor. Ignore the first four settling frames. */
          if (settle <= 12 && mean < calibration_min) calibration_min = mean;
          settle--;
          memcpy(history + history_pos * chunk, frame, chunk * sizeof(int16_t));
          history_pos = (history_pos + 1) % WAKE_PREROLL_FRAMES;
          if (history_count < WAKE_PREROLL_FRAMES) history_count++;
          if (!settle)
            {
              if (calibration_min != UINT32_MAX) noise = calibration_min;
              if (noise > 25000000) noise = 25000000;
              g_ready = true;
              printf("[Wake] listening local=1 gate=energy preroll=%ums threshold=%u/100\n", chunk * WAKE_PREROLL_FRAMES / 16, g_threshold);
            }
          continue;
        }

      wake_capture_frame(frame,(unsigned)chunk,mean,trigger,hold);
      if (applied_threshold != g_threshold)
        {
          applied_threshold = g_threshold;
          g_mn->set_det_threshold(g_model, applied_threshold / 100.0f);
          esp_mn_commands_update();
          g_mn->clean(g_model);
          hold = 0;
        }

      /* Keep a bounded local pre-roll. Quiet input does not run the
       * neural network, leaving CPU time for the camera. Each speech
       * segment starts with a clean decoder, so a periodic timeout cannot
       * split the middle of this short wake phrase. */
      if (!hold && mean > trigger)
        {
          printf("[Wake] gate open frame=%u energy=%u noise=%u history=%u\n",
                 g_frames, mean, noise, history_count);
          g_mn->clean(g_model);
          for (unsigned i = 0; i < history_count; i++)
            {
              unsigned slot = (history_pos + WAKE_PREROLL_FRAMES - history_count + i) % WAKE_PREROLL_FRAMES;
              /* Buffered frames are real microphone input too. A keyword
               * completed here must not be silently discarded. */
              if (consume_detection(infer_frame(history + slot * chunk), "preroll")) break;
            }
          if (g_event) continue;
          /* Huihui's natural comma pause is 608ms. A 512ms tail split
           * "ha lou" and "xiao ban" into separate decoder sessions. Keep
           * a bounded one-second tail; do not lower the recognition score. */
          hold = WAKE_DECODER_TAIL_FRAMES;
        }

      if (hold)
        {
          if (mean > trigger / 2) hold = WAKE_DECODER_TAIL_FRAMES;
          else hold--;
          esp_mn_state_t state = infer_frame(frame);
          if (state == ESP_MN_STATE_DETECTED)
            {
              consume_detection(state, "live");
            }
          else if (state == ESP_MN_STATE_TIMEOUT || !hold)
            g_mn->clean(g_model);
        }
      else
        {
          noise = ((uint64_t)noise * 63 + mean) / 64;
          if (noise > 25000000) noise = 25000000;
        }

      memcpy(history + history_pos * chunk, frame, chunk * sizeof(int16_t));
      history_pos = (history_pos + 1) % WAKE_PREROLL_FRAMES;
      if (history_count < WAKE_PREROLL_FRAMES) history_count++;
    }

  destroy_model(); free(input); free(frame); free(history);
  g_quiet = true; g_started = false;
  return NULL;
}
int voice_wake_start(void)
{
  pthread_t tid;pthread_attr_t attr;int ret=0;
  pthread_mutex_lock(&g_start_lock);
  if(!g_started)
    {
      g_running=true;g_started=true;
      pthread_attr_init(&attr);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
      pthread_attr_setstacksize(&attr,8192);
      /* RX DMA/HPWORK fills the PSRAM FIFO independently. Keep inference
       * below UI/NSH so buffered speech cannot monopolize the screen. */
      struct sched_param scheduling = {.sched_priority = 95};
      pthread_attr_setschedpolicy(&attr, SCHED_RR);
      pthread_attr_setschedparam(&attr, &scheduling);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
      ret=0;
#ifdef CONFIG_SMP
      /* Set affinity before the child can inherit the UI core and run. */
      cpu_set_t initial_cpu_set;CPU_ZERO(&initial_cpu_set);CPU_SET(0,&initial_cpu_set);
      ret=pthread_attr_setaffinity_np(&attr,sizeof(initial_cpu_set),&initial_cpu_set);
#endif
      if(ret==0)ret=pthread_create(&tid,&attr,wake_worker,NULL);pthread_attr_destroy(&attr);
      if(ret){g_started=false;g_running=false;}
    }
  pthread_mutex_unlock(&g_start_lock);return -ret;
}
void voice_wake_stop(void){g_running=false;}
int voice_wake_pause(void)
{
  g_pause=true;
  for(int i=0;i<200 && !g_quiet;i++)usleep(10000);
  if (g_quiet) hal_i2s_rx_discard_pending();
  return g_quiet?0:-ETIMEDOUT;
}
int voice_wake_pause_release(void)
{
  /* A pre-existing quiet=true acknowledges audio parking only, not freed
   * tensors. Wait for the wake worker's separate release acknowledgement.
   * Repeated followup calls reuse this lease until dialogue exits. */
  g_pause=true;
  if(!g_release_requested){g_release_done=false;g_release_requested=true;}
  for(int i=0;i<200 && !(g_quiet && g_release_done);i++)usleep(10000);
  if(!(g_quiet && g_release_done))return -ETIMEDOUT;
  hal_i2s_rx_discard_pending();
  printf("[WakeLease] model_released=1 enabled=%d\n",g_enabled);
  return 0;
}
void voice_wake_resume(void)
{g_release_requested=false;g_release_done=false;g_pause=false;}
int voice_wake_take_event(void)
{
  if(!g_event)return 0;
  g_pause=true;return atomic_exchange(&g_event,false)?1:0;
}
bool voice_wake_enabled(void) { return g_enabled; }
bool voice_wake_ready(void) { return g_ready; }
int voice_wake_command(int argc,char **argv)
{
  if(argc>=1 && !strcmp(argv[0],"capture"))return wake_capture_command(argc-1,argv+1);
  if(argc>=1 && !strcmp(argv[0],"localcmd"))
    {
      if(argc>2)return -EINVAL;
      if(argc==2)
        {
          if(voice_service_busy() || g_event)return -EBUSY;
          if(!strcmp(argv[1],"on"))g_local_cmd=1;
          else if(!strcmp(argv[1],"off"))g_local_cmd=0;
          else if(!strcmp(argv[1],"reset"))atomic_fetch_or(&g_local_cmd,2u);
          else if(strcmp(argv[1],"status"))return -EINVAL;
        }
      printf("[LocalCmd] requested=%u applied=%u dispatch=0\n",g_local_cmd&1u,g_local_applied);return 0;
    }
  if(argc==2 && !strcmp(argv[0],"threshold"))
    {
      const char *p=argv[1];
      if(strlen(p)!=2||p[0]<'1'||p[0]>'9'||p[1]<'0'||p[1]>'9')return -EINVAL;
      g_threshold=(p[0]-'0')*10+p[1]-'0';return 0;
    }
  if(argc==0||!strcmp(argv[0],"status"))
    {
      printf("[Wake] running=%d enabled=%d ready=%d quiet=%d paused=%d phrase=哈喽，小伴 local=1 frames=%u hits=%u errors=%u detect_us=%u inferred=%u audio_drops=%u energy=%u lag_ms=%u resyncs=%u skipped=%u\n",
             g_started,g_enabled,g_ready,g_quiet,g_pause,g_frames,g_hits,g_errors,g_last_us,g_inferred,g_audio_drops,g_energy,g_lag_ms,g_resyncs,g_skipped);
      return 0;
    }
  if(!strcmp(argv[0],"on")){int ret=voice_wake_start();if(ret<0)return ret;g_pause=false;g_enabled=true;return 0;}
  if(!strcmp(argv[0],"off")){g_enabled=false;g_event=false;return voice_wake_pause();}
  printf("Usage: plant wake status|on|off|threshold 10..99\n");return -EINVAL;
}
