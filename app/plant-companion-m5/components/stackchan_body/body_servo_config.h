#ifndef BODY_SERVO_CONFIG_H
#define BODY_SERVO_CONFIG_H
#include <stdint.h>
#include <stdbool.h>
struct body_servo_config {unsigned fw_major,fw_minor,endian,model_major,model_minor,id,baud,reply_level,min,max;};
/* SCSCL read-only inspection of0..12; never changes EEPROM or calibration. */
static inline bool body_servo_config_decode(unsigned id,const uint8_t p[13],struct body_servo_config *v)
{
 if(!p || !v)return false;
 v->fw_major=p[0];v->fw_minor=p[1];v->endian=p[2];v->model_major=p[3];v->model_minor=p[4];
 v->id=p[5];v->baud=p[6];v->reply_level=p[8];v->min=((unsigned)p[9]<<8)|p[10];v->max=((unsigned)p[11]<<8)|p[12];
 return id>=1 && id<=2 && v->id==id && v->endian==1 && v->baud==0 && v->reply_level<=1 && v->min<v->max && v->max<=1023;
}
#endif
