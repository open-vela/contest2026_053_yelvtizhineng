#pragma once
#include <stdbool.h>
#include <nuttx/config.h>
#ifdef CONFIG_PLANT_VOICE_WAKE
int voice_wake_start(void);
void voice_wake_stop(void);
int voice_wake_pause(void);
int voice_wake_pause_release(void);
void voice_wake_resume(void);
int voice_wake_take_event(void);
int voice_wake_command(int argc,char **argv);
bool voice_wake_enabled(void);
bool voice_wake_ready(void);
#else
static inline bool voice_wake_enabled(void){return false;}
static inline bool voice_wake_ready(void){return false;}
static inline int voice_wake_start(void){return 0;}
static inline void voice_wake_stop(void){}
static inline int voice_wake_pause(void){return 0;}
static inline int voice_wake_pause_release(void){return 0;}
static inline void voice_wake_resume(void){}
static inline int voice_wake_take_event(void){return 0;}
#endif
