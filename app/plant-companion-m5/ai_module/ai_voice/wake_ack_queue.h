/* Bounded queue shared by production and host checks. No allocation. */
#ifndef PLANT_WAKE_ACK_QUEUE_H
#define PLANT_WAKE_ACK_QUEUE_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
static int wake_ack_queue(const void *pcm, size_t bytes,
 int (*write_pcm)(const void *,uint32_t), int (*flush)(void),
 bool (*cancelled)(void))
{
 if(!pcm || !bytes || bytes%4 || !write_pcm || !flush || !cancelled)return -EINVAL;
 const uint8_t *p=pcm;size_t offset=0;int result=0;
 while(offset<bytes)
   {
    if(cancelled()){result=-ECANCELED;break;}
    uint32_t n=bytes-offset>2044?2044:(uint32_t)(bytes-offset);
    int rc=write_pcm(p+offset,n);
    if(rc<=0 || (uint32_t)rc>n || rc%4){result=rc<0?rc:-EIO;break;}
    offset+=(unsigned)rc;
   }
 int drained=flush();
 if(result)return result;
 if(cancelled())return -ECANCELED;
 return drained<0?drained:0;
}
#endif
