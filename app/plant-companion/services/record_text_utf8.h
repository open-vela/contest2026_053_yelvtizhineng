#ifndef RECORD_TEXT_UTF8_H
#define RECORD_TEXT_UTF8_H
#include <stddef.h>
#include <string.h>
/* Repair fixed byte fields without keeping partial or invalid code points.
 * The binary record layout and every complete UTF-8 character are preserved. */
static inline void record_text_utf8(char *s,size_t capacity)
{
 size_t r=0,w=0;if(!s||!capacity)return;
 while(r<capacity&&s[r]){
  unsigned char a=(unsigned char)s[r];size_t n=0;
  if(a<0x80)n=1;else if(a>=0xc2&&a<=0xdf)n=2;
  else if(a>=0xe0&&a<=0xef)n=3;else if(a>=0xf0&&a<=0xf4)n=4;
  int valid=n&&r+n<capacity;
  for(size_t j=1;valid&&j<n;j++){
   unsigned char b=(unsigned char)s[r+j];if((b&0xc0)!=0x80)valid=0;
  }
  if(valid&&n>=3){unsigned char b=(unsigned char)s[r+1];
   if((a==0xe0&&b<0xa0)||(a==0xed&&b>=0xa0)||
      (a==0xf0&&b<0x90)||(a==0xf4&&b>=0x90))valid=0;
  }
  if(!valid){r++;continue;}
  memmove(s+w,s+r,n);w+=n;r+=n;
 }
 s[w]=0;
}
#endif
