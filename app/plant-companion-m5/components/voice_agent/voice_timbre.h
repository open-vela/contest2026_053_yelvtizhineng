#pragma once
#include <stdint.h>
void voice_timbre_reset(void);
void voice_timbre_set_soft(int enabled);
int voice_timbre_get_soft(void);
int16_t voice_timbre_sample(int16_t input);
