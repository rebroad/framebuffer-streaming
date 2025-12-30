#include "x11_streamer.h"
#include "x11_output.h"
#include "drm_fb.h"
#include "protocol.h"
#include "audio_capture.h"
#include "dirty_rect.h"
#include "encoding_metrics.h"
#ifdef HAVE_X264
#include "h264_encoder.h"
#endif
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>

typedef struct tv_connection {
    int fd;
    bool active;
    x11_streamer_t *streamer;  // Reference to streamer for frame sending
    RROutput virtual_output_id;  // Virtual XR output created for this TV
    char display_name[64];
} tv_connection_t;

struct x11_streamer {
    int tv_fd;  // Connection to TV receiver
    char *tv_host;
    int tv_port;
    bool use_broadcast;  // Whether to use broadcast discovery
    int broadcast_timeout_ms;  // Broadcast discovery timeout
    bool running;
    x11_context_t *x11_ctx;
    tv_connection_t *tv_conn;  // Single TV connection
    pthread_mutex_t tv_mutex;
    pthread_t tv_thread;
    audio_capture_t *audio_capture;
    int refresh_rate_hz;  // Display refresh rate for frame throttling
    uint64_t last_frame_time_us;  // Last frame capture time (microseconds)
    dirty_rect_context_t *dirty_rect_ctx;  // For dirty rectangle detection
    uint8_t encoding_mode;  // Current encoding mode (0=full, 1=dirty rects, 2=H.264)
    encoding_metrics_t *metrics;  // Metrics for adaptive switching
#ifdef HAVE_X264
    h264_encoder_t *h264_encoder;  // H.264 encoder (when mode=2)
#endif
};

// Helper functions for encrypted/unencrypted protocol operations
static inline int streamer_send_message(x11_streamer_t *streamer, message_type_t type, const void *data, size_t data_len)
{
    if (streamer->noise_ctx && noise_encryption_is_ready(streamer->noise_ctx)) {
        return protocol_send_message_encrypted(streamer->noise_ctx, streamer->tv_fd, type, data, data_len);
    } else {
        return protocol_send_message(streamer->tv_fd, type, data, data_len);
    }
}

static inline int streamer_receive_message(x11_streamer_t *streamer, message_header_t *header, void **payload)
{
    if (streamer->noise_ctx && noise_encryption_is_ready(streamer->noise_ctx)) {
        return protocol_receive_message_encrypted(streamer->noise_ctx, streamer->tv_fd, header, payload);
    } else {
        return protocol_receive_message(streamer->tv_fd, header, payload);
    }
}

