/* 小绿: fixed, flash-font child UI. All LVGL access stays on ui_task.
 * The global voice service owns conversations; page deletion owns only its
 * objects/timer. Explicit Back/Stop cancels, a camera/page handoff does not.
 */
#include <nuttx/config.h>
#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "screen_voice.h"
#include "voice_face_model.h"
#include "../ui_app.h"
#include "../../services/power_service.h"
LV_FONT_DECLARE(lv_font_face_25);
LV_FONT_DECLARE(lv_font_face_16);
#define BG 0xFFF5DF
#define FG 0x244A38
#define EYE 0x2F6B45
struct face_page {
 struct face_page *next;
 lv_obj_t *root,*eye[2],*shine[2],*smile,*mouth,*status,*action,*action_text,*stop,*bars[7],*wifi,*battery,*battery_fill,*water;
 lv_timer_t *timer;
 int state,last_stop;
 bool paused;
 uint32_t blink_at,blink_end;
};
static struct face_page *s_pages;
static unsigned s_created,s_deleted,s_updates;
static int s_online=-1,s_battery=-1;
static uint32_t s_system_at;
static bool s_care;
static uint32_t s_care_at;
void screen_voice_set_care(bool care){s_care=care;s_care_at=lv_tick_get();}
static int s_preview=-1;
static uint32_t s_preview_until;
int screen_voice_preview_request(int state)
{
 struct voice_ui_snapshot v;voice_service_ui_snapshot(&v);
 if(state < -1 || state>FACE_SERVICE)return -22;
 if(v.busy || v.dialogue)return -16;
 __atomic_store_n(&s_preview_until,lv_tick_get()+8000,__ATOMIC_RELAXED);
 __atomic_store_n(&s_preview,state,__ATOMIC_RELEASE);return 0;
}
static lv_obj_t *shape(lv_obj_t *p,int x,int y,int w,int h,unsigned color,int radius)
{
 lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
 lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
 lv_obj_set_style_radius(o,radius,0);return o;
}
static lv_obj_t *label(lv_obj_t *p,int x,int y,int w,int h,const char *text,const lv_font_t *font)
{
 lv_obj_t *o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(FG),0);
 lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(o,text);return o;
}
static void hidden(lv_obj_t *o,bool yes)
{if(yes)lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);else lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN);}
static bool visible(struct face_page *p)
{return !lv_obj_has_flag(p->root,LV_OBJ_FLAG_HIDDEN) && !power_service_standby();}
static void on_back(lv_event_t *e)
{(void)e;voice_service_abort();ui_app_pop_screen();}
static void on_action(lv_event_t *e)
{
 (void)e;__atomic_store_n(&s_preview,-1,__ATOMIC_RELEASE);struct voice_ui_snapshot s;voice_service_ui_snapshot(&s);
 if(face_action_is_stop(&s))voice_service_abort();
 else {int rc=voice_service_dialogue_start();printf("[FaceUI] start rc=%d\n",rc);}
}
static void on_delete(lv_event_t *e)
{
 struct face_page *p=lv_event_get_user_data(e),**link=&s_pages;
 while(*link && *link!=p)link=&(*link)->next;
 if(*link)*link=p->next;
 if(p->timer)lv_timer_delete(p->timer);
 ++s_deleted;free(p);
}
static void update(lv_timer_t *timer)
{
 struct face_page *p=lv_timer_get_user_data(timer);
 if(!visible(p)){lv_timer_pause(timer);p->paused=true;return;}
 struct voice_ui_snapshot s;voice_service_ui_snapshot(&s);
 int state=face_state_from_voice(&s),stop=face_action_is_stop(&s);
 uint32_t now=lv_tick_get();++s_updates;
 if(state==FACE_IDLE && s_care && (uint32_t)(now-s_care_at)<5000)state=FACE_CARE;
 int preview=__atomic_load_n(&s_preview,__ATOMIC_ACQUIRE);
 if(s.busy || s.dialogue || (int32_t)(now-__atomic_load_n(&s_preview_until,__ATOMIC_RELAXED))>=0)
  {__atomic_store_n(&s_preview,-1,__ATOMIC_RELEASE);preview=-1;}
 if(preview>=0){state=preview;stop=state>=FACE_PREPARE && state<=FACE_STOP;}
 if(state!=p->state || stop!=p->last_stop){
  p->state=state;p->last_stop=stop;
  lv_label_set_text(p->status,face_caption(state));
  lv_label_set_text(p->action_text,stop?"停一下":"聊一聊");
  hidden(p->stop,!stop);
  bool error=state==FACE_RETRY || state==FACE_OFFLINE || state==FACE_MIC || state==FACE_SERVICE;
  hidden(p->smile,state==FACE_SPEAK||state==FACE_THINK||error);
  hidden(p->mouth,state!=FACE_SPEAK && state!=FACE_THINK && !error);
  hidden(p->water,state!=FACE_CARE);
  printf("[FaceUI] state=%d stop=%d gen=%u ready=%d playing=%d\n",state,stop,(unsigned)s.generation,s.capture_ready,s.playback_active);
 }
 if((int32_t)(now-p->blink_at)>=0){p->blink_end=now+160;p->blink_at=now+4800;}
 bool blink=(int32_t)(p->blink_end-now)>0;
 for(int i=0;i<2;i++){
  int height=blink?6:64;lv_obj_set_height(p->eye[i],height);
  lv_obj_set_y(p->eye[i],blink?97:68);hidden(p->shine[i],blink);
  int shift=state==FACE_THINK?((now%1800)<900?4:-4):0;
  lv_obj_set_x(p->shine[i],9+shift);
 }
 if(state==FACE_SPEAK){int h=8+(now/180%3)*3;lv_obj_set_size(p->mouth,14,h);lv_obj_set_pos(p->mouth,153,136);lv_obj_set_style_radius(p->mouth,LV_RADIUS_CIRCLE,0);}
 else if(state==FACE_THINK){lv_obj_set_size(p->mouth,18,3);lv_obj_set_pos(p->mouth,151,140);}
 else {lv_obj_set_size(p->mouth,10,10);lv_obj_set_pos(p->mouth,155,136);}
 for(int i=0;i<7;i++){
  hidden(p->bars[i],state!=FACE_LISTEN);
  if(state==FACE_LISTEN){int h=4+((now/160+i*3)%5)*2;lv_obj_set_height(p->bars[i],h);lv_obj_set_y(p->bars[i],18-h/2);}
 }
 bool fresh=(uint32_t)(now-s_system_at)<5000;
 hidden(p->wifi,!fresh || s_online<0);hidden(p->battery,!fresh || s_battery<0);
 if(fresh && s_online>=0)lv_obj_set_style_opa(p->wifi,s_online?LV_OPA_COVER:LV_OPA_40,0);
 if(fresh && s_battery>=0){lv_obj_set_width(p->battery_fill,1+16*s_battery/100);}
}
void screen_voice_visibility_tick(void)
{
 for(struct face_page *p=s_pages;p;p=p->next){
  bool show=visible(p);
  if(show && p->paused){p->paused=false;lv_timer_resume(p->timer);lv_timer_ready(p->timer);}
  else if(!show && !p->paused){p->paused=true;lv_timer_pause(p->timer);}
 }
}
void screen_voice_set_system(int online,int battery)
{s_online=online;s_battery=battery;s_system_at=lv_tick_get();}
void screen_voice_audit(void)
{
 unsigned live=0,active=0;for(struct face_page *p=s_pages;p;p=p->next){live++;if(visible(p))active++;}
 printf("[FaceAudit] created=%u deleted=%u live=%u active=%u timers=%u updates=%u hit=16,192,288,48 font=25/16 flash=1\n",s_created,s_deleted,live,active,live,s_updates);
}
/* Text stays complete in the voice/cloud pipeline. The face has one fixed
 * status caption; neither a transcript copy nor a new speech path lives here. */
