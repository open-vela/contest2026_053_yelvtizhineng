#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include "interaction_service.h"
#include "power_service.h"
#include "interaction_policy.h"
#include "server_bridge.h"
#include "voice_service.h"
#include "../ui/ui_app.h"
#include "../ui/screens/screen_camera.h"
#include "../components/camera_capture/camera_capture.h"
#include "../components/stackchan_body/stackchan_body.h"

static pthread_mutex_t g_action_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_action[24],g_arg[64],g_id[64],g_last_id[64];
static char g_target_id[32],g_status[256];
static unsigned g_done,g_failed,g_frame_id;
static int g_yaw,g_pitch;
static _Atomic bool g_worker,g_stop,g_explore_speaking;
static bool g_notice_pending;
static unsigned g_status_until;
struct interaction_job {char action[24],arg[64],run[48];};
static unsigned milliseconds(void){return (unsigned)(clock_systime_ticks()*1000/CLOCKS_PER_SEC);}
static bool allowed(const char *s)
{
  const char *const names[]={"page","capture","inspect","look","home","stop","explore","reset_scene","follow","name","target","target_photo","compare","sleep"};
  for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)if(!strcmp(s,names[i]))return true;
  return false;
}
static void status_message(const char *text,bool speak)
{
  pthread_mutex_lock(&g_action_lock);
  strlcpy(g_status,text,sizeof(g_status));
  /* Avoid a partial trailing UTF-8 character in the small screen message. */
  size_t n=strlen(g_status);
  if(n && n==sizeof(g_status)-1)
    {while(n && ((unsigned char)g_status[n-1]&0xc0)==0x80)n--;if(n && (unsigned char)g_status[n-1]>=0xc0)n--;g_status[n]=0;}
  g_status_until=milliseconds()+12000;
  g_notice_pending=speak;
  pthread_mutex_unlock(&g_action_lock);
  printf("[Interaction] status=%s\n",text);
}
int interaction_get_status(char *text,size_t n)
{
  pthread_mutex_lock(&g_action_lock);
  bool show=g_worker || g_notice_pending || (int)(g_status_until-milliseconds())>0;
  if(show)strlcpy(text,g_status,n);else if(n)text[0]=0;
  pthread_mutex_unlock(&g_action_lock);return show;
}
void interaction_stop(void)
{
  g_stop=true;
  if(g_explore_speaking)voice_service_stop();
  char *args[]={"stop"};stackchan_body_command(1,args);
}
void interaction_pause_motion(void)
{
  if(g_worker){char *args[]={"stop"};stackchan_body_command(1,args);}
}
bool interaction_active(void){return g_worker;}
bool interaction_voice_blocked(void)
{pthread_mutex_lock(&g_action_lock);bool blocked=g_worker||*g_action||g_notice_pending;pthread_mutex_unlock(&g_action_lock);return blocked;}
int interaction_submit(const char *action,const char *arg,const char *id)
{
  if(!action||!allowed(action)||!arg||!id||strlen(arg)>=sizeof(g_arg)||strlen(id)>=sizeof(g_id))return -EINVAL;
  pthread_mutex_lock(&g_action_lock);
  int rc=0;
  if(*id && !strcmp(id,g_last_id))rc=-EALREADY;
  else if(*g_action && strcmp(action,"stop"))rc=-EBUSY;
  else
    {if(g_worker)g_stop=true;
     strlcpy(g_action,action,sizeof(g_action));strlcpy(g_arg,arg,sizeof(g_arg));strlcpy(g_id,id,sizeof(g_id));}
  pthread_mutex_unlock(&g_action_lock);
  if(rc==0)power_service_activity();
  if(!strcmp(action,"stop"))interaction_stop();
  return rc;
}
static int wait_idle(void)
{
  while(!g_stop && voice_service_busy())usleep(50000);
  return g_stop?-ECANCELED:0;
}
static int wait_pose(struct stackchan_pose *pose)
{
  for(int i=0;i<240 && !g_stop;i++)
    {int rc=stackchan_body_get_pose(pose);if(rc<0)return rc;
     if(pose->settled)return 0;usleep(50000);}
  return g_stop?-ECANCELED:-ETIMEDOUT;
}
static int report_direction(const char *text,unsigned direction)
{
  if(wait_idle()<0)return -ECANCELED;
  status_message(text,false);
  g_explore_speaking=true;
  int rc=g_stop?-ECANCELED:voice_service_say(text);
  /* Stop may race the asynchronous voice start; cancel the newly owned notice. */
  if(g_stop && rc==0)voice_service_stop();
  printf("[Explore] direction=%u report_start rc=%d\n",direction,rc);
  if(rc==0)rc=wait_idle();
  g_explore_speaking=false;
  printf("[Explore] direction=%u report_done rc=%d\n",direction,rc);
  return rc;
}
static int move_degrees_mask(int yaw,int pitch,struct stackchan_pose *pose,unsigned axes)
{
  if(axes<1 || axes>3)return -EINVAL;
  if(wait_idle()<0)return -ECANCELED;
  unsigned ticket=0;
  int rc=camera_preview_motion_pause(&ticket);
  if(rc<0)return rc; /* No motion unless the producer acknowledged idle. */
  printf("[CameraMotion] paused owner=%u\n",ticket);
  if(g_stop || (ticket && !camera_preview_is_running()))rc=-ECANCELED;
  else {rc=axes==3?stackchan_body_look(yaw,pitch):stackchan_body_look_axis(axes==1?1:2,axes==1?yaw:pitch);if(rc==0)rc=wait_pose(pose);}
  uint32_t before=camera_preview_get_total_frames();
  int resumed=camera_preview_motion_resume(ticket);
  printf("[CameraMotion] resumed=%d motion_rc=%d\n",resumed,rc);
  if(rc<0)return rc;
  if(ticket && !resumed)return -ECANCELED;
  if(ticket)
    {
      if(g_stop)return -ECANCELED;
      rc=camera_preview_wait_new_frame(before,1800);
      if(rc<0)return rc;
      if(g_stop)return -ECANCELED;
      printf("[CameraMotion] fresh_frame_after_pose=1\n");
    }
  return 0;
}
static int move_degrees(int yaw,int pitch,struct stackchan_pose *pose)
{return move_degrees_mask(yaw,pitch,pose,3);}

