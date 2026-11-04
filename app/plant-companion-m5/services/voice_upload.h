#ifndef PLANT_VOICE_UPLOAD_H
#define PLANT_VOICE_UPLOAD_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
struct voice_upload;
typedef int (*voice_upload_begin_cb)(void *);
typedef int (*voice_upload_send_cb)(const uint8_t *, size_t, void *);
typedef bool (*voice_upload_cancel_cb)(void *);
/* The owner keeps this append-only PCM buffer alive until finish returns.
 * Exactly one producer publishes a length after copying immutable samples.
 * Network callbacks run exclusively in the uploader thread, never capture.
 */
struct voice_upload *voice_upload_start(const uint8_t *buffer,size_t capacity,
    voice_upload_begin_cb begin,voice_upload_send_cb send,
    voice_upload_cancel_cb cancelled,void *arg);
void voice_upload_publish(struct voice_upload *,size_t length);
/* Join before freeing PCM. No failed or cancelled request is replayed.
 * On join failure *sent=SIZE_MAX: the owner MUST retain the PCM and handle.
 */
int voice_upload_finish(struct voice_upload *,bool discard,size_t *sent);
#endif
