#ifndef HANDSFREE_INTERRUPT_H
#define HANDSFREE_INTERRUPT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "handsfree_command.h"
#include "server_bridge.h"

/* Only a short near-end utterance in an already awake playback turn may
 * interrupt. This path cannot wake from sleep or execute other actions. */
static inline int hf_interrupt_decode(bool awake,bool speaking,bool started,
    const int16_t *pcm,size_t samples,const char *spoken)
{
  if(!awake||!speaking||!started||!pcm||samples<1600||samples>64000||
      !server_bridge_configured())return false;
  char heard[512],normalized[512],echo[1024];
  int ret=server_bridge_transcribe(pcm,samples,heard,sizeof(heard));
  if(ret<0)return false;
  if(!hf_normalize(heard,normalized,sizeof(normalized)))return false;
  hf_normalize(spoken,echo,sizeof(echo));
  if(strstr(echo,normalized))return false;
  struct hf_gate gate={true,0};
  enum hf_action action=hf_parse(&gate,heard,0).action;
  return action==HF_STOP?2:action==HF_WAKE?1:0;
}
static inline bool hf_interrupt_stop(bool awake,bool speaking,bool started,
    const int16_t *pcm,size_t samples,const char *spoken)
{return hf_interrupt_decode(awake,speaking,started,pcm,samples,spoken)==2;}
#endif