static int frame_request(struct interaction_job *job,const char *mode,struct plant_vision_result *v)
{
  if(wait_idle()<0)return -ECANCELED;
  if(!screen_camera_is_active())return -ENODEV;
  struct stackchan_pose pose={0};
  /* Inspect is a camera-only task; a missing base must not disable vision.
   * No orientation landmark is stored for this uncalibrated snapshot. */
  if(strcmp(mode,"inspect") && wait_pose(&pose)<0)return -EIO;
  const uint8_t *image;size_t bytes;
  int rc=-EAGAIN;
  for(int attempt=0;attempt<4 && !g_stop;attempt++)
    {if(wait_idle()<0)return -ECANCELED;
     rc=camera_preview_snapshot(&image,&bytes,1800);
     if(rc==0)break;
     if(rc!=-EAGAIN && rc!=-EBUSY && rc!=-ETIMEDOUT)break;
     usleep(100000);}
  if(rc<0)return rc;
  uint8_t *small=NULL;int width=320,height=240;
  if(!strcmp(mode,"faces"))
    {
      width=160;height=120;bytes=38400;small=malloc(bytes);if(!small)return -ENOMEM;
      for(int y=0;y<120;y++)for(int x=0;x<160;x++)
        {size_t a=((size_t)y*2*320+x*2)*2,b=((size_t)y*160+x)*2;small[b]=image[a];small[b+1]=image[a+1];}
      image=small;
    }
  unsigned frame=++g_frame_id;unsigned start=milliseconds();
  for(int attempt=0;attempt<3;attempt++)
    {
      if(wait_idle()<0){rc=-ECANCELED;break;}
      rc=(!strcmp(mode,"inspect")?server_bridge_vision_spoken:server_bridge_vision)
          (mode,image,bytes,width,height,pose.yaw,pose.pitch,frame,job->run,job->arg,v);
      /* Retry only a positively identified invalid upstream JSON answer.
       * Keep the captured image, pose and run. The server consumes a frame
       * ID even when semantic parsing fails, so a retry needs a new ID.
       * Transport
       * timeouts and other 4xx errors are not silently replayed. */
      if(rc!=-EBADMSG || attempt==2 || g_stop)break;
      frame=++g_frame_id;
      printf("[Vision] retry frame=%u attempt=%d reason=upstream-json\n",frame,attempt+2);
      usleep(200000);
    }
  free(small);
  if(g_stop)return -ECANCELED;
  if(rc==0 && v->frame_id!=(int)frame)return -ESTALE;
  if(rc==0)printf("[Vision] mode=%s frame=%u found=%d confidence=%d center=%d,%d target=%s ms=%u\n",mode,frame,v->found,v->confidence,v->cx,v->cy,v->target_id,milliseconds()-start);
  return rc;
}
static void remember_observation(const struct plant_vision_result *v)
{
  if(*v->target_id)
    {pthread_mutex_lock(&g_action_lock);strlcpy(g_target_id,v->target_id,sizeof(g_target_id));pthread_mutex_unlock(&g_action_lock);}
}
static void *interaction_worker(void *argument)
{
  struct interaction_job *job=argument;
  struct plant_vision_result v;
  struct stackchan_pose pose;
  int rc=0;char message[256]="";
  stackchan_body_focus(true);
  if(strcmp(job->action,"look") && strcmp(job->action,"home") &&
     strcmp(job->action,"inspect") && strcmp(job->action,"name"))
    {rc=stackchan_body_prepare();if(rc<0)goto job_complete;}

  if(!strcmp(job->action,"look") || !strcmp(job->action,"home"))
    {
      rc=stackchan_body_prepare();if(rc<0)goto job_complete;
      rc=stackchan_body_get_pose(&pose);if(rc<0)goto job_complete;
      int yaw=(pose.yaw-pose.home_yaw)*5/16,pitch=(pose.pitch-pose.home_pitch)*5/16;
      if(!strcmp(job->action,"home"))yaw=pitch=0;
      else if(!strcmp(job->arg,"left"))yaw+=20;else if(!strcmp(job->arg,"right"))yaw-=20;
      else if(!strcmp(job->arg,"up"))pitch+=15;else if(!strcmp(job->arg,"down"))pitch-=15;
      else {rc=-EINVAL;goto job_complete;}
      if(yaw>BODY_YAW_MAX)yaw=BODY_YAW_MAX;if(yaw<BODY_YAW_MIN)yaw=BODY_YAW_MIN;
      if(pitch>BODY_PITCH_MAX)pitch=BODY_PITCH_MAX;if(pitch<BODY_PITCH_MIN)pitch=BODY_PITCH_MIN;
      unsigned axes=!strcmp(job->action,"home")?3:
                    (!strcmp(job->arg,"left") || !strcmp(job->arg,"right"))?1:2;
      rc=move_degrees_mask(yaw,pitch,&pose,axes);
      if(rc==0){g_yaw=yaw;g_pitch=pitch;}
    }
  else if(!strcmp(job->action,"name"))
    {
      char target[32];pthread_mutex_lock(&g_action_lock);strlcpy(target,g_target_id,sizeof(target));pthread_mutex_unlock(&g_action_lock);
      if(!*target){rc=-ENOENT;strlcpy(message,"先带我探索一下，再给看到的东西取名字吧。",sizeof(message));}
      else {rc=server_bridge_remember(target,job->arg,&v);if(rc==0)strlcpy(message,v.message,sizeof(message));}
    }
  else if(!strcmp(job->action,"inspect"))
    {
      /* One fresh preview snapshot, short object answer; no sweep or health
       * report. Reuse authenticated scene recognition and target naming. */
      status_message("正在看镜头前的物体...",false);
      rc=frame_request(job,"inspect",&v);
      if(rc==0)strlcpy(message,v.message,sizeof(message));
    }
  else if(!strcmp(job->action,"explore")||!strcmp(job->action,"reset_scene"))
    {
      if(!strcmp(job->action,"reset_scene"))
        {rc=server_bridge_reset_scene();if(rc<0)goto job_complete;
         pthread_mutex_lock(&g_action_lock);g_target_id[0]=0;pthread_mutex_unlock(&g_action_lock);}
      const int angles[][2]={{0,0},{-60,0},{-120,0},{60,0},{120,0},
                            {0,-15},{0,20},{0,45},{-60,35},{60,35}};unsigned discoveries=0;
      for(unsigned i=0;i<sizeof(angles)/sizeof(angles[0]) && !g_stop;i++)
        {
          char progress[128];snprintf(progress,sizeof(progress),"正在探索，第%u个方向...",i+1);status_message(progress,false);
          rc=move_degrees(angles[i][0],angles[i][1],&pose);if(rc<0)break;
          rc=frame_request(job,"explore",&v);if(rc<0)break;
          if(v.found && *v.target_id)
            {remember_observation(&v);discoveries++;}
          /* Complete this direction's narration before moving the camera again. */
          if(*v.message)snprintf(message,sizeof(message),"%s",v.message);
          else if(v.found && *v.label)snprintf(message,sizeof(message),"这个方向看到了%s。",v.label);
          else strlcpy(message,"这个方向暂时没有看清楚。",sizeof(message));
          rc=report_direction(message,i+1);message[0]=0;if(rc<0)break;
        }
      if(rc==0 && !g_stop)
        {rc=move_degrees(0,0,&pose);
         if(rc<0)goto job_complete;
         if(discoveries)strlcpy(message,"探索完啦，可以给刚才看到的东西取个名字。",sizeof(message));
         else strlcpy(message,"暂时没有看清楚，你可以把想认识的东西放到镜头前。",sizeof(message));}
    }
  else if(!strcmp(job->action,"follow"))
    {
      unsigned started=milliseconds(),lost=0,errors=0,tracked=0;
      while(!g_stop && milliseconds()-started<120000)
        {
          rc=frame_request(job,"faces",&v);
          if(rc<0){if(++errors>=3)break;usleep(200000);continue;}
          errors=0;
          if(!v.found)
            {
              lost++;status_message("正在找镜头前的人脸...",false);
              if(lost>=8 && !tracked){strlcpy(message,"还没找到人脸，你可以站到镜头前再叫我。",sizeof(message));break;}
              if(lost%8==0){int search[]={0,-5,5};rc=move_degrees(search[(lost/8)%3],0,&pose);if(rc<0)break;}
            }
          else
            {
              lost=0;tracked++;
              status_message("看到你啦。",false);
              if(stackchan_body_get_pose(&pose)<0){rc=-ENODEV;break;}
              int yaw=(pose.yaw-pose.home_yaw)*5/16,pitch=(pose.pitch-pose.home_pitch)*5/16;
              int next_yaw,next_pitch;
              int step=interaction_follow_target(v.cx,v.cy,v.horizontal_sign,v.vertical_sign,yaw,pitch,&next_yaw,&next_pitch);
              if(step<0){strlcpy(message,"先带我探索一下，校准转头方向，再来跟着你。",sizeof(message));rc=step;break;}
              if(step>0){rc=move_degrees(next_yaw,next_pitch,&pose);if(rc<0)break;}

            }
          usleep(200000);
        }
      if(!*message)strlcpy(message,"这一轮看完啦。",sizeof(message));
    }
  else
    {
      rc=server_bridge_target(job->arg,&v);
      if(rc==0)
        {
          int target_yaw=v.yaw,target_pitch=v.pitch;
          rc=stackchan_body_look_raw(target_yaw,target_pitch);
          if(rc==0)rc=wait_pose(&pose);
          if(rc==0 && (abs(pose.yaw-target_yaw)>10 || abs(pose.pitch-target_pitch)>10))rc=-EIO;
          const char *mode=!strcmp(job->action,"compare")?"compare":!strcmp(job->action,"target_photo")?"photo":"verify";
          if(rc==0)rc=frame_request(job,mode,&v);
          if(rc==0 && !v.found)
            {
              for(int side=-1;side<=1 && !v.found;side+=2)
                {if(g_stop)break;
                 int yaw=(target_yaw-pose.home_yaw)*5/16+side*3,pitch=(target_pitch-pose.home_pitch)*5/16;
                 if(yaw<BODY_YAW_MIN||yaw>BODY_YAW_MAX)continue;
                 rc=move_degrees(yaw,pitch,&pose);if(rc<0)break;
                 rc=frame_request(job,mode,&v);if(rc<0)break;}
            }
          if(rc==0)strlcpy(message,v.message,sizeof(message));
          if(rc==0 && !v.found)rc=-ENOENT;
        }
      else snprintf(message,sizeof(message),"我还没记住%s，先带我认识它吧。",job->arg);
    }
job_complete:
  if(!*message && rc<0)strlcpy(message,"这次没有完成，请确认网络和相机后再试。",sizeof(message));
  if(rc<0 && (!strcmp(job->action,"look") || !strcmp(job->action,"home")))
    strlcpy(message,"底座还没有准备好，暂时不能转动。",sizeof(message));
  if(rc==-EHOSTDOWN)strlcpy(message,"底座通信中断，已经停止转动。",sizeof(message));
  if(rc==-ERANGE)strlcpy(message,"目标在当前转动范围外，请重新带我认识它。",sizeof(message));
  bool cancelled=g_stop;
  if(!cancelled && *message)status_message(message,true);
  printf("[InteractionJob] action=%s rc=%d cancelled=%d frames=%u\n",job->action,rc,cancelled,g_frame_id);
  if(rc==0 && !cancelled)g_done++;else if(!cancelled)g_failed++;
  stackchan_body_focus(false);free(job);g_worker=false;
  return NULL;
}
static int start_job(const char *action,const char *arg)
{
  struct interaction_job *job=calloc(1,sizeof(*job));if(!job)return -ENOMEM;
  strlcpy(job->action,action,sizeof(job->action));strlcpy(job->arg,arg,sizeof(job->arg));
  snprintf(job->run,sizeof(job->run),"m5-%lx-%x",(unsigned long)time(NULL),milliseconds());
  pthread_attr_t attr;pthread_t thread;pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr,8192);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
  g_stop=false;g_worker=true;int rc=pthread_create(&thread,&attr,interaction_worker,job);pthread_attr_destroy(&attr);
  if(rc){g_worker=false;free(job);return -rc;}return 0;
}
void interaction_tick(void)
{
  char action[24],arg[64],id[64];
  pthread_mutex_lock(&g_action_lock);
  bool urgent=!strcmp(g_action,"stop");
  pthread_mutex_unlock(&g_action_lock);
  if((voice_service_busy()||g_worker) && !urgent)return;
  pthread_mutex_lock(&g_action_lock);
  strlcpy(action,g_action,sizeof(action));strlcpy(arg,g_arg,sizeof(arg));strlcpy(id,g_id,sizeof(id));
  if(*action)g_action[0]=0;
  bool notice=g_notice_pending;
  if(!*action && notice)strlcpy(arg,"",sizeof(arg));
  pthread_mutex_unlock(&g_action_lock);
  if(!*action)
    {
      if(notice && !voice_service_busy() && !g_worker)
        {char text[256];interaction_get_status(text,sizeof(text));
         if(*text && voice_service_say(text)==0){pthread_mutex_lock(&g_action_lock);g_notice_pending=false;pthread_mutex_unlock(&g_action_lock);}}
      return;
    }
  int rc=-EINVAL;
  if(!strcmp(action,"sleep")){voice_service_abort();rc=power_service_request(arg);}
  else if(!strcmp(action,"page")){interaction_stop();rc=ui_app_business_page(arg);}
  else if(!strcmp(action,"capture")){interaction_stop();rc=ui_app_business_capture();}
  else if(!strcmp(action,"stop")){interaction_stop();status_message("停下来啦。",false);rc=0;}
  else if(!strcmp(action,"look")||!strcmp(action,"home"))
    {
      rc=start_job(action,arg);
    }
  else
    {
      if(strcmp(action,"name"))
        {rc=ui_app_business_page("camera");if(rc<0)goto complete;
         /* The page request is consumed later in this UI tick. Retry next tick. */
         if(!screen_camera_is_active() || camera_preview_get_frame_index()<=0)
           {pthread_mutex_lock(&g_action_lock);strlcpy(g_action,action,sizeof(g_action));strlcpy(g_arg,arg,sizeof(g_arg));strlcpy(g_id,id,sizeof(g_id));pthread_mutex_unlock(&g_action_lock);return;}}
      rc=start_job(action,arg);
    }
complete:
  pthread_mutex_lock(&g_action_lock);strlcpy(g_last_id,id,sizeof(g_last_id));pthread_mutex_unlock(&g_action_lock);
  if(rc==0)g_done++;else g_failed++;
  printf("[Interaction] action=%s arg=%s id=%s rc=%d done=%u failed=%u\n",action,arg,id,rc,g_done,g_failed);
}
int interaction_command(int argc,char **argv)
{
  if(argc==1&&!strcmp(argv[0],"status"))
    {printf("[Interaction] done=%u failed=%u yaw=%d pitch=%d active=%d frames=%u target=%s\n",g_done,g_failed,g_yaw,g_pitch,g_worker,g_frame_id,g_target_id);return 0;}
  if(argc==3)return interaction_submit(argv[0],argv[1],argv[2]);
  return -EINVAL;
}
