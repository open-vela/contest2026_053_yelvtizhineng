#include "sensor_presentation.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>

static const char *const names[]={"土壤水分","土壤温度","土壤 EC","盐分","酸碱 pH","氮 N","磷 P","钾 K"};
static const char *const units[]={"%","°C","mS/cm","原值","","mg/kg","mg/kg","mg/kg"};
static const char *const explanations[]={
 "这是根边土壤的水分。\n先看读数，再和大人观察。",
 "这是根边土壤的温度。\n不是叶子温度，也不是气温。",
 "EC 是土壤的电导率。\n只凭这一项，不能判断营养。",
 "这是探头的盐分原始读数。\n协议未注明单位，先不判断。",
 "pH 是土壤的酸碱读数。\n不同植物喜欢的条件不同。",
 "这是土壤探头的氮读数。\n没有配置范围，先不下结论。",
 "这是土壤探头的磷读数。\n没有配置范围，先不下结论。",
 "这是土壤探头的钾读数。\n没有配置范围，先不下结论。"
};
const char *sensor_metric_name(int m){return m>=0&&m<SP_COUNT?names[m]:"";}
const char *sensor_metric_unit(int m){return m>=0&&m<SP_COUNT?units[m]:"";}
const char *sensor_metric_explanation(int m){return m>=0&&m<SP_COUNT?explanations[m]:"";}
const char *sensor_source_name(int s){
 return s==SENSOR_SOURCE_LOCAL?"本地探头":s==SENSOR_SOURCE_CLOUD?"云端记录":s==SENSOR_SOURCE_TEST?"测试输入":"暂无来源";
}
uint16_t sensor_valid_fields(const float v[SP_COUNT]){
  static const float lo[]={0,-40,0,0,0,0,0,0};
  static const float hi[]={100,125,65534,65534,14,65534,65534,65534};
  uint16_t mask=0;
  for(int i=0;i<SP_COUNT;i++)if(isfinite(v[i])&&v[i]>=lo[i]&&v[i]<=hi[i])mask|=1u<<i;
  return mask;
}
void sensor_present(const struct sensor_view_s *v,uint32_t mono,int64_t now,
                    struct sensor_presentation_s *p){
  memset(p,0,sizeof(*p));
  if(!v)return;
  const float values[]={v->moisture,v->temp,v->ec,v->salt,v->ph,v->nitrogen,v->phosphorus,v->potassium};
  memcpy(p->value,values,sizeof(values));
  p->fields=v->field_mask&sensor_valid_fields(values);
  p->source=v->source;
  p->version=v->version;
  snprintf(p->measured_at,sizeof(p->measured_at),"%s",v->measured_at);
  snprintf(p->source_id,sizeof(p->source_id),"%s",v->source_id);
  uint64_t window=(uint64_t)v->expected_interval_ms*3;
  if(v->source==SENSOR_SOURCE_CLOUD)window+=15000;
  if(window>UINT32_MAX)window=UINT32_MAX;
  p->expires_ms=(uint32_t)window;
  if(!p->fields||v->source==SENSOR_SOURCE_NONE){p->freshness=SP_NONE;return;}
  if(!v->expected_interval_ms){p->freshness=SP_TIME_UNKNOWN;return;}
  uint64_t age;
  if(v->source==SENSOR_SOURCE_CLOUD){
    if(v->sample_unix_s<1700000000LL||now<1700000000LL||v->sample_unix_s>now+90){
      p->freshness=SP_TIME_UNKNOWN;return;
    }
    age=(uint64_t)(now>v->sample_unix_s?now-v->sample_unix_s:0)*1000;
  }else age=(uint32_t)(mono-v->last_read_ms);
  p->age_s=age/1000>UINT32_MAX?UINT32_MAX:(uint32_t)(age/1000);
  p->freshness=age>window?SP_STALE:SP_FRESH;
  p->moisture_rule=v->moisture_rule_configured&&p->freshness==SP_FRESH&&(p->fields&(1u<<SP_WATER));
  p->dry=p->moisture_rule&&p->value[SP_WATER]<v->moisture_dry_below;
}
const char *sensor_freshness_name(const struct sensor_presentation_s *p){
 if(p->freshness==SP_NONE)return "还没读到";
 if(p->freshness==SP_STALE)return "旧读数";
 if(p->freshness==SP_TIME_UNKNOWN)return "时间待确认";
 return p->fields==SP_ALL_FIELDS?"新读数":"部分缺测";
}
void sensor_metric_format(const struct sensor_presentation_s *p,int m,char *b,size_t n){
 if(m<0||m>=SP_COUNT||!(p->fields&(1u<<m))){snprintf(b,n,"--");return;}
 float v=p->value[m];
 if(m==SP_EC){
   snprintf(b,n,"%.2f",(double)v/1000);
   size_t len=strlen(b);
   while(len&&b[len-1]=='0')b[--len]=0;
   if(len&&b[len-1]=='.')b[--len]=0;
 }else if(m==SP_WATER||m==SP_TEMP||m==SP_PH)snprintf(b,n,"%.1f",(double)v);
 else snprintf(b,n,"%.0f",(double)v);
}
void sensor_time_note(const struct sensor_presentation_s *p,char *b,size_t n){
 if(p->freshness==SP_NONE){snprintf(b,n,"还没有有效读数，请大人检查探头");return;}
 if(p->freshness==SP_TIME_UNKNOWN){
  if(p->source==SENSOR_SOURCE_CLOUD&&p->measured_at[0])snprintf(b,n,"云端测量 %s · 周期待确认",p->measured_at+5);
  else snprintf(b,n,"%s · 测量时间待确认",sensor_source_name(p->source));
  return;}
 if(p->source==SENSOR_SOURCE_CLOUD&&p->measured_at[0])
   snprintf(b,n,"%s · 测量 %s",sensor_source_name(p->source),p->measured_at+5);
 else snprintf(b,n,"%s · %lu 秒前测量",sensor_source_name(p->source),(unsigned long)p->age_s);
}
void sensor_spoken_summary(const struct sensor_presentation_s *p,char *b,size_t n){
 if(p->freshness==SP_NONE){snprintf(b,n,"我还没读到土壤的情况。请大人检查一下小探头吧。");return;}
 if(p->freshness==SP_STALE||p->freshness==SP_TIME_UNKNOWN){snprintf(b,n,"这些是上一次的读数，先等新读数。我们和大人一起看看真实的小绿吧。");return;}
 if(p->dry){snprintf(b,n,"土有点干。请和大人一起检查土壤，再看看要不要浇水。");return;}
 char water[20],temp[20];
 sensor_metric_format(p,SP_WATER,water,sizeof(water));sensor_metric_format(p,SP_TEMP,temp,sizeof(temp));
 if((p->fields&3)==3)
  snprintf(b,n,"土壤水分百分之%s，根边土壤温度%s摄氏度。我们一起看看新叶子吧。",water,temp);
 else if(p->fields&1)snprintf(b,n,"土壤水分百分之%s。其他读数还没有齐，请大人一起检查探头。",water);
 else if(p->fields&2)snprintf(b,n,"根边土壤温度%s摄氏度。其他读数还没有齐，请大人一起检查探头。",temp);
 else snprintf(b,n,"目前只读到部分土壤数据，请和大人一起查看，先不下结论。");
}

