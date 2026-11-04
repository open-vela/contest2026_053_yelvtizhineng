#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Only gate onset, never endpoint or PCM. Keep one previous 125 ms block:
 * a soft final syllable can complete WebRTC's 120 ms confirmation after a
 * louder syllable. There is no absolute energy floor to exclude quiet speech.
 */
static inline bool voice_onset_accept(unsigned *recent, int32_t mean,
                                      int32_t noise, bool speech,
                                      bool confirmed)
{
  if (noise < 1) noise = 1;
  if ((int64_t)mean >= (int64_t)noise * 2) *recent = 2;
  else if (*recent) --*recent;
  return speech && confirmed && *recent > 0;
}
