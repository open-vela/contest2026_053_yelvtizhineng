#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct voice_ns_probe;
void voice_ns_trace_set(bool enabled);
bool voice_ns_trace_enabled(void);
unsigned voice_ns_probe_get_mode(void);
int voice_ns_probe_set_mode(unsigned mode);
struct voice_ns_probe *voice_ns_probe_create(void);
void voice_ns_probe_feed(struct voice_ns_probe *probe,
                         const int16_t *samples, size_t count);
void voice_ns_probe_destroy(struct voice_ns_probe *probe);
int voice_ns_probe_selftest(void);

int voice_ns_nn_smoketest(void);

bool voice_ns_probe_requested(void);
unsigned voice_ns_endpoint_get_mode(void);
int voice_ns_endpoint_set_mode(unsigned mode);
bool voice_ns_endpoint_vote(struct voice_ns_probe *probe,bool waiting,
                            bool raw_speech,bool raw_onset,
                            int32_t raw_mean,int32_t raw_noise);
