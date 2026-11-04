#ifndef PLANT_SAMPLE_TIME_H
#define PLANT_SAMPLE_TIME_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* Backend utils.bj_now() emits Beijing civil time, independent of device TZ. */
static inline int64_t plant_sample_epoch(const char *s){
 int y,m,d,h,mi,se;char extra;
 if(!s||strlen(s)!=19||sscanf(s,"%d-%d-%d %d:%d:%d%c",&y,&m,&d,&h,&mi,&se,&extra)!=6)return 0;
 if(y<1970||y>2099||m<1||m>12||h<0||h>23||mi<0||mi>59||se<0||se>59)return 0;
 static const int md[]={31,28,31,30,31,30,31,31,30,31,30,31};
 int leap=(y%4==0&&(y%100!=0||y%400==0));
 if(d<1||d>md[m-1]+(m==2&&leap))return 0;
 int64_t days=0;
 for(int a=1970;a<y;a++)days+=365+(a%4==0&&(a%100!=0||a%400==0));
 for(int a=1;a<m;a++)days+=md[a-1]+(a==2&&leap);
 return (days+d-1)*86400+h*3600+mi*60+se-8*3600;
}
#endif
