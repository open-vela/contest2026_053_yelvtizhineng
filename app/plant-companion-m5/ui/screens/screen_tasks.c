/* One real task at a time, guided by the same companion as voice. */
#include <stdio.h>
#include <stdint.h>
#include "screen_tasks.h"
#include "../theme/companion_style.h"
#include "../../services/record_service.h"
static int s_index;
static lv_obj_t *s_title,*s_time,*s_count,*s_message,*s_action,*s_prev,*s_next;
static void tasks_show(void)
{
 int count=record_task_get_count();if(count>TASK_MAX)count=TASK_MAX;
 if(s_index>=count)s_index=count?count-1:0;
 struct record_task_s t;char b[32];bool have=count>0 && record_task_get(s_index,&t);
 snprintf(b,sizeof(b),"%d/%d",count?s_index+1:0,count);lv_label_set_text(s_count,b);
 lv_label_set_text(s_title,have?t.title:"今天没有任务");
 lv_label_set_text(s_time,have?t.time_slot:"");
 lv_label_set_text(s_message,have?(t.done?"你真棒！":"一起试一试"):"聊一聊吧");
 lv_label_set_text(lv_obj_get_child(s_action,0),have?(t.done?"已完成 · 撤销":"我做到了"):"暂无任务");
 if(have)lv_obj_remove_state(s_action,LV_STATE_DISABLED);else lv_obj_add_state(s_action,LV_STATE_DISABLED);
 if(s_index>0)lv_obj_remove_state(s_prev,LV_STATE_DISABLED);else lv_obj_add_state(s_prev,LV_STATE_DISABLED);
 if(s_index+1<count)lv_obj_remove_state(s_next,LV_STATE_DISABLED);else lv_obj_add_state(s_next,LV_STATE_DISABLED);
}
static void tasks_step(lv_event_t *e)
{int n=s_index+(int)(intptr_t)lv_event_get_user_data(e);if(n>=0 && n<record_task_get_count() && n<TASK_MAX)s_index=n;tasks_show();}
static void tasks_done(lv_event_t *e)
{
 (void)e;struct record_task_s t;if(!record_task_get(s_index,&t))return;
 record_task_set_done(s_index,!t.done);if(!t.done)record_task_on_completed();tasks_show();
}
lv_obj_t *screen_tasks_create(plant_nav_cb_t cb,void *user_data)
{
 lv_obj_t *p=pal_page("一起做任务");s_index=0;
 s_count=pal_text(p,252,8,56,22,"",TP_FONT_BODY);
 pal_face(p,14,40,112,84);s_message=pal_text(p,14,124,112,22,"",TP_FONT_BODY);
 pal_shape(p,138,37,170,99,PAL_PAPER,18);
 s_title=pal_text(p,144,47,158,54,"",TP_FONT_TITLE);
 s_time=pal_text(p,144,107,158,22,"",TP_FONT_CAPTION);
 s_prev=pal_button(p,12,144,50,48,LV_SYMBOL_LEFT,tasks_step,(void *)(intptr_t)-1);
 s_next=pal_button(p,70,144,50,48,LV_SYMBOL_RIGHT,tasks_step,(void *)(intptr_t)1);
 s_action=pal_button(p,136,144,172,48,"",tasks_done,NULL);
 widget_bottomnav_create(p,cb,user_data,PLANT_TAB_TASKS,PLANT_NAV_GREEN,false);
 tasks_show();return p;
}
