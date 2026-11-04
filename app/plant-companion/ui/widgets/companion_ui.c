#include "companion_ui.h"
#include "companion_art_decoder.h"
#include "../ui_app.h"
#include "../assets/fonts/zh_font.h"

/* Keep the designed font metrics and flash glyphs, then use the existing
 * UI-owned Chinese/icon/emoji chain for dynamic text outside the subset.
 * zh_font_get returns stable RAM font objects, including late SD retry. */
static const lv_font_t *cu_text_font(int size)
{
 static lv_font_t small,medium,large;
 if(small.get_glyph_dsc==NULL){small=lv_font_companion_16;medium=lv_font_companion_22;large=lv_font_companion_30;}
 lv_font_t *font=size>=28?&large:size>=20?&medium:&small;
 font->fallback=zh_font_get(size>=28?25:size>=20?19:16);
 return font;
}

lv_obj_t *cu_panel(lv_obj_t *p,int x,int y,int w,int h,uint32_t color,int radius){
 lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
 lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
 lv_obj_set_style_radius(o,radius,0);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;
}
lv_obj_t *cu_label(lv_obj_t *p,int x,int y,int w,const char *s,int size,uint32_t color){
 lv_obj_t *o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
 lv_obj_set_style_text_font(o,cu_text_font(size),0);
 lv_obj_set_style_text_color(o,lv_color_hex(color),0);lv_label_set_text(o,s);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);return o;
}
lv_obj_t *cu_button(lv_obj_t *p,int x,int y,int w,int h,const char *s,int size,uint32_t color,lv_event_cb_t cb,void *data){
 lv_obj_t *o=cu_panel(p,x,y,w,h,color,18);
 lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_style_bg_opa(o,LV_OPA_70,LV_STATE_PRESSED);
 lv_obj_t *l=cu_label(o,0,0,w,s,size,color==CU_GREEN?0xffffff:CU_INK);
 lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(l);
 if(cb)lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,data);return o;
}
lv_obj_t *cu_root(void){cu_art_decoder_init();return cu_panel(lv_screen_active(),0,0,480,320,CU_BG,0);}
void cu_image(lv_obj_t *p,const lv_image_dsc_t *src,int x,int y){
 lv_obj_t *o=lv_image_create(p);lv_image_set_src(o,src);lv_obj_set_pos(o,x,y);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);
}
void cu_back(lv_event_t *e){(void)e;ui_app_pop_screen();}