static void state_cb(void *u,int state){(void)u;(void)state;}
static void text_cb(void *u,const char *text,bool user){(void)u;(void)text;(void)user;}
int screen_voice_prepare_service(void)
{voice_service_set_text_update_cb(text_cb);return voice_service_start(state_cb,text_cb,NULL);}
void screen_voice_add_user_msg(lv_obj_t *scr,const char *text){(void)scr;(void)text;}
void screen_voice_add_ai_reply(lv_obj_t *scr,const char *text){(void)scr;(void)text;}
bool screen_voice_is_active(void)
{for(struct face_page *p=s_pages;p;p=p->next)if(visible(p))return true;return false;}
lv_obj_t *screen_voice_create(void)
{
 struct face_page *p=calloc(1,sizeof(*p));if(!p)return NULL;
 p->root=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(p->root);
 lv_obj_set_size(p->root,320,240);lv_obj_remove_flag(p->root,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_style_bg_color(p->root,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(p->root,LV_OPA_COVER,0);
 lv_obj_t *back=shape(p->root,0,0,48,44,BG,0);lv_obj_add_flag(back,LV_OBJ_FLAG_CLICKABLE);
 label(back,0,10,48,30,LV_SYMBOL_LEFT,&lv_font_montserrat_20);lv_obj_add_event_cb(back,on_back,LV_EVENT_CLICKED,NULL);
 lv_obj_t *brand=label(p->root,52,10,80,24,"小绿",&lv_font_face_16);lv_obj_set_style_text_align(brand,LV_TEXT_ALIGN_LEFT,0);
 lv_obj_t *leaf=shape(p->root,155,32,29,17,0x75B843,LV_RADIUS_CIRCLE);
 lv_obj_set_style_transform_rotation(leaf,-250,0);
 shape(leaf,4,8,21,2,0x315B36,1);
 for(int i=0;i<2;i++){
  p->eye[i]=shape(p->root,72+i*128,68,48,64,EYE,LV_RADIUS_CIRCLE);
  p->shine[i]=shape(p->eye[i],9,11,13,15,BG,LV_RADIUS_CIRCLE);
  lv_obj_t *cheek=shape(p->root,54+i*186,136,26,6,0xE99B7B,3);lv_obj_set_style_bg_opa(cheek,LV_OPA_70,0);
 }
 p->smile=lv_arc_create(p->root);lv_obj_remove_style_all(p->smile);
 lv_obj_set_pos(p->smile,144,115);lv_obj_set_size(p->smile,32,32);
 lv_obj_remove_flag(p->smile,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_style_arc_color(p->smile,lv_color_hex(FG),LV_PART_MAIN);
 lv_obj_set_style_arc_width(p->smile,2,LV_PART_MAIN);lv_obj_set_style_arc_opa(p->smile,LV_OPA_COVER,LV_PART_MAIN);
 lv_arc_set_bg_angles(p->smile,20,160);lv_arc_set_angles(p->smile,20,20);
 p->mouth=shape(p->root,153,136,14,10,FG,LV_RADIUS_CIRCLE);hidden(p->mouth,true);
 p->water=label(p->root,266,54,22,26,LV_SYMBOL_TINT,&lv_font_montserrat_20);
 lv_obj_set_style_text_color(p->water,lv_color_hex(0x236B8E),0);hidden(p->water,true);
 p->status=label(p->root,12,158,296,32,"嗨，我是小绿",&lv_font_face_25);
 p->action=shape(p->root,16,192,288,48,BG,0);lv_obj_add_flag(p->action,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *pill=shape(p->action,64,6,160,34,0xFFD166,17);
 lv_obj_set_style_border_width(pill,2,0);lv_obj_set_style_border_color(pill,lv_color_hex(FG),0);
 p->action_text=label(pill,36,5,98,24,"聊一聊",&lv_font_face_16);
 p->stop=shape(pill,19,12,10,10,FG,2);hidden(p->stop,true);
 for(int i=0;i<7;i++){p->bars[i]=shape(pill,133+i*3,12,2,6,EYE,1);hidden(p->bars[i],true);}
 lv_obj_add_event_cb(p->action,on_action,LV_EVENT_CLICKED,p);
 p->wifi=label(p->root,252,10,24,24,LV_SYMBOL_WIFI,&lv_font_montserrat_20);
 p->battery=shape(p->root,284,14,22,12,BG,2);lv_obj_set_style_border_width(p->battery,1,0);lv_obj_set_style_border_color(p->battery,lv_color_hex(FG),0);
 p->battery_fill=shape(p->battery,2,2,16,6,EYE,1);shape(p->battery,20,3,2,6,FG,1);
 hidden(p->wifi,true);hidden(p->battery,true);
 p->state=-1;p->last_stop=-1;p->blink_at=lv_tick_get()+4800;
 p->timer=lv_timer_create(update,80,p);if(!p->timer){lv_obj_delete(p->root);free(p);return NULL;}p->next=s_pages;s_pages=p;++s_created;
 lv_obj_add_event_cb(p->root,on_delete,LV_EVENT_DELETE,p);screen_voice_prepare_service();
 return p->root;
}
