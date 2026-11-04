#ifndef HANDSFREE_AUDIO_GATE_H
#define HANDSFREE_AUDIO_GATE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
enum {HF_AUDIO_BEGIN=1,HF_AUDIO_FEED=2,HF_AUDIO_END=4};
/* Input is echo-cleaned PCM at 16kHz. Do not run the large recognizer on
 * every quiet speaker frame: retain onset context and bound each utterance. */
static inline int hf_audio_gate_update(bool *started,uint32_t *quiet,
    const int16_t *pcm,size_t count,bool speaking)
{
  if(!count)return 0;
  uint64_t energy=0;
  for(size_t i=0;i<count;i++){int32_t s=pcm[i];energy+=(uint64_t)(s*s);}
  /* Residual speaker hum must not start a network confirmation that can
   * mask the next real word. Once started, keep soft syllables intact. */
  uint32_t threshold=speaking?(*started?9000u:90000u):9000u;
  bool active=energy/count>=threshold;
  if(!*started)
    {
      if(!active)return 0;
      *started=true;*quiet=0;return HF_AUDIO_BEGIN;
    }
  *quiet=active?0:*quiet+(uint32_t)count;
  return HF_AUDIO_FEED | (*quiet>=(speaking?8000u:16000u)?HF_AUDIO_END:0);
}
/* Ordinary speech that supersedes a pending ASR needs 200ms of energy.
 * Count in 20ms frames: a short click straddling 125ms DMA callbacks must
 * not be rounded up into 250ms of apparent speech. Wake is independent. */
static inline bool hf_followup_has_speech(const int16_t *pcm,size_t count)
{
 if(!pcm)return false;
 size_t active=0;
 for(size_t offset=0;offset<count;){
  size_t n=count-offset;if(n>320u)n=320u;
  uint64_t energy=0;
  for(size_t i=0;i<n;i++){int32_t v=pcm[offset+i];energy+=(uint64_t)(v*v);}
  if(energy/n>=9000u){active+=n;if(active>=3200u)return true;}
  offset+=n;
 }
 return false;
}
/* A wake hit is often followed immediately by its command. Keep recording
 * to a short quiet boundary instead of cutting that command's first word. */
static inline bool hf_wake_tail_update(uint32_t *quiet,const int16_t *pcm,size_t count)
{
 if(!count)return false;
 uint64_t energy=0;
 for(size_t i=0;i<count;i++){int32_t v=pcm[i];energy+=(uint64_t)(v*v);}
 *quiet=energy/count>=9000u?0:*quiet+(uint32_t)count;
 return *quiet>=8000u;
}
/* Full utterance replays stop draining RX while they run. A single noisy
 * callback must not start seconds of extra model work. Keep all live frames
 * and trailing-frame recognition unchanged; only require 200ms of the same
 * existing energy evidence for the optional phase replays. */
static inline bool hf_replay_has_speech(const int16_t *pcm,size_t count)
{
  if(!pcm)return false;
  size_t active=0;
  for(size_t offset=0;offset<count;){
    size_t n=count-offset;if(n>2004u)n=2004u;
    uint64_t energy=0;
    for(size_t i=0;i<n;i++){int32_t v=pcm[offset+i];energy+=(uint64_t)(v*v);}
    if(energy/n>=9000u){active+=n;if(active>=3200u)return true;}
    offset+=n;
  }
  return false;
}
#endif
