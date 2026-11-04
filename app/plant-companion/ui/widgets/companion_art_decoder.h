#ifndef COMPANION_ART_DECODER_H
#define COMPANION_ART_DECODER_H
/* UI thread only; register once after lv_init, before creating images. */
void cu_art_decoder_init(void);
#endif
