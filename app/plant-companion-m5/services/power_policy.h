#ifndef PLANT_POWER_POLICY_H
#define PLANT_POWER_POLICY_H
#include <stdbool.h>
#include <stdint.h>
enum power_mode { POWER_ACTIVE,POWER_DIM,POWER_STANDBY,POWER_DEEP };
static inline enum power_mode power_policy(uint32_t idle,bool enabled,
 bool demo,bool busy,bool external,bool source_valid,uint32_t dim,
 uint32_t standby,uint32_t deep)
{
 if(!enabled||demo||busy)return POWER_ACTIVE;
 if(source_valid&&!external&&deep&&idle>=deep)return POWER_DEEP;
 if(idle>=standby)return POWER_STANDBY;
 return idle>=dim?POWER_DIM:POWER_ACTIVE;
}
#endif
