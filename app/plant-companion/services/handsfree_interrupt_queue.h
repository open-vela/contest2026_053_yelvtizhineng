#ifndef HANDSFREE_INTERRUPT_QUEUE_H
#define HANDSFREE_INTERRUPT_QUEUE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* One in-flight and one replaceable pending three-second PCM snapshot.
 * A dedicated worker processes requests; capture never waits for HTTP. */
void hf_interrupt_set_turn(uint32_t token);
int hf_interrupt_offer(uint32_t token,const int16_t *pcm,size_t samples,const char *spoken);
/* 0=no result, 1=confirmed wake, 2=confirmed stop. Consumed once. */
int hf_interrupt_take(uint32_t token);
int hf_interrupt_start(void);
void hf_interrupt_process(void);
void hf_interrupt_queue_status(void);
#endif
