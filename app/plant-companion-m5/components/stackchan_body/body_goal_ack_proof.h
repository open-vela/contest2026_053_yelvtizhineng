/* A missing ordinary goal-WRITE ACK is NOT success. It can only defer
 * disposition to one bounded idempotent retry with a full ACK, followed by
 * the already-mandatory exact goal and fresh health reads.
 * All other packets, partial/error ACKs and exhausted frame windows fail. */
#ifndef BODY_GOAL_ACK_PROOF_H
#define BODY_GOAL_ACK_PROOF_H
#include <stdbool.h>
#define BODY_GOAL_ACK_UNOBSERVED 1
#define BODY_GOAL_ACK_PREFERENCE_MS 2u
static inline bool body_goal_ack_can_defer(unsigned instruction,int reg,
 int count,int reply_count,int level,bool frame_active,unsigned used,
 unsigned elapsed,unsigned arrival_limit)
{
 return instruction==3 && reg==42 && count==7 && reply_count==0 && level==1 &&
        frame_active && used==0 && elapsed>=BODY_GOAL_ACK_PREFERENCE_MS &&
        elapsed<arrival_limit;
}
#endif
