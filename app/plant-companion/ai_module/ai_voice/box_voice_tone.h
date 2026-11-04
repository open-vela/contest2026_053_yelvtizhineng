#ifndef BOX_VOICE_TONE_H
#define BOX_VOICE_TONE_H
#include <stdint.h>
/* Mono PCM at the physical 24kHz output rate. No pitch/time change, heap
 * allocation, lookahead or whole-utterance buffer. State crosses TX blocks. */
struct box_voice_tone {float z[2][2];};
static inline int16_t box_voice_tone_sample(struct box_voice_tone *state,int16_t input)
{
 static const float filter[2][5]={
  {0.9352386577f,-1.2430174717f,0.5468897479f,-1.2430174717f,0.4821284056f},
  {0.7931932525f,-0.4711192753f,0.1861142540f,-0.7702970270f,0.2784852581f}
 };
 float x=(float)input;
 for(unsigned i=0;i<2;i++){
  const float *c=filter[i];float y=c[0]*x+state->z[i][0];
  state->z[i][0]=c[1]*x-c[3]*y+state->z[i][1];
  state->z[i][1]=c[2]*x-c[4]*y;x=y;
 }
 x*=0.92f;
 if(x>32767.0f)return 32767;
 if(x< -32768.0f)return -32768;
 return (int16_t)x;
}
#endif
