#include <lvgl/lvgl.h>
#include <lvgl/src/draw/lv_image_decoder_private.h>
#include <string.h>
#include "companion_art_decoder.h"

extern const lv_image_dsc_t cu_buddy;
extern const lv_image_dsc_t *const cu_voice_frames[6];
typedef struct {const lv_image_dsc_t *source;lv_draw_buf_t *row;} art_session;

static bool supported(const void *source)
{
 if(source==&cu_buddy)return true;
 for(int i=0;i<6;i++)if(source==cu_voice_frames[i])return true;
 return false;
}
static lv_result_t art_info(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc,lv_image_header_t *header)
{
 (void)decoder;
 if(dsc->src_type!=LV_IMAGE_SRC_VARIABLE||!supported(dsc->src))return LV_RESULT_INVALID;
 const lv_image_dsc_t *source=dsc->src;
 *header=source->header;
 header->cf=LV_COLOR_FORMAT_ARGB8888;header->stride=source->header.w*4;
 return LV_RESULT_OK;
}
static lv_result_t art_open(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc)
{
 (void)decoder;
 if(!supported(dsc->src))return LV_RESULT_INVALID;
 art_session *session=lv_malloc(sizeof(*session));if(!session)return LV_RESULT_INVALID;
 session->source=dsc->src;
 session->row=lv_draw_buf_create(session->source->header.w,1,LV_COLOR_FORMAT_ARGB8888,LV_STRIDE_AUTO);
 if(!session->row){lv_free(session);return LV_RESULT_INVALID;}
 dsc->user_data=session;dsc->decoded=NULL;return LV_RESULT_OK;
}
static lv_result_t art_area(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc,
 const lv_area_t *full,lv_area_t *area)
{
 (void)decoder;
 art_session *session=dsc->user_data;
 if(!session||full->x1<0||full->y1<0||full->x2>=session->source->header.w||full->y2>=session->source->header.h)return LV_RESULT_INVALID;
 int width=lv_area_get_width(full);if(width<=0)return LV_RESULT_INVALID;
 if(area->y1==LV_COORD_MIN){
  if(!lv_draw_buf_reshape(session->row,LV_COLOR_FORMAT_ARGB8888,width,1,LV_STRIDE_AUTO))return LV_RESULT_INVALID;
  *area=*full;area->y2=area->y1;
 }else{area->y1++;area->y2++;}
 if(area->y1>full->y2)return LV_RESULT_INVALID;
 const uint8_t *palette=session->source->data;
 const uint8_t *indices=palette+1024+area->y1*session->source->header.stride+area->x1;
 for(int x=0;x<width;x++)memcpy(session->row->data+x*4,palette+indices[x]*4,4);
 dsc->decoded=session->row;return LV_RESULT_OK;
}
static void art_close(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc)
{
 (void)decoder;
 art_session *session=dsc->user_data;
 if(session){lv_draw_buf_destroy(session->row);lv_free(session);}
 dsc->user_data=NULL;dsc->decoded=NULL;
}
void cu_art_decoder_init(void)
{
 static lv_image_decoder_t *decoder;
 if(decoder)return;
 decoder=lv_image_decoder_create();if(!decoder)return;
 lv_image_decoder_set_info_cb(decoder,art_info);
 lv_image_decoder_set_open_cb(decoder,art_open);
 lv_image_decoder_set_get_area_cb(decoder,art_area);
 lv_image_decoder_set_close_cb(decoder,art_close);
}
