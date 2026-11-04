/* Gentle high-shelf reduction for the small CoreS3 speaker, at 24kHz.
 * A bilinear one-pole lowpass at ~2.8kHz mixed 1:1 with the original:
 * DC gain 0dB, high-frequency shelf -6dB. No bass boost or extra buffering.
 * Preserve filter state across PCM/network block boundaries.
 */
#include "voice_timbre.h"
#include <stdatomic.h>
static _Atomic int g_soft=1;
static int32_t g_previous,g_low;
void voice_timbre_reset(void){g_previous=0;g_low=0;}
void voice_timbre_set_soft(int enabled){g_soft=enabled!=0;}
int voice_timbre_get_soft(void){return g_soft;}
int16_t voice_timbre_sample(int16_t input)
{
  int32_t low=(int32_t)(((int64_t)9090*((int32_t)input+g_previous)+
                         (int64_t)14588*g_low+16384)>>15);
  g_previous=input;g_low=low;
  return g_soft?(int16_t)(((int32_t)input+low)/2):input;
}
