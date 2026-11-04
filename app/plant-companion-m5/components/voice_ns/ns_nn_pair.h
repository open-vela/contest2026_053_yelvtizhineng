#pragma once
#include <stddef.h>
#include <stdint.h>
void plant_nn_pair_attach(const void *owner);
void plant_nn_pair_raw(const void *owner,const int16_t *pcm,size_t samples);
void plant_nn_pair_clean(const void *owner,const int16_t *pcm,size_t samples);
void plant_nn_pair_mark(const void *owner,int speech,int allowed);
void plant_nn_pair_complete(const void *owner);
int plant_nn_pair_command(int argc,char **argv);

void plant_nn_pair_raw_mark(const void *owner,int onset,int selected_speech);
