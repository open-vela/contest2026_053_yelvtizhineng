#include <nuttx/config.h>
#include <lvgl/lvgl.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ui_app.h"
#include "../services/power_service.h"
#include "screens/screen_camera.h"
#include "screens/screen_voice.h"
#include "screens/screen_wifi.h"
#include "screens/screen_data.h"
#include "screens/screen_diagnose.h"
extern int screen_diary_voice_step(int delta);
#include "widgets/companion_ui.h"
#include "../services/handsfree_service.h"
#ifdef CONFIG_PLANT_WIFI_MANAGER
#include "../communication/wifi_manager/wifi_manager.h"
#endif
#include "../services/voice_service.h"
#include "../services/ai_service.h"
#include "../services/record_service.h"
#include "../services/sensor_presentation.h"
#include "../components/voice_agent/voice_agent.h"
#include "../components/system_monitor/user_preferences.h"
#include "../components/camera_capture/camera_capture.h"
#include "../ota/ota.h"

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done=PTHREAD_COND_INITIALIZER;
struct action_buffers {struct hf_command command;char reply[401];};
static struct action_buffers *buffers;
#define request (buffers->command)
static bool pending,processing,completed,abandoned;
#define response (buffers->reply)
static int result;
static uint32_t photo_due;
static int photo_tries,diary_index;
static bool photo_wait_prompt;
static int restore_volume=198;

