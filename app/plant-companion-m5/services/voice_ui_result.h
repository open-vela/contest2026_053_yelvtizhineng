#ifndef VOICE_UI_RESULT_H
#define VOICE_UI_RESULT_H
#include <errno.h>
#include "voice_service.h"
/* Only classify a failed uploader outcome. Discarding a silent/cancelled
 * recording is expected; its -ECANCELED upload result is not a network fault.
 * This observer never changes recording, upload or session control flow. */
static inline int voice_ui_upload_failure_reason(int record_ms,bool discard,bool integrity_bad)
{
 if(record_ms==-ECANCELED)return VOICE_UI_OK;
 if(record_ms<0)return record_ms==-ENOMEM || record_ms==-EBUSY || record_ms==-EINVAL?VOICE_UI_SERVICE:VOICE_UI_MIC;
 if(integrity_bad)return VOICE_UI_MIC;
 return discard?VOICE_UI_OK:VOICE_UI_NETWORK;
}
#endif
