#ifndef PLANT_HANDSFREE_COMMAND_H
#define PLANT_HANDSFREE_COMMAND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum hf_action {
 HF_IGNORE, HF_WAKE, HF_ASK, HF_HOME, HF_DATA, HF_TALK, HF_CAMERA,
 HF_PHOTO, HF_SAVE, HF_RESULT, HF_TASKS, HF_DIARY, HF_BACK,
 HF_NEXT, HF_PREVIOUS, HF_ALL, HF_METRIC, HF_VOLUME, HF_VOLUME_SET,
 HF_MUTE, HF_UNMUTE, HF_STOP, HF_SLEEP, HF_HELP, HF_NETWORK, HF_INFO,
 HF_TASK_DONE, HF_TASK_UNDONE, HF_WATER, HF_REPEAT, HF_CAMERA_CLOSE, HF_SCREEN_OFF, HF_POWER_ECO, HF_POWER_NORMAL, HF_KEEP_SCREEN, HF_DEEP_REST
};
struct hf_command { enum hf_action action; int value; bool woke; char text[512]; };
struct hf_gate { bool awake; uint32_t touched; };
size_t hf_normalize(const char *text,char *out,size_t capacity);
struct hf_command hf_parse(struct hf_gate *gate,const char *text,uint32_t now);
void hf_keep_awake(struct hf_gate *gate,uint32_t now);
bool hf_is_awake(const struct hf_gate *gate,uint32_t now);
const char *hf_action_name(enum hf_action action);
void hf_canonicalize_local_wake(char *text,size_t capacity);
/* Weak acoustic candidates require tolerant phonetic confirmation. */
bool hf_weak_wake_text_matches(const char *text);
#endif
