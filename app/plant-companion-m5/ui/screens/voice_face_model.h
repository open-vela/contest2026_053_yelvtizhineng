#ifndef VOICE_FACE_MODEL_H
#define VOICE_FACE_MODEL_H
#include "../../services/voice_service.h"
enum face_state {FACE_IDLE,FACE_PREPARE,FACE_LISTEN,FACE_THINK,FACE_SPEAK,FACE_STOP,FACE_RETRY,FACE_OFFLINE,FACE_MIC,FACE_CARE,FACE_SERVICE};
static inline int face_state_from_voice(const struct voice_ui_snapshot *s)
{
 if(s->cancelling)return FACE_STOP;
 if(s->error==VOICE_UI_MIC)return FACE_MIC;
 if(s->error==VOICE_UI_NETWORK)return FACE_OFFLINE;
 if(s->error==VOICE_UI_SERVICE)return FACE_SERVICE;
 if(s->error==VOICE_UI_NO_SPEECH)return FACE_RETRY;
 if(s->busy){
  if(s->state==VOICE_STATE_LISTENING)return s->capture_ready?FACE_LISTEN:FACE_PREPARE;
  if(s->state==VOICE_STATE_SPEAKING)return s->playback_active?FACE_SPEAK:FACE_THINK;
  return s->state==VOICE_STATE_THINKING?FACE_THINK:FACE_PREPARE;
 }
 if(s->dialogue)return FACE_PREPARE;
 return FACE_IDLE;
}
static inline const char *face_caption(int state)
{
 static const char *const captions[]={"嗨，我是小绿","准备一下","我在听","想一想","我来说啦","停一下","再说一次？","暂时连不上网","还听不到声音","一起照顾小花","暂时没答上来"};
 return captions[state>=0 && state<=FACE_SERVICE?state:FACE_SERVICE];
}
static inline int face_action_is_stop(const struct voice_ui_snapshot *s)
{return s->busy || s->dialogue;}
#endif
