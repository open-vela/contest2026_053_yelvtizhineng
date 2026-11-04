/* Coordinated idle power management. UI objects and mode changes are owned
 * by ui_task. No new motion/probe is ever issued on wake. */
#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <lvgl/lvgl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "power_service.h"
#include "power_policy.h"
#include "voice_service.h"
#include "../ui/ui_app.h"
#include "../ui/theme/companion_style.h"
#include "../ui/screens/screen_camera.h"
#include "../components/camera_capture/camera_capture.h"
#include "../components/voice_agent/voice_agent.h"
#include "../components/voice_wake/voice_wake.h"
#include "../components/stackchan_body/stackchan_body.h"
#include "../components/system_monitor/battery_monitor.h"
extern int board_cores3_backlight_set(int percent);
extern int board_cores3_pmic_read_reg(uint8_t reg,uint8_t *value);
extern int board_cores3_power_off(void);
static _Atomic unsigned g_activity,g_mode,g_request;
static _Atomic bool g_started,g_enabled=true,g_demo,g_background=true;
static _Atomic unsigned g_dim=60000,g_standby=180000,g_deep=900000;
static unsigned g_seen,g_idle_since,g_source_at,g_low_count,g_last_error;
static bool g_source_valid,g_external=true,g_deep_failed;
static int g_last_rc;
static lv_obj_t *g_cover,*g_menu;
enum { REQ_NONE,REQ_WAKE,REQ_STANDBY,REQ_DEEP,REQ_MENU,REQ_CLOSE,REQ_DEMO,REQ_NORMAL };
static unsigned now_ms(void){return TICK2MSEC(clock_systime_ticks());}
static const char *mode_name(unsigned m)
{return m==POWER_DIM?"dim":m==POWER_STANDBY?"standby":m==POWER_DEEP?"deep":"active";}
/* This file owns only the demo preference. Never rewrite Wi-Fi credentials
 * or an unrelated pre-existing file, and never persist accelerated tests. */
