#include <nuttx/config.h>
#include <stdatomic.h>
#ifdef CONFIG_PLANT_AI_VOICE
#include "handsfree_service.h"
#endif
#include <stdio.h>
#include <time.h>
#include "power_service.h"
static struct box_power_policy policy;
static _Atomic unsigned activity_serial,request_serial;
static _Atomic int mailbox,public_state,applied=100,apply_error;
static _Atomic bool public_eco;
static _Atomic unsigned public_busy,public_idle;
static unsigned seen_activity,seen_request;
static uint32_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)(t.tv_sec*1000u+t.tv_nsec/1000000u);}
void box_power_activity(void){atomic_fetch_add(&activity_serial,1);}
void box_power_request(enum box_power_request r){
#ifdef CONFIG_PLANT_AI_VOICE
 if(r==BOX_POWER_OFF)handsfree_service_screen_idle();
#endif
 atomic_store(&mailbox,r);atomic_fetch_add(&request_serial,1);
}
int box_power_poll(unsigned busy_mask)
{
 uint32_t now=now_ms();unsigned a=atomic_load(&activity_serial),r=atomic_load(&request_serial);
 if(a!=seen_activity){seen_activity=a;box_power_policy_activity(&policy,now);}
 if(r!=seen_request){seen_request=r;box_power_policy_request(&policy,atomic_load(&mailbox),now);}
 enum box_power_state s=box_power_policy_step(&policy,now,busy_mask!=0);
 atomic_store(&public_busy,busy_mask);atomic_store(&public_idle,now-policy.last);
 atomic_store(&public_state,s);atomic_store(&public_eco,policy.eco);
 return s==BOX_STANDBY?0:s==BOX_DIM?30:policy.eco?60:100;
}
bool box_power_standby(void){return atomic_load(&public_state)==BOX_STANDBY&&atomic_load(&applied)==0&&atomic_load(&apply_error)==0;}
unsigned box_power_sensor_interval(unsigned normal_ms)
{return box_power_standby()?60000u:atomic_load(&public_eco)?15000u:normal_ms;}
void box_power_applied(int pct,int result){atomic_store(&apply_error,result);if(!result)atomic_store(&applied,pct);}
void box_power_status(void)
{
 printf("[Power] state=%d eco=%d backlight=%d error=%d sensor_ms=%u wake_listener=preserved deep_sleep=unsupported battery_calibrated=0\n",atomic_load(&public_state),atomic_load(&public_eco),atomic_load(&applied),atomic_load(&apply_error),box_power_sensor_interval(3000));
 printf("[PowerDiag] busy_mask=%u idle_ms=%u activity=%u requests=%u\n",atomic_load(&public_busy),atomic_load(&public_idle),atomic_load(&activity_serial),atomic_load(&request_serial));
 extern void board_box_power_status(void);board_box_power_status();
}
