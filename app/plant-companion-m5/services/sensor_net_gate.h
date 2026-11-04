#ifndef PLANT_SENSOR_NET_GATE_H
#define PLANT_SENSOR_NET_GATE_H
#include <stdint.h>
#include <stdbool.h>
struct sensor_net_gate {uint32_t last;bool seen;};
/* Claim attempts before I/O, including failures; callers keep gates across
 * worker pause/restart. Unsigned subtraction supports uptime wraparound. */
static bool sensor_net_claim(struct sensor_net_gate *gate,uint32_t now,uint32_t interval)
{
  if(interval<60000u)interval=60000u;
  if(gate->seen && (uint32_t)(now-gate->last)<interval)return false;
  gate->seen=true;gate->last=now;return true;
}
#endif