static int save_demo(void)
{
 char line[32]={0};FILE *f=fopen("/mnt/sd/POWER.CFG","r");
 if(f){bool ours=fgets(line,sizeof(line),f)&&!strncmp(line,"PLANTPOWER1 ",12);fclose(f);if(!ours)return -EEXIST;}
 int fd=open("/mnt/sd/PWRNEW.CFG",O_WRONLY|O_CREAT|O_EXCL,0600);
 if(fd<0)return -errno;
 int n=snprintf(line,sizeof(line),"PLANTPOWER1 %d\n",g_demo?1:0);
 int rc=write(fd,line,n)==n?0:-EIO;
 if(close(fd)<0)rc=-EIO;
 if(rc==0&&rename("/mnt/sd/PWRNEW.CFG","/mnt/sd/POWER.CFG")<0)rc=-errno;
 if(rc<0)unlink("/mnt/sd/PWRNEW.CFG");return rc;
}
void power_service_activity(void){atomic_fetch_add(&g_activity,1);}
bool power_service_standby(void){return g_started&&g_mode>=POWER_STANDBY;}
bool power_service_background_allowed(void){return !g_started||g_background;}
void power_service_menu(void){g_request=REQ_MENU;}
static void click(lv_event_t *e)
{g_request=(unsigned)(uintptr_t)lv_event_get_user_data(e);}
static void cover_click(lv_event_t *e){(void)e;power_service_activity();}
static void delete_menu(void){if(g_menu){lv_obj_delete(g_menu);g_menu=NULL;}}
static void make_button(lv_obj_t *parent,const char *text,int x,int y,unsigned req)
{pal_button(parent,x,y,138,60,text,click,(void *)(uintptr_t)req);}
static void show_menu(void)
{
 delete_menu();g_menu=lv_obj_create(lv_layer_top());lv_obj_remove_style_all(g_menu);
 lv_obj_set_size(g_menu,300,232);lv_obj_center(g_menu);lv_obj_remove_flag(g_menu,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_style_bg_color(g_menu,lv_color_hex(PAL_BG),0);lv_obj_set_style_bg_opa(g_menu,LV_OPA_COVER,0);
 lv_obj_set_style_radius(g_menu,22,0);lv_obj_set_style_border_width(g_menu,2,0);
 lv_obj_set_style_border_color(g_menu,lv_color_hex(PAL_FG),0);
 pal_text(g_menu,12,8,240,27,"小伴休息一下",TP_FONT_TITLE);pal_face(g_menu,251,3,36,30);
 make_button(g_menu,"休息一下\n声音和触摸唤醒",8,42,REQ_STANDBY);
 make_button(g_menu,"深度休眠\n电源键恢复",150,42,REQ_DEEP);
 make_button(g_menu,g_demo?"结束演示\n恢复自动休眠":"演示模式\n保持亮屏",8,106,g_demo?REQ_NORMAL:REQ_DEMO);
 make_button(g_menu,"返回小伴",150,106,REQ_CLOSE);
 /* Keep the hint last: USB deep-sleep refusal updates this existing label. */
 pal_text(g_menu,12,175,272,43,"1分钟暗屏 / 3分钟休息\n电池15分钟深眠",TP_FONT_CAPTION);
}
static int set_mode(unsigned mode)
{
 unsigned old=g_mode;int rc=0;
 if(old==mode)return 0;
 if(mode>=POWER_STANDBY&&old<POWER_STANDBY)
  {
   /* Close camera through its UI lifecycle before powering it down. The
    * overlay consumes the wake tap, so underlying controls never fire. */
   if(screen_camera_is_active())ui_app_business_page("home");
   rc=camera_power_idle();if(rc<0)goto fail;
   rc=voice_agent_speaker_standby(true);if(rc<0)goto fail;
   if((rc=stackchan_body_idle_power())<0)goto fail;
   g_cover=lv_button_create(lv_layer_top());lv_obj_remove_style_all(g_cover);
   lv_obj_set_size(g_cover,LV_PCT(100),LV_PCT(100));
   lv_obj_set_style_bg_color(g_cover,lv_color_black(),0);lv_obj_set_style_bg_opa(g_cover,LV_OPA_COVER,0);
   lv_obj_add_event_cb(g_cover,cover_click,LV_EVENT_CLICKED,NULL);
   g_background=false;
  }
 if(mode<POWER_STANDBY&&old>=POWER_STANDBY)
  {
   rc=voice_agent_speaker_standby(false);if(rc<0)goto fail;
   g_background=true;stackchan_body_idle_wake();
   if(g_cover){lv_obj_delete(g_cover);g_cover=NULL;}
  }
 rc=board_cores3_backlight_set(mode>=POWER_STANDBY?0:mode==POWER_DIM?20:100);
 if(rc<0)goto fail;
 g_mode=mode;g_last_rc=0;
 printf("[Power] mode=%s previous=%s backlight=%u background=%d motor_auto_restore=0\n",mode_name(mode),mode_name(old),mode>=POWER_STANDBY?0:mode==POWER_DIM?20:100,g_background);
 return 0;
fail:
 g_last_rc=rc;g_background=true;voice_agent_speaker_standby(false);
 board_cores3_backlight_set(100);if(g_cover){lv_obj_delete(g_cover);g_cover=NULL;}
 g_mode=POWER_ACTIVE;g_idle_since=now_ms();g_last_error=g_idle_since;
 printf("[Power] transition_failed rc=%d restored=active no_retry_before_ms=60000\n",rc);
 return rc;
}
void power_service_init(void)
{
 char line[32]={0};FILE *f=fopen("/mnt/sd/POWER.CFG","r");
 if(f){if(fgets(line,sizeof(line),f)&&!strcmp(line,"PLANTPOWER1 1\n"))g_demo=true;fclose(f);}
 g_idle_since=now_ms();g_seen=g_activity;g_started=true;
 printf("[Power] ready dim_s=60 standby_s=180 deep_s=900 deep_battery_only=1 demo=%d\n",g_demo);
}
static void refresh_source(unsigned now)
{
 if(g_source_at&&(uint32_t)(now-g_source_at)<10000)return;g_source_at=now;
 uint8_t value=0;g_source_valid=board_cores3_pmic_read_reg(0,&value)==0;
 g_external=!g_source_valid||(value&0x20)!=0;
 int pct=-1,mv=-1;
 bool low=g_source_valid&&!g_external&&battery_read_pct(&pct)==0&&pct<=10&&
          battery_read_mv(&mv)==0&&mv<3600;
 g_low_count=low?g_low_count+1:0;
 if(g_low_count>3)g_low_count=3;
}
void power_service_tick(bool busy)
{
 if(!g_started)return;unsigned now=now_ms(),event=g_activity;
 refresh_source(now);
 unsigned req=atomic_exchange(&g_request,REQ_NONE);
 if(req==REQ_DEMO||req==REQ_NORMAL){g_demo=req==REQ_DEMO;req=REQ_WAKE;int saved=save_demo();printf("[Power] demo=%d save_rc=%d\n",g_demo,saved);}
 if(req==REQ_CLOSE){delete_menu();req=REQ_WAKE;}
 if(req==REQ_WAKE)delete_menu();
 if(event!=g_seen||req==REQ_WAKE||req==REQ_MENU)
  {g_seen=event;g_idle_since=now;g_deep_failed=false;set_mode(POWER_ACTIVE);}
 if(req==REQ_MENU){show_menu();return;}
 if(req==REQ_DEEP&&(g_external||!g_source_valid))
  {
   if(g_menu){lv_obj_t *hint=lv_obj_get_child(g_menu,-1);lv_label_set_text(hint,"请拔掉供电线，再进入深度休眠");}
   printf("[Power] deep_blocked external_or_unknown=1 active_kept=1\n");return;
  }
 if(g_menu&&req==REQ_NONE)return;
 if((req==REQ_STANDBY||req==REQ_DEEP)&&busy)
  {printf("[Power] request_blocked busy=1\n");return;}
 if(g_last_error&&(uint32_t)(now-g_last_error)<60000)return;
 unsigned idle=now-g_idle_since;
 unsigned want=power_policy(idle,g_enabled,g_demo,busy,g_external,g_source_valid,g_dim,g_standby,g_deep);
 if(g_low_count>=3&&!busy&&!g_demo&&idle>=60000)want=POWER_DEEP;
 if(req==REQ_STANDBY||req==REQ_DEEP){delete_menu();want=req==REQ_DEEP?POWER_DEEP:POWER_STANDBY;}
 /* Manual standby stays asleep until an explicit wake, irrespective of
  * the earlier idle timer. Auto standby follows the same wake contract. */
 if(g_mode==POWER_STANDBY&&want<POWER_STANDBY&&event==g_seen&&!busy&&req==REQ_NONE)want=POWER_STANDBY;
 if(want==POWER_DEEP)
  {
   if(set_mode(POWER_STANDBY)<0)return;
   if(g_external||!g_source_valid||g_deep_failed)
    {if(req==REQ_DEEP)printf("[Power] deep_blocked external_or_unknown=1 standby_kept=1\n");return;}
   /* Release the recorder before cutting the PMIC. Flush application file
    * data; preserve SD/font rails until the final whole-device shutdown. */
   if(voice_wake_pause_release()<0){g_deep_failed=true;return;}
   sync();g_mode=POWER_DEEP;
   printf("[Power] entering deep wake=power_key pmic=AXP2101\n");fflush(stdout);
   int rc=board_cores3_power_off();
   /* If execution continues, do not falsely report completed deep sleep. */
   usleep(800000);g_deep_failed=true;g_mode=POWER_STANDBY;voice_wake_resume();g_last_rc=rc<0?rc:-EIO;
   printf("[Power] deep_not_confirmed rc=%d standby_kept=1\n",g_last_rc);return;
  }
 set_mode(want);
 /* Defer the countdown until real work has finished, without treating
  * camera frames, UI refreshes or cloud polls as user activity. */
 if(busy)g_idle_since=now;
}
int power_service_request(const char *mode)
{
 unsigned r=!strcmp(mode,"standby")?REQ_STANDBY:!strcmp(mode,"deep")?REQ_DEEP:
 !strcmp(mode,"wake")?REQ_WAKE:!strcmp(mode,"menu")?REQ_MENU:
 !strcmp(mode,"demo")?REQ_DEMO:!strcmp(mode,"normal")?REQ_NORMAL:REQ_NONE;
 if(!r)return -EINVAL;g_request=r;return 0;
}
int power_service_command(int argc,char **argv)
{
 if(!argc||!strcmp(argv[0],"status"))
  {printf("[Power] mode=%s enabled=%d demo=%d idle_ms=%u dim_s=%u standby_s=%u deep_s=%u external=%d source_valid=%d background=%d last_rc=%d\n",mode_name(g_mode),g_enabled,g_demo,now_ms()-g_idle_since,g_dim/1000,g_standby/1000,g_deep/1000,g_external,g_source_valid,g_background,g_last_rc);return 0;}
 if(argc==1)return power_service_request(argv[0]);
 if(argc==4&&!strcmp(argv[0],"timers"))
  {unsigned v[3];for(int i=0;i<3;i++){char *end;unsigned long x=strtoul(argv[i+1],&end,10);if(!*argv[i+1]||*end||x<1||x>86400)return -EINVAL;v[i]=x*1000;}
   if(v[0]>=v[1]||v[1]>=v[2])return -EINVAL;g_dim=v[0];g_standby=v[1];g_deep=v[2];power_service_activity();return 0;}
 return -EINVAL;
}
