#ifndef PLANT_BOX_PHRASE_H
#define PLANT_BOX_PHRASE_H
#include <stdint.h>
#include <stddef.h>
int box_phrase_init(void);
void box_phrase_reset(void);
int box_phrase_ready(void);
/* Reuse the existing model; commands are enabled only in the awake turn. */
int box_phrase_command_mode(int awake);
const char *box_phrase_command_text(int id);
int box_phrase_command_probability(void);
/* 1 = acoustic wake, 2 = stop, 40..53 = awake commands, 0 = no hit. */
int box_phrase_detect(const int16_t *pcm,size_t samples,int allow_stop);
/* 3 = weak local candidate; tolerant ASR confirmation is mandatory. */
int box_phrase_weak_candidate(const int16_t *pcm,size_t samples,size_t onset);
/* Feed live microphone chunks without pausing capture for a whole-clip scan. */
int box_phrase_stream_begin(void);
int box_phrase_stream_feed(const int16_t *pcm,size_t samples,int allow_stop);
int box_phrase_stream_end(int allow_stop);
/* Flush a final partial frame without replaying a second of silence. */
int box_phrase_stream_end_live(int allow_stop);
int box_phrase_finish_utterance(const int16_t *pcm,size_t samples,int allow_stop,int replay_miss);
int box_phrase_finish_window(const int16_t *pcm,size_t samples,size_t onset,int allow_stop,int replay_miss);
void box_phrase_rx_guard(int enable);
int box_phrase_rx_was_yielded(void);
void box_phrase_status(void);
int box_phrase_test_file(const char *path);
/* Maintenance only; caller must pause microphone capture first. No actions. */
int box_phrase_test_command_file(const char *path);
int box_phrase_test_file_at(const char *path,size_t onset);
int box_phrase_set_threshold(int percent);
#endif