static void *tv_receiver_thread(void *arg)
{
    x11_streamer_t *streamer = (x11_streamer_t *)arg;
    message_header_t header;
    void *payload = NULL;

    // TV receiver sends HELLO first
    // Validate protocol by checking the first message

    // Receive TV HELLO with timeout to detect invalid protocols
    struct pollfd pfd = {.fd = streamer->tv_fd, .events = POLLIN};
    int poll_ret = poll(&pfd, 1, 2000);  // 2 second timeout
    if (poll_ret <= 0) {
        if (poll_ret == 0) {
            fprintf(stderr, "TV receiver handshake timeout (no response or invalid protocol)\n");
        } else {
            perror("poll");
        }
        goto cleanup;
    }

    int ret = streamer_receive_message(streamer, &header, &payload);
    if (ret <= 0) {
        fprintf(stderr, "TV receiver handshake failed: connection closed or invalid data\n");
        goto cleanup;
    }

    // Validate protocol: must be MSG_HELLO
    if (header.type != MSG_HELLO) {
        fprintf(stderr, "TV receiver protocol mismatch: expected MSG_HELLO (0x%02x), got 0x%02x\n",
                MSG_HELLO, header.type);
        if (payload)
            free(payload);
        goto cleanup;
    }

    // Validate HELLO message structure
    if (!payload || header.length < sizeof(hello_message_t)) {
        fprintf(stderr, "TV receiver handshake failed: invalid HELLO message format\n");
        if (payload)
            free(payload);
        goto cleanup;
    }

    if (payload && header.length >= sizeof(hello_message_t)) {
        hello_message_t *hello_msg = (hello_message_t *)payload;
        char *display_name = NULL;
        display_mode_t *modes = NULL;

        // Parse display name (comes right after hello_message_t)
        if (hello_msg->display_name_len > 0 &&
            hello_msg->display_name_len < 256 &&
            header.length >= sizeof(hello_message_t) + hello_msg->display_name_len) {

            display_name = (char *)payload + sizeof(hello_message_t);

            // Ensure null termination
            if (display_name[hello_msg->display_name_len - 1] != '\0') {
                // Not null-terminated, allocate and copy
                char *name_buf = calloc(1, hello_msg->display_name_len + 1);
                if (name_buf) {
                    memcpy(name_buf, display_name, hello_msg->display_name_len);
                    display_name = name_buf;
                } else {
                    display_name = NULL;
                }
            }
        }

        // Parse display modes (come after display name)
        size_t offset = sizeof(hello_message_t) + hello_msg->display_name_len;
        if (hello_msg->num_modes > 0 &&
            header.length >= offset + hello_msg->num_modes * sizeof(display_mode_t)) {
            modes = (display_mode_t *)((char *)payload + offset);
        }

        printf("TV receiver connected: version=%d, display='%s', modes=%d\n",
               hello_msg->protocol_version,
               display_name ? display_name : "(unknown)", hello_msg->num_modes);

        // Store display name
        char tv_display_name[64];
        if (display_name) {
            strncpy(tv_display_name, display_name, sizeof(tv_display_name) - 1);
            tv_display_name[sizeof(tv_display_name) - 1] = '\0';
        } else {
            snprintf(tv_display_name, sizeof(tv_display_name), "TV Display");
        }

        // Create virtual output using the TV's display name
        RROutput virtual_output_id = None;
        if (modes && hello_msg->num_modes > 0) {
            // Use the first/preferred mode to create virtual output
            display_mode_t *preferred_mode = &modes[0];

            // Create virtual output with the exact display name (no "XR-" prefix)
            virtual_output_id = x11_context_create_virtual_output(
                streamer->x11_ctx,
                tv_display_name,  // Use display name directly
                preferred_mode->width,
                preferred_mode->height,
                preferred_mode->refresh_rate / 100  // Convert from Hz*100 to Hz
            );

            if (virtual_output_id != None) {
                int refresh_rate = preferred_mode->refresh_rate / 100;
                printf("Created virtual output: '%s' (%dx%d@%dHz)\n",
                       tv_display_name, preferred_mode->width,
                       preferred_mode->height, refresh_rate);
                // Refresh outputs to get the new virtual output
                x11_context_refresh_outputs(streamer->x11_ctx);

                // Set all modes from TV receiver (not just the first one)
                if (hello_msg->num_modes > 1) {
                    int *widths = malloc(hello_msg->num_modes * sizeof(int));
                    int *heights = malloc(hello_msg->num_modes * sizeof(int));
                    int *refresh_rates = malloc(hello_msg->num_modes * sizeof(int));

                    if (widths && heights && refresh_rates) {
                        for (int i = 0; i < hello_msg->num_modes; i++) {
                            widths[i] = modes[i].width;
                            heights[i] = modes[i].height;
                            refresh_rates[i] = modes[i].refresh_rate / 100;  // Convert from Hz*100 to Hz
                        }

                        x11_context_set_virtual_output_modes(streamer->x11_ctx, virtual_output_id,
                                                            widths, heights, refresh_rates,
                                                            hello_msg->num_modes);
                        printf("Set %d modes for virtual output '%s'\n",
                               hello_msg->num_modes, tv_display_name);

                        free(widths);
                        free(heights);
                        free(refresh_rates);
                    }
                }

                pthread_mutex_lock(&streamer->tv_mutex);
                if (streamer->tv_conn) {
                    streamer->tv_conn->virtual_output_id = virtual_output_id;
                    strncpy(streamer->tv_conn->display_name, tv_display_name,
                           sizeof(streamer->tv_conn->display_name) - 1);
                    streamer->tv_conn->display_name[sizeof(streamer->tv_conn->display_name) - 1] = '\0';
                }
                streamer->refresh_rate_hz = refresh_rate;
                streamer->last_frame_time_us = 0;
                pthread_mutex_unlock(&streamer->tv_mutex);
            } else {
                printf("Failed to create virtual output for TV receiver\n");
            }
        }

        // Free display name if we allocated it
        if (display_name && display_name != (char *)payload + sizeof(hello_message_t)) {
            free(display_name);
        }

        free(payload);
        payload = NULL;

        // Start audio capture after connection is established
        if (streamer->audio_capture) {
            if (audio_capture_start(streamer->audio_capture) == 0) {
                printf("Audio capture started\n");
            } else {
                fprintf(stderr, "Failed to start audio capture\n");
            }
        }
    } else {
        if (payload)
            free(payload);
        payload = NULL;
    }

    // Main TV receiver communication loop
    while (streamer->running) {
        ret = protocol_receive_message(streamer->tv_fd, &header, &payload);
        if (ret <= 0) {
            if (ret == 0)
                printf("TV receiver disconnected\n");
            break;
        }

        switch (header.type) {
        case MSG_PING:
            streamer_send_message(streamer, MSG_PONG, NULL, 0);
            break;

        default:
            printf("Unknown message type from TV receiver: %d\n", header.type);
            break;
        }

        if (payload) {
            free(payload);
            payload = NULL;
        }
    }

cleanup:
    if (payload)
        free(payload);

    // Clean up virtual output if it was created
    pthread_mutex_lock(&streamer->tv_mutex);
    if (streamer->tv_conn && streamer->tv_conn->virtual_output_id != None) {
        x11_context_delete_virtual_output(streamer->x11_ctx, streamer->tv_conn->virtual_output_id);
        streamer->tv_conn->virtual_output_id = None;
    }
    pthread_mutex_unlock(&streamer->tv_mutex);

    if (streamer->tv_fd >= 0) {
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
    }

    streamer->running = false;
    return NULL;
}

