#ifndef PLANT_INTERACTION_SERVICE_H
#define PLANT_INTERACTION_SERVICE_H
#include <stdbool.h>
#include <stddef.h>
int interaction_submit(const char *action, const char *arg, const char *id);
void interaction_tick(void); /* UI thread only, bounded and non-blocking. */
bool interaction_active(void);
bool interaction_voice_blocked(void);
void interaction_stop(void);
void interaction_pause_motion(void);
int interaction_get_status(char *text,size_t n);
int interaction_command(int argc, char **argv);
#endif
