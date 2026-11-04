#ifndef PLANT_VOICE_SCENE_H
#define PLANT_VOICE_SCENE_H
#include <stdbool.h>
#include <stdint.h>
enum voice_scene { SCENE_IDLE, SCENE_LISTEN, SCENE_THINK, SCENE_SPEAK };
/* Presentation only; never changes audio ownership. */
static inline enum voice_scene voice_scene_choose(bool busy,int state,
 bool cancelling,bool asr,bool hearing,bool interacting,bool enabled,bool image_busy)
{
 if(busy){
  if(cancelling)return SCENE_THINK;
  if(state==1)return SCENE_LISTEN;
  if(state==3)return SCENE_SPEAK;
  return SCENE_THINK;
 }
 if(enabled&&asr)return SCENE_THINK;
 if(enabled&&(hearing||interacting))return SCENE_LISTEN;
 return image_busy?SCENE_THINK:SCENE_IDLE;
}
static inline int voice_scene_frame(enum voice_scene scene,uint32_t elapsed)
{
 if(scene==SCENE_LISTEN)return 2;
 if(scene==SCENE_THINK)return 3;
 if(scene==SCENE_SPEAK)return ((elapsed/180)%2)?5:4;
 uint32_t phase=elapsed%4600;
 return (phase>=3500&&phase<3650)||(phase>=3860&&phase<3980)?1:0;
}
#endif
