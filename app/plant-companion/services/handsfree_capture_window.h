#ifndef HANDSFREE_CAPTURE_WINDOW_H
#define HANDSFREE_CAPTURE_WINDOW_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* A quiet recording can end just as a soft first syllable begins. Keep
 * that decoded prefix for the next window, including while already awake. */
static inline size_t hf_capture_keep_tail(int16_t *pcm,size_t count,
                                         bool awake,bool utterance_ended)
{
  if(!pcm || utterance_ended)return 0;
  size_t keep=awake?16000u:32000u;if(keep>count)keep=count;
  if(keep)memmove(pcm,pcm+count-keep,keep*sizeof(*pcm));
  return keep;
}
static inline int hf_capture_seconds(size_t capacity,size_t prefix,bool awake)
{
  (void)awake;
  if(prefix>=capacity)return 0;
  return (int)((capacity-prefix)/16000u);
}
/* Eight seconds is a quiet-window deadline, not an utterance deadline.
 * A late first syllable gets the remaining hard capacity before recognition
 * replays temporarily stop draining the microphone FIFO. */
static inline bool hf_capture_quiet_deadline(size_t count,size_t prefix,
                                            bool awake,bool voice_started)
{
  return !awake&&!voice_started&&count>=prefix&&count-prefix>=128000u;
}
#endif
