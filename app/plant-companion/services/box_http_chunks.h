#ifndef BOX_HTTP_CHUNKS_H
#define BOX_HTTP_CHUNKS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
/* Incremental HTTP chunk framing. Only decoded body bytes reach playback. */
struct box_http_chunks {
  char line[128];size_t used,left,total,limit,trailers;unsigned state;
};
static inline int box_http_chunks_feed(struct box_http_chunks *c,
    const uint8_t *data,size_t size,int (*consume)(const uint8_t *,size_t,void *),void *arg)
{
  size_t off=0;
  while(off<size){
    if(c->state==5)return -EBADMSG;
    if(c->state==1){
      size_t n=size-off;if(n>c->left)n=c->left;
      if(n>c->limit-c->total)return -EFBIG;
      int ret=consume(data+off,n,arg);if(ret<0)return ret;
      c->total+=n;c->left-=n;off+=n;if(!c->left)c->state=2;continue;
    }
    unsigned char ch=data[off++];
    if(c->state==2){if(ch!='\r')return -EBADMSG;c->state=3;continue;}
    if(c->state==3){if(ch!='\n')return -EBADMSG;c->state=0;continue;}
    if(c->state==4&&++c->trailers>1024)return -EMSGSIZE;
    if(c->used==sizeof(c->line)-1)return -EMSGSIZE;
    c->line[c->used++]=(char)ch;
    if(ch!='\n')continue;
    if(c->used<2||c->line[c->used-2]!='\r')return -EBADMSG;
    c->line[c->used-2]=0;
    if(c->state==4){
      if(c->used==2)c->state=5;
      else if(!strchr(c->line,':'))return -EBADMSG;
    }else{
      size_t n=0,i=0;
      for(;c->line[i]&&c->line[i]!=';';i++){
        unsigned char v=c->line[i];unsigned digit;
        if(v>='0'&&v<='9')digit=v-'0';
        else if(v>='a'&&v<='f')digit=v-'a'+10;
        else if(v>='A'&&v<='F')digit=v-'A'+10;
        else return -EBADMSG;
        if(n>(SIZE_MAX-digit)/16)return -EOVERFLOW;
        n=n*16+digit;
      }
      if(!i)return -EBADMSG;
      if(n>c->limit-c->total)return -EFBIG;
      c->left=n;c->state=n?1:4;
    }
    c->used=0;
  }
  return c->state==5?1:0;
}
#endif
