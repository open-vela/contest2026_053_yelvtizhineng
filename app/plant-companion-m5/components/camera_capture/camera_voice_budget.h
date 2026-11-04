#ifndef PLANT_CAMERA_VOICE_BUDGET_H
#define PLANT_CAMERA_VOICE_BUDGET_H
#include <stdbool.h>
#include <stdint.h>
struct cam_voice_budget_s { uint32_t last; bool have; bool limited; };
/* Called only by the preview producer, outside frame leases and locks.
 * Tick subtraction remains valid across wrap. Fresh snapshot requests drain
 * their pipeline promptly instead of inheriting the preview rate limit. */
static inline bool cam_voice_frame_due(struct cam_voice_budget_s *s,
                                      uint32_t now, uint32_t interval,
                                      bool voice, bool snapshot)
{
  if (!voice || snapshot || !s->have || (uint32_t)(now-s->last)>=interval)
    { s->last=now; s->have=true; return true; }
  return false;
}
#endif
