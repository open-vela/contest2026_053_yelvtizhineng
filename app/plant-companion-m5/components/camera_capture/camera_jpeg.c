/* Official StackChan dependency: esp_new_jpeg 0.6.1, mono-task, quality 80.
 * Convert one MCU-row block, preserving the existing 320x240 image size.
 * No C++/FreeRTOS task or full RGB888 frame allocation is needed.
 */
#include <nuttx/config.h>
#include <errno.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "camera_jpeg.h"
#include "jpeg_vendor/esp_jpeg_enc.h"

#define JPEG_OUTPUT_CAP (64u*1024u)
_Static_assert(sizeof(jpeg_enc_config_t)==28,"Official JPEG config ABI mismatch");
extern unsigned camera_jpeg_rtos_calls(void);
extern void xtensa_coproc_enable(int cpset);

int camera_jpeg_encode_rgb565(const uint8_t *input,size_t size,int width,int height,
                             uint8_t **output,size_t *output_size)
{
  jpeg_enc_handle_t encoder=NULL;
  uint8_t *block=NULL,*encoded=NULL;
  int result=-EIO,encoded_size=0;
  if(!output || !output_size)return -EINVAL;
  *output=NULL;*output_size=0;
  if(!input || width<=0 || height<=0 || width>320 || height>240 ||
     (width%16) || (height%16) || size!=(size_t)width*height*2)return -EINVAL;
  jpeg_enc_config_t config=DEFAULT_JPEG_ENC_CONFIG();
  config.width=width;config.height=height;
  config.src_type=JPEG_PIXEL_FORMAT_RGB888;
  config.subsampling=JPEG_SUBSAMPLE_420;config.quality=80;
  config.task_enable=false;config.rotate=JPEG_ROTATE_0D;
  unsigned unsupported=camera_jpeg_rtos_calls();
  xtensa_coproc_enable(1<<3);
  int rc=jpeg_enc_open(&config,&encoder);
  if(rc!=JPEG_ERR_OK || !encoder)goto done;
  int block_size=jpeg_enc_get_block_size(encoder);
  if(block_size<=0 || block_size>320*16*3 || block_size%(width*3) ||
     ((size_t)width*height*3)%block_size)goto done;
  block=memalign(16,(size_t)block_size);
  encoded=memalign(16,JPEG_OUTPUT_CAP);
  if(!block || !encoded){result=-ENOMEM;goto done;}
  memset(encoded,0,JPEG_OUTPUT_CAP);
  size_t pixels_per_block=(size_t)block_size/3;
  size_t pixels=(size_t)width*height;
  for(size_t offset=0;offset<pixels;offset+=pixels_per_block)
    {
      for(size_t j=0;j<pixels_per_block;j++)
        {
          size_t k=(offset+j)*2;
          uint16_t value=(uint16_t)input[k]|((uint16_t)input[k+1]<<8);
          unsigned r=(value>>11)&31,g=(value>>5)&63,b=value&31;
          block[j*3]=(uint8_t)((r<<3)|(r>>2));
          block[j*3+1]=(uint8_t)((g<<2)|(g>>4));
          block[j*3+2]=(uint8_t)((b<<3)|(b>>2));
        }
      rc=jpeg_enc_process_with_block(encoder,block,block_size,encoded,JPEG_OUTPUT_CAP,&encoded_size);
      if(rc<JPEG_ERR_OK)goto done;
    }
  if(rc!=JPEG_ERR_OK || encoded_size<4 || encoded_size>(int)JPEG_OUTPUT_CAP ||
     (size_t)encoded_size>=size || encoded[0]!=0xff || encoded[1]!=0xd8 ||
     encoded[encoded_size-2]!=0xff || encoded[encoded_size-1]!=0xd9 ||
     camera_jpeg_rtos_calls()!=unsupported)goto done;
  *output=encoded;*output_size=(size_t)encoded_size;encoded=NULL;result=0;
done:
  if(encoder)jpeg_enc_close(encoder);
  if(camera_jpeg_rtos_calls()!=unsupported && result==0)
    {free(*output);*output=NULL;*output_size=0;result=-ENOTSUP;}
  free(block);free(encoded);
  return result;
}

int camera_jpeg_selftest(void)
{
  static const uint16_t colors[]={0xffff,0xffe0,0x07ff,0x07e0,0xf81f,0xf800,0x001f,0x0000};
  uint8_t *raw=malloc(320u*240u*2u),*jpeg=NULL;size_t bytes=0;
  if(!raw)return -ENOMEM;
  for(unsigned y=0;y<240;y++)for(unsigned x=0;x<320;x++)
    {uint16_t value=colors[x/40];size_t at=((size_t)y*320+x)*2;raw[at]=(uint8_t)value;raw[at+1]=(uint8_t)(value>>8);}
  int rc=camera_jpeg_encode_rgb565(raw,320u*240u*2u,320,240,&jpeg,&bytes);free(raw);
  if(rc<0){printf("[JpegTest] error rc=%d rtos=%u\n",rc,camera_jpeg_rtos_calls());return rc;}
  uint32_t hash=2166136261u;
  for(size_t i=0;i<bytes;i++){hash^=jpeg[i];hash*=16777619u;}
  printf("[JpegTest] begin bytes=%u fnv=%08x width=320 height=240 rtos=%u\n",(unsigned)bytes,(unsigned)hash,camera_jpeg_rtos_calls());
  const char hex[]="0123456789abcdef";
  for(size_t at=0;at<bytes;at+=64)
    {
      char line[160];int n=snprintf(line,sizeof(line),"[JpegTest] data %05u ",(unsigned)at);
      size_t count=bytes-at;if(count>64)count=64;
      for(size_t i=0;i<count;i++){line[n++]=hex[jpeg[at+i]>>4];line[n++]=hex[jpeg[at+i]&15];}
      line[n]=0;printf("%s\n",line);
    }
  printf("[JpegTest] end\n");free(jpeg);return 0;
}
