#ifndef BOX_POWER_POLICY_H
#define BOX_POWER_POLICY_H
#include <stdint.h>
#include <stdbool.h>
enum box_power_state { BOX_ACTIVE, BOX_DIM, BOX_STANDBY };
enum box_power_request { BOX_POWER_NONE, BOX_POWER_NORMAL, BOX_POWER_ECO,
 BOX_POWER_OFF, BOX_POWER_HOLD, BOX_POWER_WAKE };
struct box_power_policy {
 uint32_t last,hold_start; bool initialized,hold,eco,off_pending;
 enum box_power_state state;
};
unsigned box_power_busy_mask(bool voice,bool hearing,bool confirmed,bool ui,bool ai,bool camera);
unsigned box_power_retry_seconds(unsigned failures,bool standby);
void box_power_policy_activity(struct box_power_policy *p,uint32_t now);
void box_power_policy_request(struct box_power_policy *p,enum box_power_request r,uint32_t now);
enum box_power_state box_power_policy_step(struct box_power_policy *p,uint32_t now,bool busy);
#endif
