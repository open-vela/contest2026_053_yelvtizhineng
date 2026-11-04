#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Endpoint experiment only. Raw onset/PCM/pre-roll remain independent.
 * Keep recent raw speech that is strong relative to its noise floor, including
 * tiny absolute amplitudes. No absolute energy floor. This does not prove
 * preservation of unseen low-SNR long speech; actual phrase tests are needed.
 */
static inline bool plant_endpoint_speech(unsigned *recent,bool raw,
                                         int32_t mean,int32_t noise,
                                         bool nn_valid,bool nn_speech)
{
  if(noise<1)noise=1;
  if(raw && (int64_t)mean>=(int64_t)noise*2)*recent=2;
  else if(*recent)--*recent;
  return nn_valid?(nn_speech || (raw && *recent>0)):raw;
}
