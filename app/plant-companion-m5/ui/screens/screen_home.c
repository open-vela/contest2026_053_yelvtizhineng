/* Companion hub. Sensor/task/diary truth remains owned by their services. */
#include <nuttx/config.h>
#include <stdio.h>
#include "screen_home.h"
#include "screen_camera.h"
#include "screen_voice.h"
#include "../ui_app.h"
#include "../theme/companion_style.h"
#include "../../services/power_service.h"
#ifdef CONFIG_ESP32S3_WIFI
#include "screen_ota.h"
#endif
#ifdef CONFIG_PLANT_WIFI_MANAGER
#include "screen_wifi.h"
#endif
static lv_obj_t *s_status,*s_hint,*s_menu;
static void home_voice(lv_event_t *e){(void)e;ui_app_push_screen(screen_voice_create());}
static void home_camera(lv_event_t *e){(void)e;ui_app_push_screen(screen_camera_create());}
static void home_close(lv_event_t *e){(void)e;lv_obj_add_flag(s_menu,LV_OBJ_FLAG_HIDDEN);}
static void home_menu(lv_event_t *e){(void)e;lv_obj_remove_flag(s_menu,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(s_menu);}
static void home_power(lv_event_t *e){home_close(e);power_service_menu();}
#ifdef CONFIG_ESP32S3_WIFI
static void home_ota(lv_event_t *e){home_close(e);ui_app_push_screen(screen_ota_create(false));}
#endif
#ifdef CONFIG_PLANT_WIFI_MANAGER
static void home_wifi(lv_event_t *e){home_close(e);ui_app_push_screen(screen_wifi_create());}
#endif
lv_obj_t *screen_home_create(plant_nav_cb_t cb,void *user_data)
{
 lv_obj_t *p=pal_page("小伴陪着你");pal_face(p,18,39,136,86);
 pal_text(p,170,53,138,62,"一起照顾\n小花",TP_FONT_VALUE);
 s_hint=pal_text(p,170,115,138,25,"聊一聊吧",TP_FONT_BODY);
 s_status=pal_text(p,12,126,140,18,"",TP_FONT_CAPTION);
 pal_button(p,12,144,96,48,"聊一聊",home_voice,NULL);
 pal_button(p,112,144,96,48,"拍照",home_camera,NULL);
 pal_button(p,212,144,96,48,"设置",home_menu,NULL);
 widget_bottomnav_create(p,cb,user_data,PLANT_TAB_HOME,PLANT_NAV_GREEN,false);
 s_menu=pal_shape(p,0,0,320,240,PAL_BG,0);lv_obj_add_flag(s_menu,LV_OBJ_FLAG_CLICKABLE);
 pal_text(s_menu,12,13,220,26,"小伴的设置",TP_FONT_TITLE);
 pal_button(s_menu,260,0,60,48,LV_SYMBOL_LEFT,home_close,NULL);
 pal_face(s_menu,104,47,112,80);
#ifdef CONFIG_PLANT_WIFI_MANAGER
 pal_button(s_menu,12,139,142,48,"网络",home_wifi,NULL);
#endif
#ifdef CONFIG_ESP32S3_WIFI
 pal_button(s_menu,166,139,142,48,"更新",home_ota,NULL);
#endif
 pal_button(s_menu,70,190,180,48,"休息一下",home_power,NULL);
 lv_obj_add_flag(s_menu,LV_OBJ_FLAG_HIDDEN);return p;
}
void screen_home_refresh(int health,int moisture,int temp,int ph_x10,int ec,int valid)
{
 (void)health;(void)moisture;(void)temp;(void)ph_x10;(void)ec;
 if(s_hint)lv_label_set_text(s_hint,valid?"去看看小花":"等小花的数据");
}
void screen_home_set_status(const char *time_str,const char *date_str,int online,int battery)
{
 (void)date_str;char b[64];const char *clock=time_str&&time_str[0]?time_str:"--:--";
 if(battery>=0)snprintf(b,sizeof(b),"%s %s %d%%",clock,online?LV_SYMBOL_WIFI:LV_SYMBOL_CLOSE,battery);
 else snprintf(b,sizeof(b),"%s %s",clock,online?LV_SYMBOL_WIFI:LV_SYMBOL_CLOSE);
 if(s_status)lv_label_set_text(s_status,b);
}
