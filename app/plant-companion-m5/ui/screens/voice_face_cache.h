#ifndef VOICE_FACE_CACHE_H
#define VOICE_FACE_CACHE_H
#include <stdint.h>
/* The voice page never polls the power bus. Only a recent, real reading from
 * the existing Home refresh may be displayed; stale/unknown values hide. */
static inline int face_cached_battery_value(int pct,uint32_t now,uint32_t sampled)
{return pct>=0 && pct<=100 && (uint32_t)(now-sampled)<90000u?pct:-1;}
#endif
