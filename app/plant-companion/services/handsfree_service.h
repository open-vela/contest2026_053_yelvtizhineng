#ifndef PLANT_HANDSFREE_SERVICE_H
#define PLANT_HANDSFREE_SERVICE_H
#include "handsfree_command.h"
int handsfree_service_start(void);
void handsfree_service_enable(int enabled);
int handsfree_service_enabled(void);
const char *handsfree_service_hint(void);
bool handsfree_service_hearing(void);
/* True only while a microphone recording call owns RX. */
bool handsfree_service_capturing(void);
/* Read-only background ASR state for the voice-page presentation. */
bool handsfree_service_processing(void);
bool handsfree_service_interacting(void);
void handsfree_service_screen_idle(void);
int handsfree_service_inject(const char *text);
void handsfree_service_status(void);
void handsfree_service_dump_heard(void);
void handsfree_service_trace_wake(int once);
void handsfree_service_microphone(int enabled);
void handsfree_service_announce(const char *text);
/* Queue application actions to the LVGL thread; never invoke LVGL from audio. */
int ui_app_voice_action(const struct hf_command *command,char *reply,size_t size);
void handsfree_ui_poll(void);
bool handsfree_ui_busy(void);
void ui_app_voice_root(int tab);
#endif