static void streamer_send_frame_to_tv(x11_streamer_t *streamer,
                                      output_info_t *output, drm_fb_t *fb)
{
    if (!streamer->running || streamer->tv_fd < 0 || !output || !fb)
        return;

    uint64_t encoding_start_us = audio_get_timestamp_us();
    uint8_t encoding_mode = streamer->encoding_mode;
    const void *frame_data = NULL;
    size_t frame_data_size = 0;

    // Get frame data (need mapped data for dirty rectangle detection)
    if (fb->map) {
        frame_data = fb->map;
        frame_data_size = fb->size;
    } else if (fb->dma_fd >= 0) {
        // For DMA-BUF, we can't easily do dirty rectangle detection
        // Fall back to full frame mode
        encoding_mode = ENCODING_MODE_FULL_FRAME;
    } else {
        return;  // No data available
    }

    // Detect dirty rectangles if enabled
    dirty_rect_t dirty_rects[64];  // Max 64 rectangles
    int num_dirty_rects = 0;
    uint64_t total_dirty_pixels = 0;

    if (encoding_mode == ENCODING_MODE_DIRTY_RECTS && streamer->dirty_rect_ctx && frame_data) {
        // Ensure dirty rect context matches current frame size
        if (!streamer->dirty_rect_ctx ||
            dirty_rect_get_width(streamer->dirty_rect_ctx) != fb->width ||
            dirty_rect_get_height(streamer->dirty_rect_ctx) != fb->height) {
            if (streamer->dirty_rect_ctx)
                dirty_rect_destroy(streamer->dirty_rect_ctx);
            streamer->dirty_rect_ctx = dirty_rect_create(fb->width, fb->height, fb->bpp);
        }

        if (streamer->dirty_rect_ctx) {
            num_dirty_rects = dirty_rect_detect(streamer->dirty_rect_ctx, frame_data,
                                                dirty_rects, 64);

            // Calculate total dirty pixels
            for (int i = 0; i < num_dirty_rects; i++) {
                total_dirty_pixels += dirty_rects[i].width * dirty_rects[i].height;
            }

            // If dirty region is too large (>50%), fall back to full frame
            uint64_t total_pixels = fb->width * fb->height;
            if (total_dirty_pixels > total_pixels / 2) {
                encoding_mode = ENCODING_MODE_FULL_FRAME;
                num_dirty_rects = 0;
            }
        }
    }

    // Prepare frame message
    frame_message_t frame = {
        .timestamp_us = audio_get_timestamp_us(),
        .output_id = output->output_id,
        .width = fb->width,
        .height = fb->height,
        .format = fb->format,
        .pitch = fb->pitch,
        .encoding_mode = encoding_mode,
        .num_regions = num_dirty_rects
    };

    // H.264 encoding if enabled
    void *h264_data = NULL;
    size_t h264_size = 0;

#ifdef HAVE_X264
    if (encoding_mode == ENCODING_MODE_H264 && frame_data) {
        // Ensure H.264 encoder matches current frame size
        if (!streamer->h264_encoder ||
            h264_encoder_get_width(streamer->h264_encoder) != fb->width ||
            h264_encoder_get_height(streamer->h264_encoder) != fb->height) {
            if (streamer->h264_encoder)
                h264_encoder_destroy(streamer->h264_encoder);
            streamer->h264_encoder = h264_encoder_create(fb->width, fb->height,
                                                         streamer->refresh_rate_hz, 0);
            if (!streamer->h264_encoder) {
                fprintf(stderr, "Failed to create H.264 encoder, falling back to full frame\n");
                encoding_mode = ENCODING_MODE_FULL_FRAME;
            }
        }

        if (streamer->h264_encoder) {
            if (h264_encoder_encode_frame(streamer->h264_encoder, frame_data,
                                        &h264_data, &h264_size) == 0) {
                frame.size = h264_size;
            } else {
                fprintf(stderr, "H.264 encoding failed, falling back to full frame\n");
                encoding_mode = ENCODING_MODE_FULL_FRAME;
                if (h264_data) {
                    free(h264_data);
                    h264_data = NULL;
                }
            }
        }
    }
#endif

    // Calculate total data size
    if (encoding_mode == ENCODING_MODE_DIRTY_RECTS && num_dirty_rects > 0) {
        // Size = dirty rectangle headers + pixel data for each rectangle
        frame.size = num_dirty_rects * sizeof(dirty_rectangle_t);
        for (int i = 0; i < num_dirty_rects; i++) {
            uint32_t rect_size = dirty_rects[i].width * dirty_rects[i].height * fb->bpp;
            frame.size += rect_size;
        }
    } else if (encoding_mode == ENCODING_MODE_H264 && h264_data) {
        // H.264 encoded data size already set above
    } else {
        // Full frame
        frame.size = fb->size;
    }

    // Send frame header
    if (streamer_send_message(streamer, MSG_FRAME, &frame, sizeof(frame)) < 0) {
        printf("Failed to send frame to TV receiver\n");
        streamer->running = false;
        return;
    }

    // Send frame data
#ifdef HAVE_X264
    if (encoding_mode == ENCODING_MODE_H264 && h264_data && h264_size > 0) {
        // Send H.264 encoded data
        if (send(streamer->tv_fd, h264_data, h264_size, MSG_NOSIGNAL) != (ssize_t)h264_size) {
            printf("Failed to send H.264 data\n");
        }
        free(h264_data);
    } else
#endif
    if (encoding_mode == ENCODING_MODE_DIRTY_RECTS && num_dirty_rects > 0 && frame_data) {
        // Send dirty rectangles
        for (int i = 0; i < num_dirty_rects; i++) {
            dirty_rectangle_t rect_msg = {
                .x = dirty_rects[i].x,
                .y = dirty_rects[i].y,
                .width = dirty_rects[i].width,
                .height = dirty_rects[i].height,
                .data_size = dirty_rects[i].width * dirty_rects[i].height * fb->bpp
            };

            // Send rectangle header
            if (send(streamer->tv_fd, &rect_msg, sizeof(rect_msg), MSG_NOSIGNAL) != sizeof(rect_msg)) {
                printf("Failed to send dirty rectangle header\n");
                return;
            }

            // Send rectangle pixel data
            const uint8_t *src = (const uint8_t *)frame_data +
                                 (dirty_rects[i].y * fb->pitch + dirty_rects[i].x * fb->bpp);
            size_t rect_pitch = dirty_rects[i].width * fb->bpp;

            for (uint32_t y = 0; y < dirty_rects[i].height; y++) {
                if (send(streamer->tv_fd, src + y * fb->pitch, rect_pitch, MSG_NOSIGNAL) != (ssize_t)rect_pitch) {
                    printf("Failed to send dirty rectangle data\n");
                    return;
                }
            }
        }
    } else if (fb->dma_fd >= 0) {
        // Send DMA-BUF FD via ancillary data (full frame only)
        struct msghdr msg = {0};
        struct iovec iov;
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr *cmsg;

        iov.iov_base = &fb->dma_fd;
        iov.iov_len = sizeof(int);

        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = buf;
        msg.msg_controllen = sizeof(buf);

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        *(int *)CMSG_DATA(cmsg) = fb->dma_fd;

        msg.msg_controllen = cmsg->cmsg_len;

        if (sendmsg(streamer->tv_fd, &msg, 0) < 0) {
            printf("Failed to send DMA-BUF FD: %s\n", strerror(errno));
        }
    } else if (frame_data) {
        // Send mapped data (full frame)
        send(streamer->tv_fd, frame_data, frame_data_size, MSG_NOSIGNAL);
    }

    // Calculate encoding time and bytes sent
    uint64_t encoding_end_us = audio_get_timestamp_us();
    uint64_t encoding_time_us = encoding_end_us - encoding_start_us;
    uint64_t bytes_sent = sizeof(frame_message_t) + frame.size;

    // Calculate total pixels and dirty pixels for metrics
    uint64_t total_pixels = fb->width * fb->height;
    uint64_t dirty_pixels = 0;
    if (encoding_mode == ENCODING_MODE_DIRTY_RECTS && num_dirty_rects > 0) {
        dirty_pixels = total_dirty_pixels;
    } else {
        // Full frame - all pixels are "dirty"
        dirty_pixels = total_pixels;
    }

    // Record metrics
    if (streamer->metrics) {
        encoding_metrics_record_frame(streamer->metrics,
                                     bytes_sent,
                                     dirty_pixels,
                                     total_pixels,
                                     encoding_time_us,
                                     streamer->refresh_rate_hz);
    }

    // Check if we should switch encoding modes (adaptive switching)
    if (streamer->metrics && streamer->refresh_rate_hz > 0) {
        if (encoding_mode == ENCODING_MODE_DIRTY_RECTS) {
            // Check if we should switch to H.264
#ifdef HAVE_X264
            if (encoding_metrics_should_switch_to_h264(streamer->metrics, streamer->refresh_rate_hz)) {
                printf("Switching to H.264 mode (dirty region too large or bandwidth high)\n");
                streamer->encoding_mode = ENCODING_MODE_H264;
                encoding_metrics_reset(streamer->metrics);
            }
#else
            // H.264 not available, fall back to full frame
            if (encoding_metrics_should_switch_to_h264(streamer->metrics, streamer->refresh_rate_hz)) {
                printf("Switching to full frame mode (H.264 not available)\n");
                streamer->encoding_mode = ENCODING_MODE_FULL_FRAME;
                encoding_metrics_reset(streamer->metrics);
            }
#endif
        }
#ifdef HAVE_X264
        else if (encoding_mode == ENCODING_MODE_H264) {
            // Check if we should switch back to dirty rectangles
            if (encoding_metrics_should_switch_to_dirty_rects(streamer->metrics, streamer->refresh_rate_hz)) {
                printf("Switching to dirty rectangles mode (conditions improved)\n");
                streamer->encoding_mode = ENCODING_MODE_DIRTY_RECTS;
                encoding_metrics_reset(streamer->metrics);
            }
        }
#endif
        else if (encoding_mode == ENCODING_MODE_FULL_FRAME) {
            // Check if we should switch back to dirty rectangles
            if (encoding_metrics_should_switch_to_dirty_rects(streamer->metrics, streamer->refresh_rate_hz)) {
                printf("Switching to dirty rectangles mode (conditions improved)\n");
                streamer->encoding_mode = ENCODING_MODE_DIRTY_RECTS;
                encoding_metrics_reset(streamer->metrics);
            }
        }
    }

    // Log metrics periodically (every 60 frames = ~1 second at 60 FPS)
    static int log_counter = 0;
    if (streamer->metrics && ++log_counter >= 60) {
        log_counter = 0;
        printf("Metrics: FPS=%.1f, BW=%.1f MB/s, Dirty=%.1f%%, Mode=%d\n",
               encoding_metrics_get_fps(streamer->metrics),
               encoding_metrics_get_bandwidth_mbps(streamer->metrics),
               encoding_metrics_get_dirty_percent(streamer->metrics) * 100.0,
               streamer->encoding_mode);
    }
}

