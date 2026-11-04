/* No motor/power callbacks: observers are explicitly requested and reversible. */
#ifndef BODY_OBSERVE_POLICY_H
#define BODY_OBSERVE_POLICY_H
#include <stdbool.h>
#include <errno.h>
static inline int body_observe_arm(bool requested,int (*timing)(bool),int (*counter)(bool))
{
 if(!requested)return 0;
 if(!timing || !counter)return -EINVAL;
 int rc=timing(true);if(rc<0)return rc;
 rc=counter(true);if(rc<0){int restored=timing(false);return restored<0?restored:rc;}
 return 0;
}
#endif
