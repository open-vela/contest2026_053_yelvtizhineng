/* Cloud sensor cache only; one plant and one truthful parameter at a time. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "screen_data.h"
#include "../theme/companion_style.h"
static struct sensor_view_s s_view;
static int s_index;
static lv_obj_t *s_name,*s_value,*s_note,*s_source,*s_count;
static const char *const names[]={"水分","温度","EC","盐分","氮","磷","钾","pH"};
static const char *const units[]={"%","°C"," uS/cm"," mg/kg"," mg/kg"," mg/kg"," mg/kg",""};
static void data_show(void)
{
 char b[48];float values[]={s_view.moisture,s_view.temp,s_view.ec,s_view.salt,s_view.nitrogen,s_view.phosphorus,s_view.potassium,s_view.ph};
 bool valid=s_view.valid && isfinite(values[s_index]);
 snprintf(b,sizeof(b),"%d/8",s_index+1);lv_label_set_text(s_count,b);lv_label_set_text(s_name,names[s_index]);
 if(valid)snprintf(b,sizeof(b),s_index<2||s_index==7?"%.1f":"%.0f",(double)values[s_index]);else snprintf(b,sizeof(b),"--");
 lv_label_set_text(s_value,b);lv_label_set_text(s_source,valid?units[s_index]:"等待云端数据");
 /* Only the service's existing evaluated parameters carry care advice.
  * EC is not mapped to fertilizer, nor N/P/K/pH to invented health rules. */
 const char *note="看看小花的数据";
 if(!valid)note="还不太清楚";
 else if(s_index==0 && s_view.moisture_note[0])note=s_view.moisture_note;
 else if(s_index==1 && s_view.temp_note[0])note=s_view.temp_note;

 lv_label_set_text(s_note,note);
}
static void data_step(lv_event_t *e)
{s_index=(s_index+(int)(intptr_t)lv_event_get_user_data(e)+8)%8;data_show();}
lv_obj_t *screen_data_create(plant_nav_cb_t cb,void *user_data)
{
 lv_obj_t *p=pal_page("小花的状态");s_index=0;s_view.valid=false;
 s_count=pal_text(p,252,8,56,22,"",TP_FONT_BODY);
 /* Sprout + stem + pot: the plant shares the companion's face. */
 pal_shape(p,69,93,10,37,PAL_LEAF,5);
 pal_shape(p,33,103,38,14,PAL_LEAF,LV_RADIUS_CIRCLE);
 pal_shape(p,78,99,37,14,PAL_LEAF,LV_RADIUS_CIRCLE);
 pal_shape(p,45,123,61,23,PAL_YELLOW,8);
 pal_face(p,20,35,112,72);
 pal_shape(p,148,36,160,104,PAL_PAPER,18);
 s_name=pal_text(p,160,43,136,22,"",TP_FONT_BODY);
 s_value=pal_text(p,160,67,136,34,"",TP_FONT_VALUE);
 s_source=pal_text(p,160,104,136,28,"",TP_FONT_CAPTION);
 s_note=pal_text(p,12,149,190,42,"",TP_FONT_BODY);
 pal_button(p,202,144,50,48,LV_SYMBOL_LEFT,data_step,(void *)(intptr_t)-1);
 pal_button(p,258,144,50,48,LV_SYMBOL_RIGHT,data_step,(void *)(intptr_t)1);
 widget_bottomnav_create(p,cb,user_data,PLANT_TAB_DATA,PLANT_NAV_GREEN,false);
 data_show();return p;
}
void screen_data_refresh(const struct sensor_view_s *v,const struct sensor_history_s *history)
{(void)history;if(!v)return;s_view=*v;data_show();}
