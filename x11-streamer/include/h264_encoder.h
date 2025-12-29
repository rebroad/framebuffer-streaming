#ifndef H264_ENCODER_H
#define H264_ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct h264_encoder h264_encoder_t;

// Create H.264 encoder
// width, height: Frame dimensions
// fps: Target frame rate
// bitrate_kbps: Target bitrate in kbps (0 = auto)
h264_encoder_t *h264_encoder_create(uint32_t width, uint32_t height, int fps, int bitrate_kbps);

// Destroy encoder
void h264_encoder_destroy(h264_encoder_t *encoder);

// Encode a frame
// input: Raw ARGB8888 pixel data (width * height * 4 bytes)
// output: Encoded H.264 data (caller must free)
// output_size: Size of encoded data
// Returns: 0 on success, -1 on error
int h264_encoder_encode_frame(h264_encoder_t *encoder,
							  const void *input,
							  void **output,
							  size_t *output_size);

// Get encoder parameters (for debugging)
uint32_t h264_encoder_get_width(h264_encoder_t *encoder);
uint32_t h264_encoder_get_height(h264_encoder_t *encoder);

#endif // H264_ENCODER_H