static uint32_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)(t.tv_sec*1000u+t.tv_nsec/1000000u);}
int ui_app_voice_action(const struct hf_command *c,char *reply,size_t size)
{
 struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=10;
 pthread_mutex_lock(&lock);
 if(!buffers){buffers=calloc(1,sizeof(*buffers));if(!buffers){pthread_mutex_unlock(&lock);return -ENOMEM;}}
 if(pending||processing){pthread_mutex_unlock(&lock);return -EBUSY;}
 request=*c;pending=true;completed=false;abandoned=false;
 while(!completed){
  int r=pthread_cond_timedwait(&done,&lock,&deadline);
  if(r==ETIMEDOUT){pending=false;abandoned=true;pthread_mutex_unlock(&lock);return -ETIMEDOUT;}
 }
 snprintf(reply,size,"%s",response);int ret=result;
 pthread_mutex_unlock(&lock);return ret;
}
static void sensor_snapshot(struct sensor_presentation_s *p)
{
 struct sensor_view_s view;sensor_service_get_view(&view);
 sensor_present(&view,now_ms(),ui_app_unix_time(),p);
}
static void all_spoken_readings(const struct sensor_presentation_s *p,char *reply,size_t cap)
{
 size_t used=(size_t)snprintf(reply,cap,"%s。",sensor_freshness_name(p));
 for(int i=0;i<8 && used<cap;i++){
  char value[56];sensor_metric_format(p,i,value,sizeof(value));
  int n=(p->fields&(1u<<i))?
   snprintf(reply+used,cap-used,"%s%s%s。",sensor_metric_name(i),value,sensor_metric_unit(i)):
   snprintf(reply+used,cap-used,"%s还没有读数。",sensor_metric_name(i));
  if(n<0||(size_t)n>=cap-used)break;
  used+=(size_t)n;
 }
}
static int execute(const struct hf_command *c,char *reply,size_t cap)
{
 struct sensor_presentation_s p;char value[56];
 if((photo_due||screen_camera_voice_busy())&&c->action!=HF_CAMERA_CLOSE&&c->action!=HF_STOP&&c->action!=HF_SLEEP&&c->action!=HF_HELP&&c->action!=HF_VOLUME&&c->action!=HF_VOLUME_SET&&c->action!=HF_MUTE&&c->action!=HF_UNMUTE){
  snprintf(reply,cap,"正在拍照，请等我拍好后再切换页面。");return 1;
 }
 /* A queued shutter is a live job. Chatter/help/volume cannot cancel it;
  * only an explicit stop, sleep or camera close can do so. */
 if(c->action==HF_STOP||c->action==HF_SLEEP||c->action==HF_CAMERA_CLOSE)photo_due=0;
 switch(c->action){
 case HF_SCREEN_OFF:box_power_request(BOX_POWER_OFF);snprintf(reply,cap,"屏幕关掉啦，说你好小伴还能叫我。");break;
 case HF_POWER_ECO:box_power_request(BOX_POWER_ECO);snprintf(reply,cap,"省电模式开启啦，我还会听你叫我。");break;
 case HF_POWER_NORMAL:box_power_request(BOX_POWER_NORMAL);snprintf(reply,cap,"已经恢复正常模式。");break;
 case HF_KEEP_SCREEN:box_power_request(BOX_POWER_HOLD);snprintf(reply,cap,"屏幕会保持亮着五分钟。");break;
 case HF_DEEP_REST:box_power_request(BOX_POWER_OFF);snprintf(reply,cap,"现在先熄屏省电，我还会听你叫我。深度休眠暂时没有开启。");break;
 case HF_HOME:ui_app_voice_root(PLANT_TAB_HOME);snprintf(reply,cap,"回到首页啦。你还想做什么？");break;
 case HF_TALK:ui_app_voice_open_chat();snprintf(reply,cap,"我在听，你想问什么？");break;
 case HF_DATA:case HF_ALL:case HF_METRIC:
  ui_app_voice_root(PLANT_TAB_DATA);sensor_snapshot(&p);
  if(c->action==HF_METRIC){
   screen_data_voice_view(1,c->value);sensor_metric_format(&p,c->value,value,sizeof(value));
   if(p.fields&(1u<<c->value))snprintf(reply,cap,"%s是%s%s。%s。%s",sensor_metric_name(c->value),value,sensor_metric_unit(c->value),sensor_freshness_name(&p),sensor_metric_explanation(c->value));
   else snprintf(reply,cap,"这一项还没有有效读数，等传感器更新后再问我吧。");
  }else if(c->action==HF_ALL){screen_data_voice_view(2,0);all_spoken_readings(&p,reply,cap);}
  else sensor_spoken_summary(&p,reply,cap);
  break;
 case HF_CAMERA:case HF_PHOTO:
  {
   if(ai_service_busy()){snprintf(reply,cap,"上一张照片还在识别，请稍等。");break;}
   bool camera_open=screen_camera_voice_visible();
   if(!camera_open){ui_app_voice_root(PLANT_TAB_HOME);ui_app_push_screen(screen_camera_create());}
   if(c->action==HF_PHOTO){
    photo_tries=0;
    /* An already aligned view needs neither a sensor restart nor a
     * spoken preparation prompt. LVGL gives immediate shutter feedback. */
    photo_wait_prompt=!camera_open;
    photo_due=now_ms()+(camera_open?100:2500);
    if(camera_open)reply[0]='\0';
    else snprintf(reply,cap,"请把植物放在镜头前，我来拍照。");
   }else snprintf(reply,cap,"相机打开啦。对准植物后，说帮我拍照就好。");
  }
  break;
 case HF_SAVE:
  {int saved=screen_diagnose_voice_save();
   if(saved==0)snprintf(reply,cap,"已记到成长日记。可以说看看日记。");
   else if(saved==-2)snprintf(reply,cap,"这次没能保存，请再说一次保存到日记。");
   else snprintf(reply,cap,"先拍照识别植物，再说保存到日记吧。");}
  break;
 case HF_REPEAT:
  voice_service_copy_spoken(reply,cap);
  if(!reply[0])snprintf(reply,cap,"你可以先说看看状态，或者问我植物问题。 ");
  break;
 case HF_RESULT:
  if(screen_diagnose_voice_result(reply,cap)<0)snprintf(reply,cap,"现在还没有识别结果，可以说帮我拍照。");
  break;
 case HF_TASKS:case HF_TASK_DONE:case HF_TASK_UNDONE:{
  if(c->action!=HF_TASKS){
   struct record_task_s t;
   if(!record_task_get(c->value,&t)){snprintf(reply,cap,"没有找到这个任务。可以说看看任务。");break;}
   bool target=c->action==HF_TASK_DONE;
   if(t.done!=target){if(record_task_set_done(c->value,target)<0){snprintf(reply,cap,"任务没能保存，请再试一次。");break;}if(target)record_task_on_completed();}
   snprintf(reply,cap,target?"第%d个任务已完成，做得好！":"第%d个任务已恢复为未完成。",c->value+1);
  }else{
   size_t used=(size_t)snprintf(reply,cap,"今天完成了%d个任务。",record_task_done_count());
   for(int i=0;i<record_task_get_count()&&used+65<cap;i++){
    struct record_task_s t;if(record_task_get(i,&t))used+=(size_t)snprintf(reply+used,cap-used,"第%d个，%s，%s。",i+1,t.title,t.done?"已完成":"还没完成");
   }
  }
  ui_app_voice_root(PLANT_TAB_TASKS);break;
 }
 case HF_DIARY:
  diary_index=0;ui_app_voice_root(PLANT_TAB_DIARY);
  {struct record_diary_s d;if(record_diary_get(0,&d))snprintf(reply,cap,"最新的成长日记是：%s。%s。说下一页可以继续听。",d.title,d.detail);
   else snprintf(reply,cap,"还没有成长日记。拍照后说保存到日记，就能记录啦。");}break;
 case HF_NEXT:case HF_PREVIOUS:{
  ui_app_voice_close_chat(true);
  int delta=c->action==HF_NEXT?1:-1;
  if(screen_data_voice_step(delta,reply,cap)==0)break;
  if(screen_diary_voice_step(delta)==0){
   diary_index+=delta;if(diary_index<0)diary_index=0;
   int count=record_diary_get_count();if(diary_index>=count)diary_index=count>0?count-1:0;
   printf("[DiaryVoice] index=%d count=%d\n",diary_index,count);
   struct record_diary_s d;if(record_diary_get(diary_index,&d))snprintf(reply,cap,"第%d条日记，%s。%s。",diary_index+1,d.title,d.detail);
   else snprintf(reply,cap,"还没有日记。");
  }else snprintf(reply,cap,"这一页不用翻页。可以说看看数据，或者看看日记。");
  break;
 }
 case HF_CAMERA_CLOSE:ui_app_voice_root(PLANT_TAB_HOME);snprintf(reply,cap,"相机已关闭。你还想做什么？");break;
 case HF_BACK:
  if(ui_app_voice_close_chat(false))snprintf(reply,cap,"已经返回。也可以说回首页。");
  else if(screen_data_voice_back()==0)snprintf(reply,cap,"回到全部数据啦。");
  else {camera_preview_take();ui_app_voice_back_to_parent();snprintf(reply,cap,"已经返回。也可以说回首页。");}
  break;
 case HF_VOLUME:case HF_VOLUME_SET:case HF_MUTE:case HF_UNMUTE:{
  int v=voice_agent_get_dac_volume();
  if(c->action==HF_MUTE){if(v)restore_volume=v;v=0;}
  else if(c->action==HF_UNMUTE)v=restore_volume;
  else if(c->action==HF_VOLUME_SET)v=c->value?150+c->value*60/100:0;
  else v+=c->value;
  if(v<0)v=0;if(v>210)v=210;
  if(voice_agent_set_dac_volume(v)<0){snprintf(reply,cap,"声音没调成功，请再试一次。");break;}
  if(v)restore_volume=v;
  int saved=plant_pref_save_int(PLANT_PREF_VOLUME_PATH,v);
  snprintf(reply,cap,v?"声音调好啦。%s":"已经静音。",saved<0?"这次没能保存设置。":"");break;
 }
 case HF_STOP:if(screen_camera_voice_busy())ui_app_voice_root(PLANT_TAB_HOME);if(!c->value)voice_service_abort();snprintf(reply,cap,"停下来啦。");break;
 case HF_SLEEP:ui_app_voice_root(PLANT_TAB_HOME);snprintf(reply,cap,"好呀，需要我时再叫我。");break;
 case HF_HELP:
  if(c->value==1)snprintf(reply,cap,"这台传感器没有钼元素的读数。可以说，请查看土壤中磷元素的数值，或者听全部读数。");
  else if(c->value==2)snprintf(reply,cap,"我还没找到这项读数。请说完整一些，比如，请查看土壤中磷元素的数值。也可以说，全部读数。");
  else snprintf(reply,cap,"你可以说，看看小绿的状态，湿度多少，帮我拍照，保存到日记，看看任务，完成第一个任务，下一页，返回，回首页，声音小一点。也可以直接问我植物问题。");break;
 case HF_NETWORK:
  ui_app_voice_root(PLANT_TAB_HOME);ui_app_push_screen(screen_wifi_create());
#ifdef CONFIG_PLANT_WIFI_MANAGER
  snprintf(reply,cap,wifi_manager_link_up()&&wifi_manager_has_ip()?
    "无线网络已经连上啦。更换网络和输入密码，请大人帮忙。":
    "无线网络还没连接好，请稍等一会儿，或者请大人帮忙。");
#else
  snprintf(reply,cap,"更换网络和输入密码，请大人帮忙。");
#endif
  break;
 case HF_INFO:{
  ui_app_voice_root(PLANT_TAB_HOME);lv_obj_t *r=cu_root();
  cu_button(r,12,6,60,48,"返回",16,0xe4edda,cu_back,NULL);
  cu_label(r,95,18,300,"设备信息",22,CU_INK);
  snprintf(reply,cap,"我是植小伴BOX，固件版本%d.%d.%d。可以说返回。",OTA_FW_VERSION_MAJOR,OTA_FW_VERSION_MINOR,OTA_FW_VERSION_PATCH);
  cu_label(r,24,100,430,reply,22,CU_INK);ui_app_push_screen(r);break;
 }
 case HF_WATER:snprintf(reply,cap,"这台BOX还没有接上真实水泵，暂时不能自动浇水。可以请大人帮小绿检查土壤。");break;
 default:return -EINVAL;
 }
 printf("[HFUI] action=%s value=%d\n",hf_action_name(c->action),c->value);
 return 0;
}
bool handsfree_ui_busy(void){pthread_mutex_lock(&lock);bool b=pending||processing;pthread_mutex_unlock(&lock);return b||photo_due!=0;}
void handsfree_ui_poll(void)
{
 screen_camera_voice_speech_pause(handsfree_service_hearing()||voice_service_busy());
 struct hf_command c;bool run=false;
 pthread_mutex_lock(&lock);if(pending){c=request;pending=false;processing=true;run=true;}pthread_mutex_unlock(&lock);
 if(run){
  char text[401]={0};int ret=execute(&c,text,sizeof(text));
  /* Complete old-page destruction before releasing the waiting speech
   * worker. A voice page's delete callback cancels its prior interaction;
   * it must run before the next feedback notice can start. This poll is
   * owned by the LVGL timer, outside any touch event callback. */
  ui_app_voice_finish_navigation();
  pthread_mutex_lock(&lock);processing=false;
  if(!abandoned){snprintf(response,sizeof(response),"%s",text);result=ret;completed=true;pthread_cond_signal(&done);}
  pthread_mutex_unlock(&lock);
 }
 if(photo_due&&(int32_t)(now_ms()-photo_due)>=0){
  if(photo_wait_prompt){
   if(voice_service_busy()){photo_due=now_ms()+250;return;}
   photo_wait_prompt=false;photo_due=now_ms()+150;return;
  }
  int ret=screen_camera_voice_shoot();
  if(ret==-EAGAIN&&++photo_tries<12)photo_due=now_ms()+500;
  else {photo_due=0;if(ret<0)handsfree_service_announce("暂时没拍成功，请对准植物，再说拍照。");}
 }
}
