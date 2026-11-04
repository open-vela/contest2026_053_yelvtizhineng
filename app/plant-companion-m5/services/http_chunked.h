/* Bounded incremental HTTP chunk framing; never pass framing to PCM audio. */
#ifndef PLANT_HTTP_CHUNKED_H
#define PLANT_HTTP_CHUNKED_H
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
struct plant_chunked {size_t left; unsigned state,n,trailers; char line[80];};
typedef int (*plant_chunk_cb)(const uint8_t *,size_t,void *);
/* state: size, data, data CR, data LF, trailers, done */
static int plant_chunk_feed(struct plant_chunked *s,const uint8_t *p,size_t n,
                            plant_chunk_cb cb,void *arg)
{
  while(n)
    {
      if(s->state==5)return -EPROTO;
      if(s->state==1)
        {size_t take=n<s->left?n:s->left;int rc=cb(p,take,arg);if(rc<0)return rc;
         p+=take;n-=take;s->left-=take;if(!s->left)s->state=2;continue;}
      unsigned char c=*p++;n--;
      if(s->state==2){if(c!='\r')return -EPROTO;s->state=3;continue;}
      if(s->state==3){if(c!='\n')return -EPROTO;s->state=0;continue;}
      if(s->n>=sizeof(s->line)-1)return -EMSGSIZE;
      s->line[s->n++]=(char)c;
      if(c!='\n')continue;
      if(s->n<2||s->line[s->n-2]!='\r')return -EPROTO;
      if(s->state==4)
        {s->trailers+=s->n;if(s->trailers>2048)return -EMSGSIZE;
         if(s->n==2)s->state=5;s->n=0;continue;}
      size_t value=0;unsigned digits=0,i=0;
      for(;i<s->n-2 && s->line[i]!=';';i++)
        {unsigned char d=(unsigned char)s->line[i];unsigned v;
         if(d>='0'&&d<='9')v=d-'0';else if(d>='a'&&d<='f')v=d-'a'+10;
         else if(d>='A'&&d<='F')v=d-'A'+10;else return -EPROTO;
         if(value>(SIZE_MAX-v)/16)return -EOVERFLOW;value=value*16+v;digits++;}
      if(!digits)return -EPROTO;
      s->n=0;s->left=value;s->state=value?1:4;
    }
  return 0;
}
#endif
