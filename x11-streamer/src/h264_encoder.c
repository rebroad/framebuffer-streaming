#include "h264_encoder.h"
#include <x264.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct h264_encoder {
    x264_t *encoder;
    x264_param_t params;
    uint32_t width;
    uint32_t height;
    int fps;
    x264_picture_t pic_in;
    x264_picture_t pic_out;
    bool initialized;
};

h264_encoder_t *h264_encoder_create(uint32_t width, uint32_t height, int fps, int bitrate_kbps)
{
    h264_encoder_t *enc = calloc(1, sizeof(h264_encoder_t));
    if (!enc)
        return NULL;

    enc->width = width;
    enc->height = height;
    enc->fps = fps > 0 ? fps : 60;

    // Set up x264 parameters for low latency
    x264_param_default_preset(&enc->params, "ultrafast", "zerolatency");

    // Configure for low latency streaming
    enc->params.i_width = width;
    enc->params.i_height = height;
    enc->params.i_fps_num = enc->fps;
    enc->params.i_fps_den = 1;
    enc->params.i_keyint_max = enc->fps * 2;  // Keyframe every 2 seconds
    enc->params.b_intra_refresh = 1;  // Use intra refresh instead of keyframes for lower latency
    enc->params.i_bframe = 0;  // No B-frames for lower latency
    enc->params.b_annexb = 1;  // Use Annex-B format (NAL units)

    // Set bitrate if specified
    if (bitrate_kbps > 0) {
        enc->params.rc.i_bitrate = bitrate_kbps;
        enc->params.rc.i_rc_method = X264_RC_ABR;  // Average bitrate
    } else {
        // Auto bitrate based on resolution
        // Rough estimate: ~1 Mbps per 100k pixels
        uint64_t pixels = width * height;
        enc->params.rc.i_bitrate = (pixels / 100000) * 1000;
        if (enc->params.rc.i_bitrate < 1000)
            enc->params.rc.i_bitrate = 1000;  // Minimum 1 Mbps
        enc->params.rc.i_rc_method = X264_RC_ABR;
    }

    // Tune for low latency
    enc->params.rc.i_vbv_max_bitrate = enc->params.rc.i_bitrate * 2;
    enc->params.rc.i_vbv_buffer_size = enc->params.rc.i_bitrate;

    // CPU usage: ultrafast preset already minimizes CPU
    // But we can set thread count
    enc->params.i_threads = 1;  // Single thread for lower latency

    // Initialize encoder
    enc->encoder = x264_encoder_open(&enc->params);
    if (!enc->encoder) {
        free(enc);
        return NULL;
    }

    // Allocate picture buffers
    x264_picture_alloc(&enc->pic_in, X264_CSP_I420, width, height);
    enc->initialized = true;

    return enc;
}

void h264_encoder_destroy(h264_encoder_t *encoder)
{
    if (!encoder)
        return;

    if (encoder->encoder) {
        x264_encoder_close(encoder->encoder);
        encoder->encoder = NULL;
    }

    if (encoder->initialized) {
        x264_picture_clean(&encoder->pic_in);
    }

    free(encoder);
}

// Convert ARGB8888 to I420 (YUV420)
static void argb_to_i420(const uint8_t *argb, uint8_t *y, uint8_t *u, uint8_t *v,
                         uint32_t width, uint32_t height, uint32_t pitch)
{
    // Simple conversion. TODO: could be optimized with SIMD
    uint32_t uv_width = width / 2;

    for (uint32_t i = 0; i < height; i++) {
        for (uint32_t j = 0; j < width; j++) {
            const uint8_t *pixel = argb + (i * pitch + j * 4);
            uint8_t r = pixel[2];
            uint8_t g = pixel[1];
            uint8_t b = pixel[0];

            // Convert RGB to Y
            int y_val = (int)(0.299 * r + 0.587 * g + 0.114 * b);
            y[i * width + j] = (y_val > 255) ? 255 : (y_val < 0) ? 0 : y_val;

            // Subsample U and V (every 2x2 block)
            if (i % 2 == 0 && j % 2 == 0) {
                int u_val = (int)(-0.169 * r - 0.331 * g + 0.5 * b + 128);
                int v_val = (int)(0.5 * r - 0.419 * g - 0.081 * b + 128);

                uint32_t uv_idx = (i / 2) * uv_width + (j / 2);
                u[uv_idx] = (u_val > 255) ? 255 : (u_val < 0) ? 0 : u_val;
                v[uv_idx] = (v_val > 255) ? 255 : (v_val < 0) ? 0 : v_val;
            }
        }
    }
}

int h264_encoder_encode_frame(h264_encoder_t *encoder,
                              const void *input,
                              void **output,
                              size_t *output_size)
{
    if (!encoder || !encoder->encoder || !input || !output || !output_size)
        return -1;

    // Convert ARGB8888 to I420
    // x264 expects I420 with specific plane layout
    uint8_t *y_plane = encoder->pic_in.img.plane[0];
    uint8_t *u_plane = encoder->pic_in.img.plane[1];
    uint8_t *v_plane = encoder->pic_in.img.plane[2];

    // Assume input is ARGB8888 with pitch = width * 4
    uint32_t input_pitch = encoder->width * 4;
    argb_to_i420((const uint8_t *)input, y_plane, u_plane, v_plane,
                 encoder->width, encoder->height, input_pitch);

    // Set picture properties
    encoder->pic_in.i_pts = encoder->pic_in.i_pts + 1;
    encoder->pic_in.i_type = X264_TYPE_AUTO;

    // Encode
    x264_nal_t *nals = NULL;
    int i_nals = 0;
    int frame_size = x264_encoder_encode(encoder->encoder, &nals, &i_nals, &encoder->pic_in, &encoder->pic_out);

    if (frame_size < 0) {
        return -1;
    }

    // Allocate output buffer
    size_t total_size = 0;
    for (int i = 0; i < i_nals; i++) {
        total_size += nals[i].i_payload;
    }

    uint8_t *out_buf = malloc(total_size);
    if (!out_buf)
        return -1;

    // Copy NAL units to output buffer
    size_t offset = 0;
    for (int i = 0; i < i_nals; i++) {
        memcpy(out_buf + offset, nals[i].p_payload, nals[i].i_payload);
        offset += nals[i].i_payload;
    }

    *output = out_buf;
    *output_size = total_size;

    return 0;
}

uint32_t h264_encoder_get_width(h264_encoder_t *encoder)
{
    return encoder ? encoder->width : 0;
}

uint32_t h264_encoder_get_height(h264_encoder_t *encoder)
{
    return encoder ? encoder->height : 0;
}

