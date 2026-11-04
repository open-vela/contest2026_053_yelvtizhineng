#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
struct voice_vad;
struct voice_vad *voice_vad_create(void);
void voice_vad_destroy(struct voice_vad *vad);
bool voice_vad_feed(struct voice_vad *vad,const int16_t *samples,size_t count);
/* This feed contained 120 ms of consecutive speech, including prior frames. */
bool voice_vad_started(const struct voice_vad *vad);
bool voice_vad_onset_corroborated(const struct voice_vad *vad);

/* Diagnostic only: bounded volatile PCM, linked SDK replay, no uploads. */
int voice_vad_capture_command(int argc,char **argv);

void voice_vad_capture_complete(struct voice_vad *vad,unsigned recorded_ms,bool triggered);

void voice_vad_strict_trace(const struct voice_vad *v,unsigned *frames,unsigned *total,unsigned *run,bool *confirmed);
unsigned voice_vad_endpoint_get_mode(void);
int voice_vad_endpoint_set_mode(unsigned mode);
bool voice_vad_endpoint_vote(struct voice_vad *,bool waiting,bool raw,bool onset);