static void streamer_capture_and_send_frames(x11_streamer_t *streamer)
{
    if (!streamer || !streamer->x11_ctx || !streamer->x11_ctx->outputs || streamer->tv_fd < 0)
        return;

    pthread_mutex_lock(&streamer->tv_mutex);
    RROutput virtual_output_id = (streamer->tv_conn && streamer->tv_conn->virtual_output_id != None)
                                  ? streamer->tv_conn->virtual_output_id : None;
    pthread_mutex_unlock(&streamer->tv_mutex);

    if (virtual_output_id == None)
        return;  // No virtual output created yet

    // Capture frame from virtual output
    output_info_t *output = x11_context_find_output(streamer->x11_ctx, virtual_output_id);

    if (!output || !output->connected || output->framebuffer_id == 0)
        return;

    // Open framebuffer
    drm_fb_t *fb = drm_fb_open(output->framebuffer_id);
    if (!fb) {
        // Framebuffer might have changed, refresh outputs
        return;
    }

    // For dirty rectangles and H.264, we need mapped data, so always map
    // For full frame mode, prefer DMA-BUF for zero-copy
    if (streamer->encoding_mode == ENCODING_MODE_DIRTY_RECTS ||
        streamer->encoding_mode == ENCODING_MODE_H264) {
        // Always map for dirty rectangle detection or H.264 encoding
        if (drm_fb_map(fb) < 0) {
            drm_fb_close(fb);
            return;
        }
    } else {
        // Try to export as DMA-BUF (preferred method for full frame)
        if (drm_fb_export_dma_buf(fb) < 0) {
            // Fallback: map the framebuffer
            if (drm_fb_map(fb) < 0) {
                drm_fb_close(fb);
                return;
            }
        }
    }

    // Send frame to TV receiver
    streamer_send_frame_to_tv(streamer, output, fb);

    // Close framebuffer (TV receiver should have received the data/FD)
    drm_fb_close(fb);
}

