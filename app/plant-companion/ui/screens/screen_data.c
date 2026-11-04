#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "screen_data.h"
#include "../ui_app.h"
#include "../widgets/companion_ui.h"
#include "../../services/sensor_presentation.h"
#include "../../services/voice_service.h"
#include "../../services/ai_service.h"
typedef struct {
 lv_obj_t *root,*overview,*detail,*list,*fresh,*time,*summary;
 lv_obj_t *value[4],*detail_title,*detail_value,*detail_note,*detail_source;
 lv_obj_t *row_name[3],*row_value[3],*page_title,*page_note;
 struct sensor_presentation_s snapshot;
 int mode,metric,page,rendered_mode;
 bool speaking,leaving;
} data_page;
static data_page *active;
static const int first[]={SP_WATER,SP_TEMP,SP_EC,SP_PH};
static void render(data_page *p);
static void back(lv_event_t *e){
 data_page *p=lv_event_get_user_data(e);
 if(p->mode){p->mode=0;render(p);}
 else{if(p->speaking)voice_service_abort();p->leaving=true;ui_app_nav_to(PLANT_TAB_HOME);}
}
static void metric(lv_event_t *e){
 data_page *p=active;if(!p)return;p->metric=(int)(intptr_t)lv_event_get_user_data(e);p->mode=1;render(p);
}
static void all(lv_event_t *e){data_page *p=lv_event_get_user_data(e);p->mode=2;p->page=0;render(p);}
static void page(lv_event_t *e){
 data_page *p=active;if(!p)return;
 p->page+= (int)(intptr_t)lv_event_get_user_data(e);
 if(p->page<0)p->page=0;
 if(p->page>2)p->page=2;
 render(p);
}
static void speak(lv_event_t *e){
 data_page *p=lv_event_get_user_data(e);
 if(p->speaking&&voice_service_busy()){voice_service_abort();lv_label_set_text(lv_obj_get_child(p->summary,0),"正在停止");return;}
 if(voice_service_busy()||ai_service_busy()){lv_label_set_text(lv_obj_get_child(p->summary,0),"请等一下");return;}
 char text[400];sensor_spoken_summary(&p->snapshot,text,sizeof(text));
 int ret=voice_service_say(text);
 p->speaking=ret==0;
 lv_label_set_text(lv_obj_get_child(p->summary,0),ret<0?"重试播报":"正在准备");
}
static void deleted(lv_event_t *e){
 data_page *p=lv_event_get_user_data(e);
 if(p->speaking&&!p->leaving)voice_service_abort();
 if(active==p)active=NULL;
 lv_free(p);
}
static void render(data_page *p){
 struct sensor_presentation_s *s=&p->snapshot;char a[48],text[192];
 if(p->rendered_mode!=p->mode){
 p->rendered_mode=p->mode;
 lv_obj_add_flag(p->overview,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(p->detail,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(p->list,LV_OBJ_FLAG_HIDDEN);
 lv_obj_remove_flag(p->mode==0?p->overview:p->mode==1?p->detail:p->list,LV_OBJ_FLAG_HIDDEN);
 }
 lv_label_set_text(p->fresh,s->dry?"土有点干":sensor_freshness_name(s));
 sensor_time_note(s,text,sizeof(text));lv_label_set_text(p->time,text);
 for(int i=0;i<4;i++){
  sensor_metric_format(s,first[i],a,sizeof(a));
  snprintf(text,sizeof(text),"%s%s%s",a,first[i]==SP_PH?"":" ",sensor_metric_unit(first[i]));
  lv_label_set_text(p->value[i],text);
 }
 int m=p->metric;sensor_metric_format(s,m,a,sizeof(a));
 lv_label_set_text(p->detail_title,sensor_metric_name(m));
 snprintf(text,sizeof(text),"%s %s",a,sensor_metric_unit(m));lv_label_set_text(p->detail_value,text);
 lv_label_set_text(p->detail_note,sensor_metric_explanation(m));
 snprintf(text,sizeof(text),"%s · %s\n%s · 本组版本 %lu",sensor_source_name(s->source),
   (s->fields&(1u<<m))?sensor_freshness_name(s):"这一项缺测",s->source_id,(unsigned long)s->version);
 lv_label_set_text(p->detail_source,text);
 snprintf(text,sizeof(text),"全部读数   %d / 3",p->page+1);lv_label_set_text(p->page_title,text);
 for(int i=0;i<3;i++){
  m=p->page*3+i;
  if(m>=SP_COUNT){lv_obj_add_flag(lv_obj_get_parent(p->row_name[i]),LV_OBJ_FLAG_HIDDEN);continue;}
  lv_obj_remove_flag(lv_obj_get_parent(p->row_name[i]),LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(p->row_name[i],sensor_metric_name(m));sensor_metric_format(s,m,a,sizeof(a));
  snprintf(text,sizeof(text),"%s %s",a,sensor_metric_unit(m));lv_label_set_text(p->row_value[i],text);
 }
 snprintf(text,sizeof(text),"%s · %s · 同组 %lu",sensor_source_name(s->source),sensor_freshness_name(s),(unsigned long)s->version);
 lv_label_set_text(p->page_note,text);
}
lv_obj_t *screen_data_create(plant_nav_cb_t cb,void *u){
 (void)cb;(void)u;lv_obj_t *r=cu_root();
 data_page *p=lv_malloc(sizeof(*p));if(!p)return r;memset(p,0,sizeof(*p));p->root=r;p->rendered_mode=-1;active=p;
 lv_obj_add_event_cb(r,deleted,LV_EVENT_DELETE,p);
 cu_button(r,8,4,64,48,"返回",16,0xe4edda,back,p);
 cu_label(r,84,14,235,"小绿的状态",22,CU_INK);
 p->fresh=cu_label(r,330,20,145,"还没读到",16,0x667d66);
 p->overview=cu_panel(r,0,56,480,264,CU_BG,0);
 cu_image(p->overview,&cu_soil_plant,146,3);
 /* Four connections all terminate in the root/soil region (y=183 absolute). */
 static const lv_point_precise_t points[4][2]={{{140,60},{213,127}},{{340,60},{265,127}},{{140,144},{213,127}},{{340,144},{265,127}}};
 for(int i=0;i<4;i++){
  lv_obj_t *line=lv_line_create(p->overview);lv_line_set_points(line,points[i],2);
  lv_obj_set_style_line_color(line,lv_color_hex(0xc5d5ba),0);lv_obj_set_style_line_width(line,2,0);
 }
 for(int i=0;i<4;i++){
  int x=i%2?340:12,y=i<2?14:106;
  lv_obj_t *b=cu_button(p->overview,x,y,128,76,"",16,0xffffff,metric,(void *)(intptr_t)first[i]);
  cu_label(b,8,7,120,sensor_metric_name(first[i]),16,0x667d66);
  p->value[i]=cu_label(b,8,31,120,"--",22,CU_INK);
 }
 p->time=cu_label(p->overview,16,181,452,"还没有有效读数",16,0x667d66);
 cu_button(p->overview,246,204,218,48,"全部读数",22,0xe4edda,all,p);
 p->summary=cu_button(p->overview,16,204,218,48,"听小伴说",22,CU_GREEN,speak,p);
 p->detail=cu_panel(r,0,56,480,264,CU_BG,0);
 p->detail_title=cu_label(p->detail,24,8,430,"土壤水分",22,CU_INK);
 p->detail_value=cu_label(p->detail,24,48,430,"--",30,CU_INK);
 p->detail_note=cu_label(p->detail,24,103,432,"",22,CU_INK);
 p->detail_source=cu_label(p->detail,24,187,432,"",16,0x667d66);
 p->list=cu_panel(r,0,56,480,264,CU_BG,0);
 p->page_title=cu_label(p->list,24,2,432,"全部读数",22,CU_INK);
 for(int i=0;i<3;i++){
  lv_obj_t *row=cu_panel(p->list,16,36+i*51,448,47,0xffffff,14);
  p->row_name[i]=cu_label(row,12,10,190,"",22,CU_INK);
  p->row_value[i]=cu_label(row,208,9,228,"",22,CU_INK);
 }
 p->page_note=cu_label(p->list,20,191,440,"",16,0x667d66);
 cu_button(p->list,16,216,210,48,"上一页",22,0xe4edda,page,(void *)(intptr_t)-1);
 cu_button(p->list,254,216,210,48,"下一页",22,0xe4edda,page,(void *)(intptr_t)1);
 sensor_service_read_now();
 struct sensor_view_s v;sensor_service_get_view(&v);screen_data_refresh(&v,NULL);
 return r;
}
void screen_data_refresh(const struct sensor_view_s *v,const struct sensor_history_s *h){
 (void)h;data_page *p=active;if(!p)return;
 struct sensor_view_s fixture;
 if(sensor_present_fixture_get(&fixture))v=&fixture;
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
 sensor_present(v,(uint32_t)(ts.tv_sec*1000u+ts.tv_nsec/1000000u),ui_app_unix_time(),&p->snapshot);
 render(p);
 if(p->speaking){
  if(!voice_service_busy()){
   p->speaking=false;
   lv_label_set_text(lv_obj_get_child(p->summary,0),voice_service_last_error()<0?"重试播报":"再听一次");
  }else lv_label_set_text(lv_obj_get_child(p->summary,0),voice_service_get_state()==VOICE_STATE_SPEAKING?"停一下":"正在准备");
 }
}


int screen_data_voice_view(int mode,int metric_id)
{
 if(!active)return -1;
 active->mode=mode;if(metric_id>=0&&metric_id<SP_COUNT)active->metric=metric_id;
 if(mode==2)active->page=0;render(active);
 printf("[DataVoice] mode=%d metric=%d page=%d\n",active->mode,active->metric,active->page);return 0;
}
int screen_data_voice_back(void)
{
 if(!active||!active->mode)return -1;
 active->mode=0;render(active);
 printf("[DataVoice] mode=%d metric=%d page=%d\n",active->mode,active->metric,active->page);return 0;
}
int screen_data_voice_step(int delta,char *text,size_t size)
{
 if(!active||active->mode!=2)return -1;
 active->page+=delta;if(active->page<0)active->page=0;if(active->page>2)active->page=2;
 render(active);printf("[DataVoice] mode=%d metric=%d page=%d\n",active->mode,active->metric,active->page);
 size_t used=snprintf(text,size,"第%d页。",active->page+1);
 for(int i=active->page*3;i<active->page*3+3&&i<SP_COUNT&&used+80<size;i++){
  char value[48];sensor_metric_format(&active->snapshot,i,value,sizeof(value));
  used+=snprintf(text+used,size-used,"%s，%s%s。",sensor_metric_name(i),value,sensor_metric_unit(i));
 }
 return 0;
}
