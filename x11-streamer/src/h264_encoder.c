#include "h264_encoder.h"
#include <x264.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif
#ifdef __AVX2__
#include <immintrin.h>
#endif

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

// Convert ARGB8888 to I420 (YUV420) with SIMD optimization
// Uses fixed-point arithmetic for better performance
static void argb_to_i420(const uint8_t *argb, uint8_t *y, uint8_t *u, uint8_t *v,
                         uint32_t width, uint32_t height, uint32_t pitch)
{
    uint32_t uv_width = width / 2;

#ifdef __SSE2__
    // SIMD-optimized path using SSE2
    // Fixed-point coefficients (scaled by 256 for precision)
    const __m128i y_r_coeff = _mm_set1_epi16(77);   // 0.299 * 256 ≈ 77
    const __m128i y_g_coeff = _mm_set1_epi16(150); // 0.587 * 256 ≈ 150
    const __m128i y_b_coeff = _mm_set1_epi16(29);  // 0.114 * 256 ≈ 29

    for (uint32_t i = 0; i < height; i++) {
        uint32_t j = 0;

        // Process 4 pixels at a time (SSE2 processes 4 32-bit pixels)
        for (; j + 4 <= width; j += 4) {
            // Load 4 ARGB pixels (16 bytes)
            __m128i pixels = _mm_loadu_si128((__m128i*)(argb + i * pitch + j * 4));

            // Extract R, G, B channels (ARGB format: byte order is B G R A)
            // Shuffle to get: R R R R, G G G G, B B B B
            __m128i r = _mm_and_si128(_mm_srli_epi32(pixels, 16), _mm_set1_epi32(0xFF));
            __m128i g = _mm_and_si128(_mm_srli_epi32(pixels, 8), _mm_set1_epi32(0xFF));
            __m128i b = _mm_and_si128(pixels, _mm_set1_epi32(0xFF));

            // Convert to 16-bit for multiplication
            __m128i r_16 = _mm_unpacklo_epi8(_mm_packus_epi16(r, r), _mm_setzero_si128());
            __m128i g_16 = _mm_unpacklo_epi8(_mm_packus_epi16(g, g), _mm_setzero_si128());
            __m128i b_16 = _mm_unpacklo_epi8(_mm_packus_epi16(b, b), _mm_setzero_si128());

            // Calculate Y = (77*R + 150*G + 29*B) / 256
            __m128i y_val = _mm_add_epi16(_mm_add_epi16(
                _mm_mullo_epi16(r_16, y_r_coeff),
                _mm_mullo_epi16(g_16, y_g_coeff)),
                _mm_mullo_epi16(b_16, y_b_coeff));
            y_val = _mm_srli_epi16(y_val, 8); // Divide by 256

            // Store Y values (pack to 8-bit)
            __m128i y_packed = _mm_packus_epi16(y_val, y_val);
            _mm_storel_epi64((__m128i*)(y + i * width + j), y_packed);

            // For U/V, process every 2x2 block (simplified - use first pixel of block)
            if (i % 2 == 0 && j % 2 == 0) {
                uint8_t r_val = (uint8_t)_mm_extract_epi16(r_16, 0);
                uint8_t g_val = (uint8_t)_mm_extract_epi16(g_16, 0);
                uint8_t b_val = (uint8_t)_mm_extract_epi16(b_16, 0);

                int u_val = (-43 * r_val - 85 * g_val + 128 * b_val) / 256 + 128;
                int v_val = (128 * r_val - 107 * g_val - 21 * b_val) / 256 + 128;

                uint32_t uv_idx = (i / 2) * uv_width + (j / 2);
                u[uv_idx] = (u_val > 255) ? 255 : (u_val < 0) ? 0 : u_val;
                v[uv_idx] = (v_val > 255) ? 255 : (v_val < 0) ? 0 : v_val;
            }
        }

        // Handle remaining pixels with scalar code
        for (; j < width; j++) {
            const uint8_t *pixel = argb + (i * pitch + j * 4);
            uint8_t r = pixel[2];
            uint8_t g = pixel[1];
            uint8_t b = pixel[0];

            // Convert RGB to Y using fixed-point
            int y_val = (77 * r + 150 * g + 29 * b) / 256;
            y[i * width + j] = (y_val > 255) ? 255 : (y_val < 0) ? 0 : y_val;

            // Subsample U and V (every 2x2 block)
            if (i % 2 == 0 && j % 2 == 0) {
                int u_val = (-43 * r - 85 * g + 128 * b) / 256 + 128;
                int v_val = (128 * r - 107 * g - 21 * b) / 256 + 128;

                uint32_t uv_idx = (i / 2) * uv_width + (j / 2);
                u[uv_idx] = (u_val > 255) ? 255 : (u_val < 0) ? 0 : u_val;
                v[uv_idx] = (v_val > 255) ? 255 : (v_val < 0) ? 0 : v_val;
            }
        }
    }
#else
    // Scalar fallback path (no SIMD)
    // Fixed-point coefficients for better performance
    for (uint32_t i = 0; i < height; i++) {
        for (uint32_t j = 0; j < width; j++) {
            const uint8_t *pixel = argb + (i * pitch + j * 4);
            uint8_t r = pixel[2];
            uint8_t g = pixel[1];
            uint8_t b = pixel[0];

            // Convert RGB to Y using fixed-point (0.299*256≈77, 0.587*256≈150, 0.114*256≈29)
            int y_val = (77 * r + 150 * g + 29 * b) / 256;
            y[i * width + j] = (y_val > 255) ? 255 : (y_val < 0) ? 0 : y_val;

            // Subsample U and V (every 2x2 block)
            if (i % 2 == 0 && j % 2 == 0) {
                // Fixed-point: -0.169*256≈-43, -0.331*256≈-85, 0.5*256=128
                //              0.5*256=128, -0.419*256≈-107, -0.081*256≈-21
                int u_val = (-43 * r - 85 * g + 128 * b) / 256 + 128;
                int v_val = (128 * r - 107 * g - 21 * b) / 256 + 128;

                uint32_t uv_idx = (i / 2) * uv_width + (j / 2);
                u[uv_idx] = (u_val > 255) ? 255 : (u_val < 0) ? 0 : u_val;
                v[uv_idx] = (v_val > 255) ? 255 : (v_val < 0) ? 0 : v_val;
            }
        }
    }
#endif
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

