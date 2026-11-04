#ifndef WAKE_ACK_TIMBRE_H
#define WAKE_ACK_TIMBRE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
/* Same sample-by-sample L=R path as production sp_flush_pcm(). */
static inline int wake_ack_filter(const void *pcm,uint32_t bytes,int16_t *out,
 size_t samples,int16_t (*filter)(int16_t))
{
 if(!pcm || !out || !filter || !bytes || bytes%4 || bytes/2>samples)return -EINVAL;
 const uint8_t *p=pcm;
 for(uint32_t i=0;i<bytes/4;i++)
   {int16_t x;memcpy(&x,p+i*4,2);out[i*2]=out[i*2+1]=filter(x);}
 return (int)bytes;
}
#endif
