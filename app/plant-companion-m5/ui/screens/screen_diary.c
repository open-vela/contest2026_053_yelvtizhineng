/* Real stored memories, presented as one scrapbook page at a time. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "screen_diary.h"
#include "../theme/companion_style.h"
#include "../../services/record_service.h"
static int s_index;
static lv_obj_t *s_title,*s_detail,*s_day,*s_count,*s_prev,*s_next;
static void diary_show(void)
{
 int count=record_diary_get_count();if(count>DIARY_MAX)count=DIARY_MAX;
 if(s_index>=count)s_index=count?count-1:0;
 struct record_diary_s d;char b[32];bool have=count>0 && record_diary_get(s_index,&d);
 snprintf(b,sizeof(b),"%d/%d",count?s_index+1:0,count);lv_label_set_text(s_count,b);
 lv_label_set_text(s_title,have?d.title:"还没有日记");
 lv_label_set_text(s_detail,have?d.detail:"一起留下小小回忆");
 if(have){if(d.day_offset==0)snprintf(b,sizeof(b),"今天");else if(d.day_offset==1)snprintf(b,sizeof(b),"昨天");else snprintf(b,sizeof(b),"%lu天前",(unsigned long)d.day_offset);}
 else b[0]=0;lv_label_set_text(s_day,b);
 if(s_index>0)lv_obj_remove_state(s_prev,LV_STATE_DISABLED);else lv_obj_add_state(s_prev,LV_STATE_DISABLED);
 if(s_index+1<count)lv_obj_remove_state(s_next,LV_STATE_DISABLED);else lv_obj_add_state(s_next,LV_STATE_DISABLED);
}
static void diary_step(lv_event_t *e)
{int n=s_index+(int)(intptr_t)lv_event_get_user_data(e);if(n>=0 && n<record_diary_get_count() && n<DIARY_MAX)s_index=n;diary_show();}
static void diary_water(lv_event_t *e)
{
 (void)e;struct record_diary_s d;memset(&d,0,sizeof(d));d.color=0x3b82f6;
 snprintf(d.title,sizeof(d.title),"记录了一次浇水");snprintf(d.detail,sizeof(d.detail),"手动记录");
 record_diary_add(&d);record_diary_save();s_index=0;diary_show();
}
lv_obj_t *screen_diary_create(plant_nav_cb_t cb,void *user_data)
{
 lv_obj_t *p=pal_page("我们的成长日记");s_index=0;
 s_count=pal_text(p,252,8,56,22,"",TP_FONT_BODY);
 pal_face(p,16,43,100,78);pal_text(p,19,121,100,20,"小小回忆",TP_FONT_BODY);
 pal_shape(p,128,36,180,103,PAL_PAPER,18);
 s_day=pal_text(p,138,40,160,18,"",TP_FONT_CAPTION);
 s_title=pal_text(p,138,61,160,48,"",TP_FONT_TITLE);
 s_detail=pal_text(p,138,109,160,28,"",TP_FONT_CAPTION);
 s_prev=pal_button(p,12,144,50,48,LV_SYMBOL_LEFT,diary_step,(void *)(intptr_t)-1);
 pal_button(p,70,144,180,48,"记一次浇水",diary_water,NULL);
 s_next=pal_button(p,258,144,50,48,LV_SYMBOL_RIGHT,diary_step,(void *)(intptr_t)1);
 widget_bottomnav_create(p,cb,user_data,PLANT_TAB_DIARY,PLANT_NAV_GREEN,false);
 diary_show();return p;
}
