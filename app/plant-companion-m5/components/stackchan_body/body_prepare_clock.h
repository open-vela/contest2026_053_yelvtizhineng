#pragma once
#include <stdint.h>
/* Measure settle by elapsed wall time, not by accumulated tick-rounded naps.
 * Returning0 means the existing1200ms settle interval has elapsed. */
static inline unsigned body_prepare_settle_left(uint32_t started,uint32_t now)
{
 uint32_t elapsed=now-started;
 return elapsed>=1200u?0u:(1200u-elapsed<20u?1200u-elapsed:20u);
}
