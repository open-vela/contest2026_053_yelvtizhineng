#ifndef PLANT_CAMERA_JPEG_H
#define PLANT_CAMERA_JPEG_H
#include <stdint.h>
#include <stddef.h>
/* Input remains full-resolution LE RGB565. Caller frees successful output.
 * Unsupported geometry/allocation/encoder errors leave the raw path usable. */
int camera_jpeg_encode_rgb565(const uint8_t *input,size_t size,int width,int height,
                             uint8_t **output,size_t *output_size);
#endif
