#include <nuttx/config.h>
#include "screen_home.h"
#include "screen_camera.h"
#include "screen_voice.h"
#include "screen_wifi.h"
#include "../../ota/ota.h"
#include "../ui_app.h"
#include "../widgets/companion_ui.h"
#include "../../services/voice_service.h"
#include "../../services/handsfree_service.h"
#include "../../services/ai_service.h"
#include "../../components/voice_agent/voice_agent.h"
#include "../../components/system_monitor/user_preferences.h"
#include <stdio.h>
#include <string.h>
static lv_obj_t *status,*clock_label,*menu_volume_value,*menu_volume_hint;
static void photo(lv_event_t *e){
 (void)e;if(voice_service_busy()||ai_service_busy())return;
 ui_app_push_screen(screen_camera_create());
}
static void voice(lv_event_t *e){(void)e;ui_app_push_screen(screen_voice_create());}
static void data(lv_event_t *e){(void)e;ui_app_nav_to(PLANT_TAB_DATA);}
static void menu_nav(lv_event_t *e){
 int tab=(int)(intptr_t)lv_event_get_user_data(e);ui_app_pop_screen();ui_app_nav_to(tab);
}
static void wifi(lv_event_t *e){(void)e;ui_app_push_screen(screen_wifi_create());}
static void maintenance(lv_event_t *e){
 (void)e;lv_obj_t *p=cu_root();char version[64];
 cu_button(p,12,6,56,48,"返回",16,0xe4edda,cu_back,NULL);
 cu_label(p,90,14,300,"设备信息",22,CU_INK);
 cu_label(p,24,80,430,"植小伴 BOX",30,CU_INK);
 snprintf(version,sizeof(version),"固件版本 %d.%d.%d",OTA_FW_VERSION_MAJOR,OTA_FW_VERSION_MINOR,OTA_FW_VERSION_PATCH);
 cu_label(p,24,132,430,version,22,CU_INK);
 cu_label(p,24,181,430,"480 × 320 彩屏",22,CU_INK);
 cu_label(p,24,244,432,"当前固件暂不支持在线升级。\n维护时请由大人协助。",16,0x667d66);
 ui_app_push_screen(p);
}
static void volume(lv_event_t *e){
 int d=(int)(intptr_t)lv_event_get_user_data(e);
 int v=voice_agent_get_dac_volume()+d;if(v<0)v=0;if(v>255)v=255;
 if(voice_agent_set_dac_volume(v)==0){
  plant_pref_save_int("/mnt/sd/plant_volume.cfg",v);
  if(menu_volume_value)lv_label_set_text_fmt(menu_volume_value,"%d%%",(v*100+127)/255);
  if(menu_volume_hint)lv_label_set_text(menu_volume_hint,d<0?"已调低":"已调高");
 }else if(menu_volume_hint)lv_label_set_text(menu_volume_hint,"请再试一次");
}
static void menu_deleted(lv_event_t *e){(void)e;menu_volume_value=NULL;menu_volume_hint=NULL;}
static lv_obj_t *menu_shape(lv_obj_t *p,int x,int y,int w,int h,uint32_t color,int radius){
 lv_obj_t *o=cu_panel(p,x,y,w,h,color,radius);lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);return o;
}
static void menu_line(lv_obj_t *p,const lv_point_precise_t *points,int n,uint32_t color){
 lv_obj_t *o=lv_line_create(p);lv_line_set_points(o,points,n);
 lv_obj_set_style_line_color(o,lv_color_hex(color),0);lv_obj_set_style_line_width(o,2,0);
 lv_obj_set_style_line_rounded(o,true,0);lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);
}
static void menu_icon(lv_obj_t *card,int kind){
 static const uint32_t bg[]={0xe6efd2,0xf9e8d6,0xe1efec,0xe5eaf5};
 static const uint32_t ink[]={0x5f8a3f,0xb07c47,0x4a8d7a,0x647eaa};
 static const lv_point_precise_t task[]={{9,8},{7,8},{7,30},{28,30},{28,8},{25,8}};
 static const lv_point_precise_t tick[]={{11,19},{15,23},{24,14}};
 static const lv_point_precise_t book[]={{5,7},{17,10},{29,7},{29,28},{17,31},{5,28},{5,7}};
 static const lv_point_precise_t spine[]={{17,10},{17,31}};
 static const lv_point_precise_t wifi_outer[]={{3,13},{8,9},{13,7},{19,7},{24,9},{29,13}};
 static const lv_point_precise_t wifi_inner[]={{8,18},{12,15},{16,14},{20,15},{24,18}};
 static const lv_point_precise_t wifi_tip[]={{13,23},{16,22},{19,23}};
 lv_obj_t *chip=menu_shape(card,12,18,36,36,bg[kind],12);
 if(kind==0){
  menu_line(chip,task,6,ink[kind]);menu_line(chip,tick,3,ink[kind]);
  lv_obj_t *clip=menu_shape(chip,11,4,13,7,bg[kind],2);
  lv_obj_set_style_border_color(clip,lv_color_hex(ink[kind]),0);lv_obj_set_style_border_width(clip,2,0);
 }else if(kind==1){menu_line(chip,book,7,ink[kind]);menu_line(chip,spine,2,ink[kind]);}
 else if(kind==2){
  menu_line(chip,wifi_outer,6,ink[kind]);menu_line(chip,wifi_inner,5,ink[kind]);menu_line(chip,wifi_tip,3,ink[kind]);
  menu_shape(chip,14,27,4,4,ink[kind],2);
 }else{
  lv_obj_t *device=menu_shape(chip,8,4,21,29,bg[kind],5);
  lv_obj_set_style_border_color(device,lv_color_hex(ink[kind]),0);lv_obj_set_style_border_width(device,2,0);
  menu_shape(chip,12,10,13,14,0xc7d5eb,3);menu_shape(chip,16,27,4,3,ink[kind],2);
 }
}
static void menu_card(lv_obj_t *p,int x,int y,int kind,const char *title,const char *hint,lv_event_cb_t cb,void *arg){
 lv_obj_t *card=cu_button(p,x,y,216,72,title,22,0xfffdf8,cb,arg);
 lv_obj_t *label=lv_obj_get_child(card,0);
 lv_obj_align(label,LV_ALIGN_TOP_LEFT,62,10);lv_obj_set_width(label,146);
 lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_LEFT,0);
 cu_label(card,62,42,146,hint,16,0x71806b);menu_icon(card,kind);
 lv_obj_set_style_border_color(card,lv_color_hex(0xe5ebdc),0);lv_obj_set_style_border_width(card,1,0);
}
static void more(lv_event_t *e){
 (void)e;lv_obj_t *p=cu_root();
 lv_obj_set_style_bg_color(p,lv_color_hex(0xfbfaf2),0);
 lv_obj_add_event_cb(p,menu_deleted,LV_EVENT_DELETE,NULL);
 cu_button(p,12,6,64,44,"返回",16,0xe4edda,cu_back,NULL);
 lv_obj_t *title=cu_label(p,105,13,270,"更多功能",22,CU_INK);
 lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);cu_image(p,&cu_leaf,438,15);
 menu_card(p,16,64,0,"观察任务","一起发现新变化",menu_nav,(void *)PLANT_TAB_TASKS);
 menu_card(p,248,64,1,"成长日记","看看成长的记录",menu_nav,(void *)PLANT_TAB_DIARY);
 menu_card(p,16,144,2,"连接网络","连接家里的网络",wifi,NULL);
 menu_card(p,248,144,3,"设备信息","看看当前版本",maintenance,NULL);
 lv_obj_t *bar=menu_shape(p,20,232,440,72,0xe9efdf,18);
 cu_label(bar,16,9,94,"声音",22,CU_INK);
 menu_volume_hint=cu_label(bar,16,42,96,"自动保存",16,0x71806b);
 cu_button(bar,124,14,64,44,"-",30,0xfffdf8,volume,(void *)(intptr_t)-6);
 menu_volume_value=cu_label(bar,201,21,130,"",22,CU_INK);
 lv_obj_set_style_text_align(menu_volume_value,LV_TEXT_ALIGN_CENTER,0);
 lv_label_set_text_fmt(menu_volume_value,"%d%%",(voice_agent_get_dac_volume()*100+127)/255);
 cu_button(bar,350,14,64,44,"+",30,0xfffdf8,volume,(void *)(intptr_t)6);
 ui_app_push_screen(p);
}
static void deleted(lv_event_t *e){(void)e;status=NULL;clock_label=NULL;}
lv_obj_t *screen_home_create(plant_nav_cb_t cb,void *u){
 (void)cb;(void)u;lv_obj_t *p=cu_root();
 lv_obj_add_event_cb(p,deleted,LV_EVENT_DELETE,NULL);
 /* Home-only colors preserve the established appearance of other pages. */
 lv_obj_set_style_bg_color(p,lv_color_hex(0xfbfaf2),0);
 cu_image(p,&cu_leaf,16,15);
 cu_label(p,44,13,108,"植小伴",22,CU_INK);
 clock_label=cu_label(p,185,18,74,"--:--",16,0x71806b);
 lv_obj_t *status_button=cu_button(p,306,4,78,44,"状态",16,0xe4edda,data,NULL);
 cu_image(status_button,&cu_leaf,8,11);
 lv_obj_align(lv_obj_get_child(status_button,0),LV_ALIGN_TOP_LEFT,33,12);
 lv_obj_set_width(lv_obj_get_child(status_button,0),38);
 lv_obj_t *more_button=cu_button(p,396,4,72,44,"更多",16,0xe4edda,more,NULL);
 for(int i=0;i<3;i++){
  lv_obj_t *dot=cu_panel(more_button,9+i*5,20,3,3,CU_INK,2);
  lv_obj_remove_flag(dot,LV_OBJ_FLAG_CLICKABLE);
 }
 lv_obj_align(lv_obj_get_child(more_button,0),LV_ALIGN_TOP_LEFT,26,12);
 lv_obj_set_width(lv_obj_get_child(more_button,0),40);
 cu_image(p,&cu_buddy,24,36);
 lv_obj_t *headline=cu_label(p,226,77,238,"今天一起\n发现新变化",30,CU_INK);
 lv_obj_set_style_text_line_space(headline,6,0);
 status=cu_label(p,226,180,238,"每一片新叶，都值得期待",16,0x71806b);
 lv_obj_t *talk_button=cu_button(p,14,240,270,64,"和小伴说话",30,CU_GREEN,voice,NULL);
 cu_image(talk_button,&cu_mic,24,16);
 lv_obj_align(lv_obj_get_child(talk_button,0),LV_ALIGN_TOP_LEFT,65,14);
 lv_obj_set_width(lv_obj_get_child(talk_button,0),192);
 lv_obj_t *photo_button=cu_button(p,296,240,170,64,"拍照观察",22,0xe4edda,photo,NULL);
 cu_image(photo_button,&cu_camera,13,18);
 lv_obj_align(lv_obj_get_child(photo_button,0),LV_ALIGN_TOP_LEFT,48,19);
 lv_obj_set_width(lv_obj_get_child(photo_button,0),114);
 return p;
}
void screen_home_refresh(int a,int b,int c,int d,int e,int f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void screen_home_set_status(const char *t,const char *d,int online,int bat){
 (void)d;(void)bat;if(clock_label)lv_label_set_text(clock_label,t&&*t?t:"--:--");
 if(status){
  const char *message="每一片新叶，都值得期待";
  if(handsfree_service_enabled()){
   const char *hint=handsfree_service_hint();
   /* Idle copy has one voice invitation, on the primary button. Keep the
    * live listening, processing, playback and error feedback visible. */
   if(hint&&*hint&&strcmp(hint,"说你好，小伴")!=0)message=hint;
  }
  if(!online)message="还没连上网络";
  if(strcmp(lv_label_get_text(status),message)!=0)lv_label_set_text(status,message);
 }
}