static void streamer_capture_and_send_audio(x11_streamer_t *streamer)
{
    if (!streamer || !streamer->audio_capture || streamer->tv_fd < 0)
        return;

    void *audio_data = NULL;
    uint32_t audio_size = 0;

    int ret = audio_capture_read(streamer->audio_capture, &audio_data, &audio_size);
    if (ret > 0 && audio_data && audio_size > 0) {
        // Create audio message
        audio_message_t audio_msg = {
            .timestamp_us = audio_get_timestamp_us(),
            .sample_rate = 48000,
            .channels = 2,
            .format = AUDIO_FORMAT_PCM_S16LE,
            .data_size = audio_size
        };

        // Send audio header
        if (streamer_send_message(streamer, MSG_AUDIO, &audio_msg, sizeof(audio_msg)) < 0) {
            printf("Failed to send audio header\n");
            free(audio_data);
            return;
        }

        // Send audio data
        if (send(streamer->tv_fd, audio_data, audio_size, MSG_NOSIGNAL) != (ssize_t)audio_size) {
            printf("Failed to send audio data\n");
        }

        free(audio_data);
    } else if (ret < 0) {
        // Error reading audio
        if (audio_data)
            free(audio_data);
    }
}

static void streamer_check_and_notify_output_changes(x11_streamer_t *streamer)
{
    if (!streamer || !streamer->x11_ctx || !streamer->x11_ctx->outputs || streamer->tv_fd < 0)
        return;

    pthread_mutex_lock(&streamer->tv_mutex);
    RROutput virtual_output_id = (streamer->tv_conn && streamer->tv_conn->virtual_output_id != None)
                                  ? streamer->tv_conn->virtual_output_id : None;
    pthread_mutex_unlock(&streamer->tv_mutex);

    if (virtual_output_id == None)
        return;

    output_info_t *output = x11_context_find_output(streamer->x11_ctx, virtual_output_id);
    if (!output)
        return;

    // Check for resolution/refresh rate changes
    if (output->width != output->prev_width ||
        output->height != output->prev_height ||
        output->refresh_rate != output->prev_refresh_rate) {

        // Send CONFIG message to TV receiver
        config_message_t config = {
            .output_id = output->output_id,
            .width = output->width,
            .height = output->height,
            .refresh_rate = output->refresh_rate
        };

        streamer_send_message(streamer, MSG_CONFIG, &config, sizeof(config));
        printf("Sent CONFIG to TV receiver: %dx%d@%dHz\n",
               config.width, config.height, config.refresh_rate);
    }

    // Check for connection status changes (on/off)
    if (output->connected != output->prev_connected) {
        // Send CONFIG message with width=0, height=0 to indicate disconnect
        // or restore resolution to indicate reconnect
        config_message_t config = {
            .output_id = output->output_id,
            .width = output->connected ? output->width : 0,
            .height = output->connected ? output->height : 0,
            .refresh_rate = output->connected ? output->refresh_rate : 0
        };

        streamer_send_message(streamer, MSG_CONFIG, &config, sizeof(config));
        printf("Sent CONFIG to TV receiver: %s (output %s)\n",
               output->connected ? "connected" : "disconnected",
               output->name);
    }
}

