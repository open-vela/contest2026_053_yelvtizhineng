/****************************************************************************
 * GC0308 camera sensor helpers.
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_GC0308_H
#define __APPS_PLANT_COMPANION_GC0308_H

#include <stdint.h>

int gc0308_init(void);
int gc0308_config_rgb565(uint16_t w, uint16_t h);
int gc0308_config_yuv422(uint16_t w, uint16_t h);
int gc0308_resume_yuv_qvga(void);
int gc0308_shutdown(void);

#endif /* __APPS_PLANT_COMPANION_GC0308_H */