/* Explicit diagnostic override, disabled at every boot. Only the UI reads it. */
static pthread_mutex_t fixture_lock=PTHREAD_MUTEX_INITIALIZER;
static struct sensor_view_s *fixture;
int sensor_present_fixture_set(int mode){
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
 uint32_t now=(uint32_t)(ts.tv_sec*1000u+ts.tv_nsec/1000000u);
 pthread_mutex_lock(&fixture_lock);
 if(mode==0){free(fixture);fixture=NULL;pthread_mutex_unlock(&fixture_lock);return 0;}
 if(mode<1||mode>6){pthread_mutex_unlock(&fixture_lock);return -1;}
 if(!fixture)fixture=malloc(sizeof(*fixture));
 if(!fixture){pthread_mutex_unlock(&fixture_lock);return -1;}
 memset(fixture,0,sizeof(*fixture));
 fixture->source=SENSOR_SOURCE_TEST;fixture->version=mode;
 fixture->moisture=62;fixture->temp=24.3f;fixture->ec=1200;fixture->salt=500;
 fixture->ph=6.4f;fixture->nitrogen=40;fixture->phosphorus=18;fixture->potassium=120;
 fixture->field_mask=SP_ALL_FIELDS;fixture->valid=true;fixture->last_read_ms=now;
 fixture->expected_interval_ms=mode==6?1000:60000;
 fixture->moisture_rule_configured=true;fixture->moisture_dry_below=30;
 snprintf(fixture->source_id,sizeof(fixture->source_id),"UI controlled fixture");
 if(mode==2)fixture->moisture=22;
 if(mode==3)fixture->field_mask&=~((1u<<SP_TEMP)|(1u<<SP_N));
 if(mode==4)fixture->last_read_ms=now-240000u;
 if(mode==5){fixture->field_mask=0;fixture->valid=false;}
 pthread_mutex_unlock(&fixture_lock);return 0;
}
int sensor_present_fixture_get(struct sensor_view_s *out){
 int yes;pthread_mutex_lock(&fixture_lock);yes=fixture!=NULL;
 if(yes)*out=*fixture;
 pthread_mutex_unlock(&fixture_lock);return yes;
}
