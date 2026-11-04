#ifndef BODY_FEEDBACK_POLICY_H
#define BODY_FEEDBACK_POLICY_H
#include <stdbool.h>
/* Official Hal servo_init: only id2 has enableStallProtection=true.
 * An explicitly requested custom hold retains both-axis monitoring.
 * Startup and final both-axis goal/encoder/health proofs are separate. */
static inline bool body_official_feedback_axis(unsigned id,unsigned axes,bool nod,bool explicit_hold)
{return (id==1||id==2) && (explicit_hold || (id==2 && (nod || (axes&2u))));}
#endif
