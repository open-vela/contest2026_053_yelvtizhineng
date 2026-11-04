#ifndef BOX_AEC_H
#define BOX_AEC_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef void (*box_aec_sink)(const int16_t *,size_t,void *);
bool box_aec_init(void);
bool box_aec_ready(void);
void box_aec_begin(void);
void box_aec_set_speaker_active(bool active);
void box_aec_feed(const int16_t *mic,const int16_t *ref,size_t n,box_aec_sink sink,void *arg);
void box_aec_finish(box_aec_sink sink,void *arg);
bool box_aec_speaker_active(void);
void box_aec_record_begin(void);
void box_aec_record_end(box_aec_sink sink,void *arg);
void box_aec_dump(void);
#endif
