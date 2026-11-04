#ifndef VOICE_PCM_WINDOW_H
#define VOICE_PCM_WINDOW_H
#include <stddef.h>
#include <stdint.h>
/* 16 kHz mono: keep quiet consonants and pauses inside the utterance.
 * Only outer silence is removed, with 800 ms padding on each side. */
static inline void voice_pcm_window(const int16_t *pcm,size_t count,
                                   size_t *offset,size_t *length)
{
  *offset=0;*length=count;
  if(!pcm || count<=16000)return;
  size_t first=count,last=0;
  for(size_t start=0;start<count;start+=320){
    size_t n=count-start;if(n>320)n=320;
    uint64_t energy=0;
    for(size_t i=0;i<n;i++){int32_t v=pcm[start+i];energy+=(uint64_t)(v*v);}
    if(energy/n>=4500){if(first==count)first=start;last=start+n;}
  }
  if(first==count)return;
  first=first>12800?first-12800:0;
  last=count-last>12800?last+12800:count;
  if(last-first<16000){
    size_t middle=first+(last-first)/2;
    first=middle>8000?middle-8000:0;
    if(first+16000>count)first=count-16000;
    last=first+16000;
  }
  *offset=first;*length=last-first;
}
#endif