x11_streamer_t *x11_streamer_create(const streamer_discovery_options_t *options)
{
    x11_streamer_t *streamer = calloc(1, sizeof(x11_streamer_t));
    if (!streamer)
        return NULL;

    // Set defaults if options not provided
    streamer_discovery_options_t opts;
    if (options) {
        opts = *options;
    } else {
        opts.use_broadcast = true;
        opts.host = NULL;
        opts.port = DEFAULT_TV_PORT;
        opts.broadcast_timeout_ms = 5000;
    }

    // If host is specified, disable broadcast
    if (opts.host) {
        streamer->tv_host = strdup(opts.host);
        streamer->use_broadcast = false;
    } else {
        streamer->tv_host = NULL;
        streamer->use_broadcast = opts.use_broadcast;
    }
    streamer->tv_port = opts.port;
    streamer->broadcast_timeout_ms = opts.broadcast_timeout_ms;
    streamer->tv_fd = -1;
    streamer->x11_ctx = x11_context_create();
    if (!streamer->x11_ctx) {
        free(streamer->tv_host);
        free(streamer);
        return NULL;
    }

    pthread_mutex_init(&streamer->tv_mutex, NULL);

    // Allocate TV connection structure
    streamer->tv_conn = calloc(1, sizeof(tv_connection_t));
    if (!streamer->tv_conn) {
        x11_context_destroy(streamer->x11_ctx);
        pthread_mutex_destroy(&streamer->tv_mutex);
        if (streamer->tv_host)
            free(streamer->tv_host);
        free(streamer);
        return NULL;
    }
    streamer->tv_conn->streamer = streamer;

    // Create audio capture (48kHz, stereo, 16-bit PCM for low latency)
    streamer->audio_capture = audio_capture_create(48000, 2, AUDIO_FORMAT_PCM_S16LE);
    if (!streamer->audio_capture) {
        fprintf(stderr, "Warning: Failed to create audio capture\n");
    }

    // Initialize encoding mode (default to dirty rectangles)
    streamer->encoding_mode = ENCODING_MODE_DIRTY_RECTS;
    streamer->dirty_rect_ctx = NULL;  // Will be created when we know frame size

#ifdef HAVE_X264
    streamer->h264_encoder = NULL;  // Will be created when needed
#endif

    // Create metrics tracker (60 frame window = 1 second at 60 FPS)
    streamer->metrics = encoding_metrics_create(60);
    if (!streamer->metrics) {
        fprintf(stderr, "Warning: Failed to create encoding metrics\n");
    }

    return streamer;
}

void x11_streamer_destroy(x11_streamer_t *streamer)
{
    if (!streamer)
        return;

    x11_streamer_stop(streamer);

    // Wait for TV receiver thread to finish
    if (streamer->tv_thread) {
        pthread_join(streamer->tv_thread, NULL);
        streamer->tv_thread = 0;
    }

    // Clean up virtual output
    if (streamer->tv_conn && streamer->tv_conn->virtual_output_id != None) {
        x11_context_delete_virtual_output(streamer->x11_ctx, streamer->tv_conn->virtual_output_id);
    }

    if (streamer->tv_fd >= 0)
        close(streamer->tv_fd);

    if (streamer->tv_conn)
        free(streamer->tv_conn);

    if (streamer->audio_capture)
        audio_capture_destroy(streamer->audio_capture);

    if (streamer->dirty_rect_ctx)
        dirty_rect_destroy(streamer->dirty_rect_ctx);

#ifdef HAVE_X264
    if (streamer->h264_encoder)
        h264_encoder_destroy(streamer->h264_encoder);
#endif

    if (streamer->metrics)
        encoding_metrics_destroy(streamer->metrics);

    if (streamer->x11_ctx)
        x11_context_destroy(streamer->x11_ctx);

    pthread_mutex_destroy(&streamer->tv_mutex);
    if (streamer->tv_host)
        free(streamer->tv_host);
    free(streamer);
}

// Discovery response structure
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int tcp_port;
    char display_name[256];
} discovery_response_info_t;

