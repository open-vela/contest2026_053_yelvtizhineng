#ifndef COMPANION_UI_H
#define COMPANION_UI_H
#include <lvgl/lvgl.h>
#define CU_BG 0xf4f8ed
#define CU_INK 0x244b39
#define CU_GREEN 0x45895d
extern const lv_image_dsc_t cu_buddy,cu_soil_plant,cu_mic,cu_camera,cu_leaf,cu_parent;
extern const lv_font_t lv_font_companion_16,lv_font_companion_22,lv_font_companion_30;
lv_obj_t *cu_panel(lv_obj_t *p,int x,int y,int w,int h,uint32_t color,int radius);
lv_obj_t *cu_label(lv_obj_t *p,int x,int y,int w,const char *s,int size,uint32_t color);
lv_obj_t *cu_button(lv_obj_t *p,int x,int y,int w,int h,const char *s,int size,uint32_t color,lv_event_cb_t cb,void *data);
lv_obj_t *cu_root(void);
void cu_image(lv_obj_t *p,const lv_image_dsc_t *src,int x,int y);
void cu_back(lv_event_t *e);
#endif
