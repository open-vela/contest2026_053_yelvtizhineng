/* Shared child-facing shapes. UI objects only; no service, audio or motor work. */
#ifndef PLANT_COMPANION_STYLE_H
#define PLANT_COMPANION_STYLE_H
#include "theme_plant.h"
#define PAL_BG 0xFFF5DF
#define PAL_FG 0x244A38
#define PAL_EYE 0x2F6B45
#define PAL_LEAF 0x75B843
#define PAL_PAPER 0xFFFBF0
#define PAL_YELLOW 0xFFD166
#define PAL_WATER 0x236B8E
static inline lv_obj_t *pal_shape(lv_obj_t *p,int x,int y,int w,int h,unsigned c,int radius)
{
 lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
 lv_obj_set_style_bg_color(o,lv_color_hex(c),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
 lv_obj_set_style_radius(o,radius,0);return o;
}
static inline lv_obj_t *pal_text(lv_obj_t *p,int x,int y,int w,int h,const char *s,const lv_font_t *f)
{
 lv_obj_t *l=lv_label_create(p);lv_obj_set_pos(l,x,y);lv_obj_set_size(l,w,h);
 lv_obj_set_style_text_font(l,f,0);lv_obj_set_style_text_color(l,lv_color_hex(PAL_FG),0);
 lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);lv_label_set_text(l,s);
 lv_obj_remove_flag(l,LV_OBJ_FLAG_CLICKABLE);return l;
}
static inline lv_obj_t *pal_button(lv_obj_t *p,int x,int y,int w,int h,const char *s,lv_event_cb_t cb,void *data)
{
 lv_obj_t *b=lv_button_create(p);lv_obj_remove_style_all(b);lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
 lv_obj_set_style_radius(b,18,0);lv_obj_set_style_bg_color(b,lv_color_hex(PAL_YELLOW),0);
 lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);lv_obj_set_style_border_width(b,2,0);
 lv_obj_set_style_border_color(b,lv_color_hex(PAL_FG),0);
 lv_obj_set_style_bg_color(b,lv_color_hex(0xF5BD49),LV_STATE_PRESSED);
 lv_obj_t *l=pal_text(b,2,0,w-4,h,s,TP_FONT_BODY);
 lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_height(l,LV_SIZE_CONTENT);lv_obj_center(l);
 if(cb)lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,data);return b;
}
/* Same leaf, eye proportions, highlights and cheeks as the conversation face.
 * A static small companion avoids adding hidden-page animation timers. */
static inline lv_obj_t *pal_face(lv_obj_t *p,int x,int y,int w,int h)
{
 lv_obj_t *face=pal_shape(p,x,y,w,h,PAL_BG,18);
 pal_shape(face,w/2-3,0,w/5,h/7,PAL_LEAF,LV_RADIUS_CIRCLE);
 for(int i=0;i<2;i++){
  int ex=i?w*65/100:w*20/100;
  lv_obj_t *eye=pal_shape(face,ex,h*27/100,w*15/100,h*39/100,PAL_EYE,LV_RADIUS_CIRCLE);
  pal_shape(eye,3,4,w/20,h/10,PAL_BG,LV_RADIUS_CIRCLE);
  pal_shape(face,ex-3,h*72/100,w/7,4,0xE99B7B,LV_RADIUS_CIRCLE);
 }
 lv_obj_t *smile=lv_arc_create(face);lv_obj_remove_style_all(smile);
 lv_obj_remove_flag(smile,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_size(smile,w/5,h/4);lv_obj_set_pos(smile,w*40/100,h*61/100);
 lv_arc_set_bg_angles(smile,25,155);lv_obj_set_style_arc_width(smile,3,LV_PART_MAIN);
 lv_obj_set_style_arc_color(smile,lv_color_hex(PAL_FG),LV_PART_MAIN);
 return face;
}
static inline lv_obj_t *pal_page(const char *title)
{
 lv_obj_t *p=pal_shape(lv_screen_active(),0,0,320,240,PAL_BG,0);
 pal_text(p,12,7,296,24,title,TP_FONT_TITLE);return p;
}
#endif
