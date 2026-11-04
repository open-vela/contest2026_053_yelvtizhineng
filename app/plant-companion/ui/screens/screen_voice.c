#include <nuttx/config.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "screen_voice.h"
#include "voice_scene.h"
#include "../ui_app.h"
#include "../widgets/companion_ui.h"
#include "../../services/voice_service.h"
#include "../../services/handsfree_service.h"
#include "../../services/ai_service.h"

extern const lv_image_dsc_t *const cu_voice_frames[6];
typedef struct {
 lv_timer_t *timer;
 lv_obj_t *title,*hint,*button,*mascot,*bars[7];
 uint32_t scene_start;
 int previous,frame;
 bool cancelling,leaving;
} voice_page;

/* Maintenance reads counters, never LVGL objects or page pointers. There
 * is no expression fixture; transitions come from the real audio services. */
static _Atomic int debug_active,debug_scene,debug_frame;
static _Atomic unsigned debug_ticks,debug_changes,debug_scenes;
static _Atomic unsigned debug_seen[6];
static const char *const scene_names[]={"idle","listen","think","speak"};
void screen_voice_status(void)
{
 int scene=atomic_load(&debug_scene);
 printf("[VoiceUI] active=%d scene=%s frame=%d ticks=%u frames=%u transitions=%u seen=%u,%u,%u,%u,%u,%u\n",
  atomic_load(&debug_active),scene_names[scene],atomic_load(&debug_frame),
  atomic_load(&debug_ticks),atomic_load(&debug_changes),atomic_load(&debug_scenes),
  atomic_load(&debug_seen[0]),atomic_load(&debug_seen[1]),atomic_load(&debug_seen[2]),
  atomic_load(&debug_seen[3]),atomic_load(&debug_seen[4]),atomic_load(&debug_seen[5]));
}
static void text_if_changed(lv_obj_t *label,const char *text)
{if(strcmp(lv_label_get_text(label),text)!=0)lv_label_set_text(label,text);}
static void back(lv_event_t *e)
{
 voice_page *p=lv_event_get_user_data(e);p->leaving=true;
 voice_service_abort();ui_app_pop_screen();
}
static void clicked(lv_event_t *e)
{
 voice_page *p=lv_event_get_user_data(e);
 if(!voice_service_busy()){
  if(ai_service_busy()||handsfree_service_processing())return;
  int ret=voice_service_start(NULL,NULL,NULL);
  if(!ret)ret=voice_service_talk(5);
  if(ret<0)text_if_changed(p->hint,"暂时没成功，请再试一次");
  p->cancelling=false;
 }else if(voice_service_get_state()==VOICE_STATE_LISTENING){
  voice_service_talk(5); /* Finish without discarding the recording. */
 }else{
  voice_service_abort();p->cancelling=true;
 }
}
static void refresh(voice_page *p)
{
 bool busy=voice_service_busy();int state=voice_service_get_state();
 bool asr=handsfree_service_processing(),image_busy=ai_service_busy();
 enum voice_scene scene=voice_scene_choose(busy,state,p->cancelling,asr,
  handsfree_service_hearing(),handsfree_service_interacting(),
  handsfree_service_enabled()!=0,image_busy);
 uint32_t now=lv_tick_get();
 if(!busy)p->cancelling=false;
 if(p->previous!=(int)scene){
  p->previous=scene;p->scene_start=now;
  atomic_store(&debug_scene,scene);atomic_fetch_add(&debug_scenes,1);
  printf("[VoiceUI] transition=%s foreground=%d asr=%d\n",scene_names[scene],busy,asr);
 }
 uint32_t elapsed=now-p->scene_start;
 const char *title="小伴在这里",*hint="点一下，聊聊新发现",*button="开始说话";
 bool disabled=!busy&&(asr||image_busy);
 if(scene==SCENE_LISTEN){
  title="我在听呢";
  hint=busy?"慢慢说，不着急":"说出你想问的问题";
  if(busy)button="说好啦";
 }else if(scene==SCENE_THINK){
  title="让我想一想";
  hint=asr?"正在听懂你的话":"很快就来回答你";
  button=busy?"取消":"请稍等";
  if(image_busy&&!busy){title="正在看照片";hint="看清楚，再告诉你";}
 }else if(scene==SCENE_SPEAK){
  title="小伴说给你听";hint="点下面可以停一下";button="停一下";
 }
 if(p->cancelling&&busy){title="正在停下来";hint="等一下就好";button="取消";}
 if(!busy&&voice_service_last_error()<0&&!asr){
  hint=voice_service_last_error()==-ENOMSG?"没听清，请再说一次":"暂时没成功，请再试一次";
 }
 text_if_changed(p->title,title);text_if_changed(p->hint,hint);
 text_if_changed(lv_obj_get_child(p->button,0),button);
 if(disabled)lv_obj_add_state(p->button,LV_STATE_DISABLED);
 else lv_obj_remove_state(p->button,LV_STATE_DISABLED);
 int frame=voice_scene_frame(scene,elapsed);
 if(frame!=p->frame){
  lv_image_set_src(p->mascot,cu_voice_frames[frame]);p->frame=frame;
  atomic_store(&debug_frame,frame);atomic_fetch_add(&debug_changes,1);
  atomic_fetch_add(&debug_seen[frame],1);
 }
 /* Gentle breathing and wave motion. No file reads, HTTP requests, or
  * per-tick image allocations in the presentation timer. */
 lv_obj_set_y(p->mascot,59+((elapsed/500)%4==1?1:0));
 static const uint8_t wave[8]={8,14,23,30,22,13,7,11};
 for(int i=0;i<7;i++){
  bool moving=scene==SCENE_LISTEN||scene==SCENE_SPEAK;
  int height=moving?wave[(elapsed/140+i)%8]:5;
  bool show=scene!=SCENE_THINK||(i>=2&&i<=4);
  if(show)lv_obj_remove_flag(p->bars[i],LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(p->bars[i],LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_height(p->bars[i],height);lv_obj_set_y(p->bars[i],113-height/2);
  lv_obj_set_style_bg_opa(p->bars[i],scene==SCENE_THINK&&i!=(int)(2+elapsed/300%3)?LV_OPA_30:scene==SCENE_IDLE?LV_OPA_30:LV_OPA_COVER,0);
 }
 atomic_fetch_add(&debug_ticks,1);
}
static void poll(lv_timer_t *timer){refresh(lv_timer_get_user_data(timer));}
static void deleted(lv_event_t *e)
{
 voice_page *p=lv_event_get_user_data(e);
 if(p->timer)lv_timer_delete(p->timer);
 lv_obj_delete(p->mascot);
 atomic_store(&debug_active,0);
 if(!p->leaving)voice_service_abort();lv_free(p);
}
static lv_obj_t *decoration(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int radius)
{
 lv_obj_t *o=cu_panel(parent,x,y,w,h,color,radius);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);return o;
}
lv_obj_t *screen_voice_create(void)
{
 lv_obj_t *root=cu_root();voice_page *p=lv_malloc(sizeof(*p));if(!p)return root;
 *p=(voice_page){.previous=-1,.frame=-1};
 lv_obj_add_event_cb(root,deleted,LV_EVENT_DELETE,p);
 lv_obj_set_style_bg_color(root,lv_color_hex(0xfbfaf2),0);
 cu_button(root,12,6,64,44,"返回",16,0xe4edda,back,p);
 cu_label(root,163,13,190,"和小伴说话",22,CU_INK);
 decoration(root,23,58,190,171,0xeef2df,80);
 decoration(root,48,218,145,12,0xd9e6c2,60);
 p->mascot=lv_image_create(root);
 lv_obj_remove_flag(p->mascot,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_pos(p->mascot,38,59);lv_image_set_src(p->mascot,cu_voice_frames[0]);
 lv_obj_t *bubble=decoration(root,232,70,230,150,0xfffdf8,24);
 p->title=cu_label(bubble,14,17,204,"小伴在这里",30,CU_INK);
 p->hint=cu_label(bubble,14,64,204,"点一下，聊聊新发现",16,0x71806b);
 for(int i=0;i<7;i++)p->bars[i]=decoration(bubble,34+i*24,109,10,5,0x8fbd64,5);
 p->button=cu_button(root,20,240,440,64,"开始说话",30,CU_GREEN,clicked,p);
 lv_obj_set_style_bg_opa(p->button,LV_OPA_50,LV_STATE_DISABLED);
 cu_image(p->button,&cu_mic,124,16);
 lv_obj_align(lv_obj_get_child(p->button,0),LV_ALIGN_TOP_LEFT,172,14);
 lv_obj_set_width(lv_obj_get_child(p->button,0),180);
 atomic_store(&debug_active,1);atomic_store(&debug_ticks,0);
 atomic_store(&debug_changes,0);atomic_store(&debug_scenes,0);
 for(int i=0;i<6;i++)atomic_store(&debug_seen[i],0);
 p->timer=lv_timer_create(poll,100,p);refresh(p);
 return root;
}