// Broadcast discovery: find TV receiver on all network interfaces using UDP
static int discover_tv_receiver(x11_streamer_t *streamer, struct in_addr *found_addr, int *found_port)
{
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("socket(UDP)");
        return -1;
    }

    // Enable broadcast
    int broadcast = 1;
    if (setsockopt(udp_fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        perror("setsockopt(SO_BROADCAST)");
        close(udp_fd);
        return -1;
    }

    // Bind to any port to receive responses
    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = 0;  // Let system choose port
    if (bind(udp_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        perror("bind(UDP)");
        close(udp_fd);
        return -1;
    }

    // Set timeout for receiving responses
    struct timeval timeout;
    timeout.tv_sec = streamer->broadcast_timeout_ms / 1000;
    timeout.tv_usec = (streamer->broadcast_timeout_ms % 1000) * 1000;
    if (setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt(SO_RCVTIMEO)");
        close(udp_fd);
        return -1;
    }

    // Build discovery request message
    message_header_t request_header = {
        .type = MSG_DISCOVERY_REQUEST,
        .length = 0,
        .sequence = 0
    };

    // Send broadcast on all interfaces
    struct ifaddrs *ifaddrs_list = NULL;
    if (getifaddrs(&ifaddrs_list) < 0) {
        perror("getifaddrs");
        close(udp_fd);
        return -1;
    }

    printf("Sending UDP broadcast discovery requests (port 4321)...\n");

    struct sockaddr_in broadcast_addr = {0};
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(4321);  // Default broadcast port

    int sent_count = 0;
    for (struct ifaddrs *ifa = ifaddrs_list; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        struct in_addr if_addr = sin->sin_addr;

        // Calculate broadcast address
        if (ifa->ifa_netmask) {
            struct sockaddr_in *netmask = (struct sockaddr_in *)ifa->ifa_netmask;
            broadcast_addr.sin_addr.s_addr = (if_addr.s_addr & netmask->sin_addr.s_addr) |
                                             ~(netmask->sin_addr.s_addr);
        } else {
            broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
        }

        sendto(udp_fd, &request_header, sizeof(request_header), 0,
               (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
        sent_count++;
    }
    freeifaddrs(ifaddrs_list);

    printf("Sent %d discovery requests, waiting for responses...\n", sent_count);

    // Collect responses
    discovery_response_info_t *responses = NULL;
    int response_count = 0;
    int response_capacity = 0;

    while (1) {
        uint8_t buffer[1024];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        ssize_t n = recvfrom(udp_fd, buffer, sizeof(buffer), 0,
                            (struct sockaddr *)&from_addr, &from_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;  // Timeout or interrupted
            }
            perror("recvfrom");
            break;
        }

        if (n < 9) continue;  // Too short for header

        message_header_t *header = (message_header_t *)buffer;
        if (header->type != MSG_DISCOVERY_RESPONSE) continue;

        // Parse discovery response
        if (n < (ssize_t)(9 + sizeof(discovery_response_t))) continue;

        discovery_response_t *resp = (discovery_response_t *)(buffer + 9);
        uint16_t tcp_port = resp->tcp_port;
        uint16_t name_len = resp->display_name_len;

        if (n < (ssize_t)(9 + sizeof(discovery_response_t) + name_len)) continue;

        char display_name[256] = {0};
        if (name_len > 0 && name_len < sizeof(display_name)) {
            memcpy(display_name, buffer + 9 + sizeof(discovery_response_t), name_len);
        }

        // Add to responses list
        if (response_count >= response_capacity) {
            response_capacity = response_capacity ? response_capacity * 2 : 8;
            responses = realloc(responses, response_capacity * sizeof(discovery_response_info_t));
        }

        inet_ntop(AF_INET, &from_addr.sin_addr, responses[response_count].ip, INET_ADDRSTRLEN);
        responses[response_count].tcp_port = tcp_port;
        strncpy(responses[response_count].display_name, display_name, sizeof(responses[response_count].display_name) - 1);
        response_count++;
    }

    close(udp_fd);

    if (response_count == 0) {
        printf("No TV receivers found.\n");
        return -1;
    }

    // Display found receivers
    printf("\nFound %d TV receiver(s):\n", response_count);
    for (int i = 0; i < response_count; i++) {
        printf("  %d. %s:%d - %s\n",
               i + 1, responses[i].ip, responses[i].tcp_port,
               responses[i].display_name[0] ? responses[i].display_name : "Unknown");
    }

    // Select receiver
    int selected = 0;
    if (response_count == 1) {
        selected = 0;
        printf("\nAuto-selecting the only receiver...\n");
    } else {
        printf("\nSelect receiver (1-%d): ", response_count);
        fflush(stdout);
        char line[32];
        if (fgets(line, sizeof(line), stdin)) {
            selected = atoi(line) - 1;
            if (selected < 0 || selected >= response_count) {
                printf("Invalid selection.\n");
                free(responses);
                return -1;
            }
        } else {
            printf("No selection made.\n");
            free(responses);
            return -1;
        }
    }

    // Set found address and port
    if (inet_aton(responses[selected].ip, found_addr) == 0) {
        printf("Invalid IP address: %s\n", responses[selected].ip);
        free(responses);
        return -1;
    }
    *found_port = responses[selected].tcp_port;

    printf("Selected: %s:%d\n", responses[selected].ip, *found_port);
    free(responses);
    return 0;
}

int x11_streamer_run(x11_streamer_t *streamer)
{
    if (!streamer)
        return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(streamer->tv_port);

    // Use broadcast discovery if enabled and no host specified
    if (streamer->use_broadcast && !streamer->tv_host) {
        struct in_addr found_addr;
        int found_port;
        if (discover_tv_receiver(streamer, &found_addr, &found_port) == 0) {
            addr.sin_addr = found_addr;
            addr.sin_port = htons(found_port);
            streamer->tv_port = found_port;
            char addr_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &found_addr, addr_str, sizeof(addr_str));
            if (streamer->tv_host)
                free(streamer->tv_host);
            streamer->tv_host = strdup(addr_str);
        } else {
            fprintf(stderr, "TV receiver not found via broadcast discovery\n");
            return -1;
        }
    } else if (streamer->tv_host) {
        // Direct connection to specified host
        if (inet_aton(streamer->tv_host, &addr.sin_addr) == 0) {
            // Try to resolve hostname
            struct hostent *he = gethostbyname(streamer->tv_host);
            if (!he) {
                fprintf(stderr, "Failed to resolve host: %s\n", streamer->tv_host);
                return -1;
            }
            memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        }
    } else {
        fprintf(stderr, "No host specified and broadcast disabled\n");
        return -1;
    }

    streamer->tv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (streamer->tv_fd < 0) {
        perror("socket");
        return -1;
    }

    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, addr_str, sizeof(addr_str));
    printf("Connecting to TV receiver at %s:%d...\n", addr_str, streamer->tv_port);

    if (connect(streamer->tv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    printf("Connected to TV receiver\n");

    // Always prompt user for PIN (displayed on TV receiver screen)
    printf("Enter PIN (4 digits, displayed on TV receiver): ");
    fflush(stdout);
    char pin_str[32];
    if (!fgets(pin_str, sizeof(pin_str), stdin)) {
        fprintf(stderr, "No PIN entered.\n");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }
    uint16_t pin = (uint16_t)atoi(pin_str);

    // Send PIN verification
    pin_verify_t pin_msg = {.pin = pin};
    if (protocol_send_message(streamer->tv_fd, MSG_PIN_VERIFY, &pin_msg, sizeof(pin_msg)) < 0) {
        fprintf(stderr, "Failed to send PIN verification\n");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    // Wait for PIN verified response
    message_header_t header;
    void *payload = NULL;
    if (protocol_receive_message(streamer->tv_fd, &header, &payload) <= 0 ||
        header.type != MSG_PIN_VERIFIED) {
        fprintf(stderr, "PIN verification failed\n");
        if (payload) free(payload);
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }
    if (payload) free(payload);
    printf("PIN verified successfully\n");

    // Start TV receiver thread to handle communication
    streamer->running = true;
    if (pthread_create(&streamer->tv_thread, NULL, tv_receiver_thread, streamer) != 0) {
        perror("pthread_create");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    // Refresh outputs
    if (x11_context_refresh_outputs(streamer->x11_ctx) < 0) {
        printf("Failed to refresh outputs\n");
        return -1;
    }

    printf("Found %d outputs\n", streamer->x11_ctx->num_outputs);

    // Get X11 display file descriptor for polling
    int x11_fd = x11_context_get_fd(streamer->x11_ctx);

    // Main streamer loop
    while (streamer->running) {
        struct pollfd pfds[1];
        int num_fds = 0;

        if (x11_fd >= 0) {
            pfds[num_fds].fd = x11_fd;
            pfds[num_fds].events = POLLIN;
            num_fds++;
        }

        int ret = poll(pfds, num_fds, 100);  // 100ms timeout
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        // Process X11/RandR events
        if (x11_fd >= 0 && ret > 0 && (pfds[0].revents & POLLIN)) {
            int changes = x11_context_process_events(streamer->x11_ctx);
            if (changes > 0) {
                // Output configuration changed, check for changes and notify TV receiver
                streamer_check_and_notify_output_changes(streamer);
            }
        }

        // Capture and send frames at display refresh rate
        static int refresh_counter = 0;

        uint64_t now_us = audio_get_timestamp_us();
        pthread_mutex_lock(&streamer->tv_mutex);
        int refresh_rate = streamer->refresh_rate_hz;
        uint64_t last_frame_time = streamer->last_frame_time_us;
        pthread_mutex_unlock(&streamer->tv_mutex);

        if (refresh_rate > 0) {
            // Calculate frame interval in microseconds
            uint64_t frame_interval_us = 1000000ULL / refresh_rate;

            // Only capture if enough time has passed since last frame
            if (now_us - last_frame_time >= frame_interval_us) {
                streamer_capture_and_send_frames(streamer);

                pthread_mutex_lock(&streamer->tv_mutex);
                streamer->last_frame_time_us = now_us;
                pthread_mutex_unlock(&streamer->tv_mutex);
            }
        } else {
            // Fallback: capture at ~10 FPS if refresh rate unknown
            static int frame_counter = 0;
            frame_counter++;
            if (frame_counter >= 10) {  // 100ms / 10 = 10 FPS
                streamer_capture_and_send_frames(streamer);
                frame_counter = 0;
            }
        }

        // Capture and send audio (low latency, frequent reads)
        streamer_capture_and_send_audio(streamer);

        // Refresh outputs occasionally (every 60 seconds)
        refresh_counter++;
        if (refresh_counter >= 60) {
            x11_context_refresh_outputs(streamer->x11_ctx);
            refresh_counter = 0;
        }
    }

    return 0;
}

void x11_streamer_stop(x11_streamer_t *streamer)
{
    if (streamer)
        streamer->running = false;
}

