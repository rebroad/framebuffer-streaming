#include "x11_streamer.h"
#include "x11_output.h"
#include "drm_fb.h"
#include "protocol.h"
#include "audio_capture.h"
#include "dirty_rect.h"
#include "encoding_metrics.h"
#include "noise_encryption.h"
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
#include <sys/select.h>

typedef struct tv_connection {
    int fd;
    bool active;
    x11_streamer_t *streamer;  // Reference to streamer for frame sending
    RROutput virtual_output_id;  // Virtual XR output created for this TV
    char display_name[64];
    bool paused;  // True when receiver has no surface (paused sending frames)
    // Pre-received HELLO message (when encryption is disabled, HELLO is received before thread starts)
    message_header_t hello_header;
    void *hello_payload;
} tv_connection_t;

struct x11_streamer {
    bool force_encrypt;
    bool force_no_encrypt;
    uint16_t pin;  // PIN from command line (0 if not provided)
    streamer_display_mode_t display_mode;  // Display mode: extend or mirror
    int tv_fd;  // Connection to TV receiver
    char *tv_host;
    int tv_port;
    bool use_broadcast;  // Whether to use broadcast discovery
    int broadcast_timeout_ms;  // Broadcast discovery timeout
    char *program_name;  // Program name (for error messages)
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
    bool enable_encryption;  // Whether encryption is enabled (from options)
    noise_encryption_context_t *noise_ctx;  // Noise Protocol encryption context
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

// Helper function to get PIN (from CLI or prompt)
static uint16_t get_pin(x11_streamer_t *streamer)
{
    if (streamer->pin != 0xFFFF) {
        return streamer->pin;  // Use PIN from command line
    }

    // Prompt for PIN
    printf("Enter PIN (4 digits, displayed on TV receiver): ");
    fflush(stdout);
    char pin_str[32];
    if (!fgets(pin_str, sizeof(pin_str), stdin)) {
        return 0xFFFF;  // Error - caller should check
    }
    uint16_t pin = (uint16_t)atoi(pin_str);
    if (pin > 9999) {
        return 0xFFFF;  // Invalid PIN
    }
    return pin;
}

// Helper function to send raw data (encrypted if available)
static inline int streamer_send_raw(x11_streamer_t *streamer, const void *data, size_t data_len)
{
    if (streamer->noise_ctx && noise_encryption_is_ready(streamer->noise_ctx)) {
        return noise_encryption_send(streamer->noise_ctx, streamer->tv_fd, data, data_len);
    } else {
        return send(streamer->tv_fd, data, data_len, MSG_NOSIGNAL) == (ssize_t)data_len ? 0 : -1;
    }
}

static void *tv_receiver_thread(void *arg)
{
    x11_streamer_t *streamer = (x11_streamer_t *)arg;
    message_header_t header;
    void *payload = NULL;

    // Check if HELLO was already received (when encryption is disabled)
    if (streamer->tv_conn && streamer->tv_conn->hello_payload) {
        // Use pre-received HELLO
        header = streamer->tv_conn->hello_header;
        payload = streamer->tv_conn->hello_payload;
        streamer->tv_conn->hello_payload = NULL;  // Clear so we don't free it twice
    } else {
        // TV receiver sends HELLO first (when encryption is enabled)
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
    }

    char *display_name = NULL;  // Declare outside block for cleanup
    if (payload && header.length >= sizeof(hello_message_t)) {
        hello_message_t *hello_msg = (hello_message_t *)payload;
        display_mode_t *modes = NULL;

        // Convert from network byte order
        hello_msg->protocol_version = ntohs(hello_msg->protocol_version);
        hello_msg->num_modes = ntohs(hello_msg->num_modes);
        hello_msg->display_name_len = ntohs(hello_msg->display_name_len);

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
            // Convert display modes from network byte order
            for (int i = 0; i < hello_msg->num_modes; i++) {
                modes[i].width = ntohl(modes[i].width);
                modes[i].height = ntohl(modes[i].height);
                modes[i].refresh_rate = ntohl(modes[i].refresh_rate);
            }
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

        // Handle display setup based on mode
        RROutput virtual_output_id = None;
        output_info_t *primary_output = NULL;

        if (streamer->display_mode == STREAMER_DISPLAY_MODE_MIRROR) {
            // Mirror mode: use primary display directly (no virtual output needed)
            primary_output = x11_context_get_primary_output(streamer->x11_ctx);
            if (!primary_output) {
                fprintf(stderr, "Error: Could not find primary display for mirroring\n");
                // Free and continue to cleanup
                if (display_name && display_name != (char *)payload + sizeof(hello_message_t)) {
                    free(display_name);
                }
                free(payload);
                return NULL;
            }

            if (!primary_output->connected || primary_output->framebuffer_id == 0) {
                fprintf(stderr, "Error: Primary display '%s' is not connected or has no framebuffer\n",
                       primary_output->name ? primary_output->name : "unknown");
                // Free and continue to cleanup
                if (display_name && display_name != (char *)payload + sizeof(hello_message_t)) {
                    free(display_name);
                }
                free(payload);
                return NULL;
            }

            printf("Mirroring primary display '%s' (%dx%d@%dHz) - no virtual output needed\n",
                   primary_output->name ? primary_output->name : "unknown",
                   primary_output->width, primary_output->height, primary_output->refresh_rate);

            // Store primary output ID for frame capture
            pthread_mutex_lock(&streamer->tv_mutex);
            if (streamer->tv_conn) {
                streamer->tv_conn->virtual_output_id = primary_output->output_id;  // Reuse field for primary output ID
                strncpy(streamer->tv_conn->display_name, tv_display_name,
                       sizeof(streamer->tv_conn->display_name) - 1);
                streamer->tv_conn->display_name[sizeof(streamer->tv_conn->display_name) - 1] = '\0';
            }
            streamer->refresh_rate_hz = primary_output->refresh_rate;
            streamer->last_frame_time_us = 0;
            pthread_mutex_unlock(&streamer->tv_mutex);
        } else {
            // Extend mode: create virtual output
            if (modes && hello_msg->num_modes > 0) {
                display_mode_t *preferred_mode = &modes[0];
                int output_width = preferred_mode->width;
                int output_height = preferred_mode->height;
                int output_refresh = preferred_mode->refresh_rate / 100;  // Convert from Hz*100 to Hz

                // Create virtual output with the exact display name (no "XR-" prefix)
                virtual_output_id = x11_context_create_virtual_output(
                    streamer->x11_ctx,
                    tv_display_name,  // Use display name directly
                    output_width,
                    output_height,
                    output_refresh
                );

                if (virtual_output_id != None) {
                    int refresh_rate = output_refresh;

                    // Find and print the TV receiver's virtual output
                    output_info_t *tv_output = NULL;
                    for (int i = 0; i < streamer->x11_ctx->num_outputs; i++) {
                        if (streamer->x11_ctx->outputs[i].output_id == virtual_output_id) {
                            tv_output = &streamer->x11_ctx->outputs[i];
                            break;
                        }
                    }

                    if (tv_output) {
                        printf("TV receiver virtual output: '%s' %dx%d@%dHz",
                               tv_output->name ? tv_output->name : tv_display_name,
                               tv_output->width, tv_output->height, refresh_rate);
                        if (hello_msg->num_modes > 1) {
                            printf(" (%d modes available)", hello_msg->num_modes);
                        }
                        printf("\n");
                    } else {
                        printf("Created virtual output: '%s' (%dx%d@%dHz)\n",
                               tv_display_name, output_width, output_height, refresh_rate);
                    }

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
            } else {
                fprintf(stderr, "Error: TV receiver sent no display modes\n");
            }
        }

        // Free display name if we allocated it
        if (display_name && display_name != (char *)payload + sizeof(hello_message_t)) {
            free(display_name);
            display_name = NULL;
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
        int ret = streamer_receive_message(streamer, &header, &payload);
        if (ret <= 0) {
            if (ret == 0)
                printf("TV receiver disconnected\n");
            break;
        }

        switch (header.type) {
        case MSG_PING:
            streamer_send_message(streamer, MSG_PONG, NULL, 0);
            break;

        case MSG_PAUSE:
            if (streamer->tv_conn) {
                streamer->tv_conn->paused = true;
                printf("TV receiver paused (no surface) - frame sending paused\n");
            }
            break;

        case MSG_RESUME:
            if (streamer->tv_conn) {
                streamer->tv_conn->paused = false;
                printf("TV receiver resumed (surface available) - frame sending resumed\n");
            }
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

    // Get frame data (framebuffer should already be mapped)
    if (!fb->map) {
        printf("Framebuffer not mapped\n");
        return;
    }
    frame_data = fb->map;
    frame_data_size = fb->size;

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

    // Prepare frame message (will convert to network byte order before sending)
    uint64_t timestamp_us = audio_get_timestamp_us();
    frame_message_t frame = {
        .timestamp_us = timestamp_us,  // uint64 - need manual conversion
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

    // Convert frame message to network byte order before sending
    frame_message_t frame_net = frame;
    // Convert uint64 timestamp (split into two uint32 and convert)
    uint32_t timestamp_low = (uint32_t)(frame.timestamp_us & 0xFFFFFFFF);
    uint32_t timestamp_high = (uint32_t)((frame.timestamp_us >> 32) & 0xFFFFFFFF);
    frame_net.timestamp_us = ((uint64_t)htonl(timestamp_high) << 32) | htonl(timestamp_low);
    frame_net.output_id = htonl(frame.output_id);
    frame_net.width = htonl(frame.width);
    frame_net.height = htonl(frame.height);
    frame_net.format = htonl(frame.format);
    frame_net.pitch = htonl(frame.pitch);
    frame_net.size = htonl(frame.size);

    // Send frame header
    if (streamer_send_message(streamer, MSG_FRAME, &frame_net, sizeof(frame_net)) < 0) {
        printf("Failed to send frame to TV receiver\n");
        streamer->running = false;
        return;
    }

    // Send frame data (encrypted if available)
#ifdef HAVE_X264
    if (encoding_mode == ENCODING_MODE_H264 && h264_data && h264_size > 0) {
        // Send H.264 encoded data
        if (streamer_send_raw(streamer, h264_data, h264_size) < 0) {
            printf("Failed to send H.264 data\n");
        }
        free(h264_data);
    } else
#endif
    if (encoding_mode == ENCODING_MODE_DIRTY_RECTS && num_dirty_rects > 0 && frame_data) {
        // Send dirty rectangles
        for (int i = 0; i < num_dirty_rects; i++) {
            dirty_rectangle_t rect_msg = {
                .x = htonl(dirty_rects[i].x),
                .y = htonl(dirty_rects[i].y),
                .width = htonl(dirty_rects[i].width),
                .height = htonl(dirty_rects[i].height),
                .data_size = htonl(dirty_rects[i].width * dirty_rects[i].height * fb->bpp)
            };

            // Send rectangle header
            if (streamer_send_raw(streamer, &rect_msg, sizeof(rect_msg)) < 0) {
                printf("Failed to send dirty rectangle header\n");
                return;
            }

            // Send rectangle pixel data
            const uint8_t *src = (const uint8_t *)frame_data +
                                 (dirty_rects[i].y * fb->pitch + dirty_rects[i].x * fb->bpp);
            size_t rect_pitch = dirty_rects[i].width * fb->bpp;

            for (uint32_t y = 0; y < dirty_rects[i].height; y++) {
                if (streamer_send_raw(streamer, src + y * fb->pitch, rect_pitch) < 0) {
                    printf("Failed to send dirty rectangle data\n");
                    return;
                }
            }
        }
    } else if (frame_data) {
        // Send mapped data (full frame)
        if (streamer_send_raw(streamer, frame_data, frame_data_size) < 0) {
            printf("Failed to send frame data\n");
        }
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

    // Map the framebuffer for CPU access (needed for network transmission)
    // Note: DMA-BUF zero-copy doesn't help here since we're sending over the network anyway
    // We just need to map the framebuffer memory (whether DMA-BUF or dumb buffer) and send pixel data
    if (drm_fb_map(fb) < 0) {
        drm_fb_close(fb);
        return;
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
        // Create audio message (convert to network byte order)
        uint64_t audio_timestamp_us = audio_get_timestamp_us();
        audio_message_t audio_msg = {
            .timestamp_us = audio_timestamp_us,  // uint64 - need manual conversion
            .sample_rate = 48000,
            .channels = 2,
            .format = AUDIO_FORMAT_PCM_S16LE,
            .data_size = audio_size
        };
        // Convert to network byte order
        uint32_t audio_ts_low = (uint32_t)(audio_msg.timestamp_us & 0xFFFFFFFF);
        uint32_t audio_ts_high = (uint32_t)((audio_msg.timestamp_us >> 32) & 0xFFFFFFFF);
        audio_msg.timestamp_us = ((uint64_t)htonl(audio_ts_high) << 32) | htonl(audio_ts_low);
        audio_msg.sample_rate = htonl(audio_msg.sample_rate);
        audio_msg.channels = htons(audio_msg.channels);
        audio_msg.format = htons(audio_msg.format);
        audio_msg.data_size = htonl(audio_msg.data_size);

        // Send audio header
        if (streamer_send_message(streamer, MSG_AUDIO, &audio_msg, sizeof(audio_msg)) < 0) {
            printf("Failed to send audio header\n");
            free(audio_data);
            return;
        }

        // Send audio data (encrypted if available)
        if (streamer_send_raw(streamer, audio_data, audio_size) < 0) {
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
            .output_id = htonl(output->output_id),
            .width = htonl(output->width),
            .height = htonl(output->height),
            .refresh_rate = htonl(output->refresh_rate)
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
            .output_id = htonl(output->output_id),
            .width = htonl(output->connected ? output->width : 0),
            .height = htonl(output->connected ? output->height : 0),
            .refresh_rate = htonl(output->connected ? output->refresh_rate : 0)
        };

        streamer_send_message(streamer, MSG_CONFIG, &config, sizeof(config));
        printf("Sent CONFIG to TV receiver: %s (output %s)\n",
               output->connected ? "connected" : "disconnected",
               output->name);
    }
}

x11_streamer_t *x11_streamer_create(const x11_streamer_options_t *options)
{
    x11_streamer_t *streamer = calloc(1, sizeof(x11_streamer_t));
    if (!streamer)
        return NULL;

    // Set defaults if options not provided
    x11_streamer_options_t opts;
    if (options) {
        opts = *options;
    } else {
        opts.use_broadcast = true;
        opts.host = NULL;
        opts.port = DEFAULT_TV_PORT;
        opts.broadcast_timeout_ms = 5000;
        opts.pin = 0xFFFF;  // No PIN provided
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
    streamer->force_encrypt = opts.force_encrypt;
    streamer->force_no_encrypt = opts.force_no_encrypt;
    streamer->pin = opts.pin;
    streamer->display_mode = opts.display_mode;
    // Store program name (extract basename if provided)
    if (opts.program_name) {
        const char *basename = strrchr(opts.program_name, '/');
        streamer->program_name = strdup(basename ? basename + 1 : opts.program_name);
    } else {
        streamer->program_name = strdup("x11-streamer");  // Default fallback
    }
    streamer->enable_encryption = true;  // Default: enabled, will be determined during handshake
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

    if (streamer->noise_ctx) {
        noise_encryption_cleanup(streamer->noise_ctx);
        streamer->noise_ctx = NULL;
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
    if (streamer->program_name)
        free(streamer->program_name);
    free(streamer);
}

// Discovery response structure
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int tcp_port;
    char display_name[256];
} discovery_response_info_t;

// Forward declarations for USB tethering functions
static bool enable_usb_tethering_via_adb(void);
static bool get_usb_tethering_ip_via_adb(char *ip_buf, size_t ip_buf_size);

// Broadcast discovery: find TV receiver on all network interfaces using UDP
static int discover_tv_receiver(x11_streamer_t *streamer, struct in_addr *found_addr, int *found_port)
{
    // Enable USB tethering before attempting broadcast discovery
    printf("Attempting to enable USB tethering on connected device...\n");
    enable_usb_tethering_via_adb();

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
        printf("No TV receivers found via broadcast discovery.\n");

        // Try to get USB tethering IP and inform the user
        char usb_ip[INET_ADDRSTRLEN] = {0};
        if (get_usb_tethering_ip_via_adb(usb_ip, sizeof(usb_ip))) {
            fprintf(stderr, "\nUSB tethering is available. Try connecting directly to: %s:%d\n",
                    usb_ip, streamer->tv_port);
            const char *prog_name = streamer->program_name ? streamer->program_name : "x11-streamer";
            fprintf(stderr, "Example: %s %s:%d\n", prog_name, usb_ip, streamer->tv_port);
        } else {
            fprintf(stderr, "\nCould not detect USB tethering IP address.\n");
            fprintf(stderr, "Please ensure USB tethering is enabled on your device, or specify the device IP as a positional argument.\n");
        }
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

// Helper function to check if TV receiver is listening via adb
static bool check_tv_receiver_listening(int port)
{
    // Try multiple methods to check if port is listening
    char cmd[512];

    // Method 1: netstat (older Android versions)
    snprintf(cmd, sizeof(cmd), "adb shell 'netstat -an 2>/dev/null | grep :%d | grep LISTEN' 2>/dev/null", port);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "LISTEN") || strstr(line, "tcp")) {
                pclose(fp);
                return true;
            }
        }
        pclose(fp);
    }

    // Method 2: ss (newer Android versions)
    snprintf(cmd, sizeof(cmd), "adb shell 'ss -tuln 2>/dev/null | grep :%d' 2>/dev/null", port);
    fp = popen(cmd, "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            pclose(fp);
            return true;
        }
        pclose(fp);
    }

    // Method 3: Check if app process is running
    snprintf(cmd, sizeof(cmd), "adb shell 'ps -A 2>/dev/null | grep framebuffer' 2>/dev/null");
    fp = popen(cmd, "r");
    if (fp) {
        char line[256];
        bool app_running = false;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "framebuffer") || strstr(line, "com.framebuffer")) {
                app_running = true;
                break;
            }
        }
        pclose(fp);
        if (app_running) {
            // App is running but port check failed - might be listening on different interface
            return false;  // Return false but we know app is running
        }
    }

    return false;  // Can't verify via adb
}

// Helper function to get all device IPs via adb
static int get_device_ips_via_adb(char ip_list[][16], int max_ips)
{
    // Get all IPv4 addresses from all interfaces
    FILE *fp = popen("adb shell 'ip -4 addr show | grep \"inet \" | awk \"{print \\$2}\" | cut -d/ -f1' 2>/dev/null", "r");
    if (!fp)
        return 0;

    int count = 0;
    char line[64];
    while (count < max_ips && fgets(line, sizeof(line), fp)) {
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        // Skip localhost
        if (strcmp(line, "127.0.0.1") != 0 && strlen(line) > 0) {
            strncpy(ip_list[count], line, 15);
            ip_list[count][15] = '\0';
            count++;
        }
    }
    pclose(fp);
    return count;
}

// Helper function to get device IP via adb (backward compatibility - returns WiFi IP)
static bool get_device_ip_via_adb(char *ip_buf, size_t ip_buf_size)
{
    FILE *fp = popen("adb shell 'ip -4 addr show wlan0 | grep inet | head -1 | awk \"{print \\$2}\" | cut -d/ -f1' 2>/dev/null", "r");
    if (fp) {
        if (fgets(ip_buf, ip_buf_size, fp)) {
            // Remove newline
            size_t len = strlen(ip_buf);
            if (len > 0 && ip_buf[len-1] == '\n') {
                ip_buf[len-1] = '\0';
            }
            pclose(fp);
            return strlen(ip_buf) > 0;
        }
        pclose(fp);
    }
    return false;
}

// Helper function to enable USB tethering via ADB
static bool enable_usb_tethering_via_adb(void)
{
    // First, try to set the USB function to RNDIS (USB tethering)
    // This may fail on non-rooted devices, but we try anyway
    int result = system("adb shell 'svc usb setFunctions rndis' 2>/dev/null");
    if (result == 0) {
        printf("USB tethering enabled via ADB\n");
        // Give it a moment to establish
        sleep(1);
        return true;
    }

    // If that fails, try setting the tether_dun_required setting
    // This might help on some devices
    system("adb shell 'settings put global tether_dun_required 0' 2>/dev/null");

    // Check if USB is already in RNDIS mode
    FILE *fp = popen("adb shell 'svc usb getFunctions' 2>/dev/null", "r");
    if (fp) {
        char line[64];
        if (fgets(line, sizeof(line), fp)) {
            pclose(fp);
            if (strstr(line, "rndis") != NULL) {
                printf("USB tethering already enabled (RNDIS mode detected)\n");
                return true;
            }
        } else {
            pclose(fp);
        }
    }

    // If we can't enable it directly, inform the user
    fprintf(stderr, "Warning: Could not enable USB tethering via ADB. "
                    "You may need to enable it manually on your device.\n");
    return false;
}

// Helper function to get USB tethering IP address via ADB
static bool get_usb_tethering_ip_via_adb(char *ip_buf, size_t ip_buf_size)
{
    // Check for USB tethering interface (typically usb0, rndis0, or similar)
    // Try common USB tethering interface names
    const char *usb_interfaces[] = {"usb0", "rndis0", "rndis", NULL};

    for (int i = 0; usb_interfaces[i] != NULL; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "adb shell 'ip -4 addr show %s 2>/dev/null | grep \"inet \" | head -1 | awk \"{print \\$2}\" | cut -d/ -f1' 2>/dev/null",
                 usb_interfaces[i]);

        FILE *fp = popen(cmd, "r");
        if (fp) {
            if (fgets(ip_buf, ip_buf_size, fp)) {
                // Remove newline
                size_t len = strlen(ip_buf);
                if (len > 0 && ip_buf[len-1] == '\n') {
                    ip_buf[len-1] = '\0';
                }
                pclose(fp);
                if (strlen(ip_buf) > 0 && strcmp(ip_buf, "127.0.0.1") != 0) {
                    return true;
                }
            }
            pclose(fp);
        }
    }

    // Fallback: look for any interface with "usb" or "rndis" in the name
    FILE *fp = popen("adb shell 'ip -4 addr show | grep -E \"(usb|rndis)\" -A 2 | grep \"inet \" | head -1 | awk \"{print \\$2}\" | cut -d/ -f1' 2>/dev/null", "r");
    if (fp) {
        if (fgets(ip_buf, ip_buf_size, fp)) {
            size_t len = strlen(ip_buf);
            if (len > 0 && ip_buf[len-1] == '\n') {
                ip_buf[len-1] = '\0';
            }
            pclose(fp);
            if (strlen(ip_buf) > 0 && strcmp(ip_buf, "127.0.0.1") != 0) {
                return true;
            }
        }
        pclose(fp);
    }

    return false;
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

    // Check if device is reachable via adb
    char device_ip[INET_ADDRSTRLEN] = {0};
    // Get all device IPs and check if target is reachable
    char device_ips[8][16];
    int num_ips = get_device_ips_via_adb(device_ips, 8);
    if (num_ips > 0) {
        printf("Debug: Device IPs (via adb): ");
        for (int i = 0; i < num_ips; i++) {
            printf("%s%s", device_ips[i], (i < num_ips - 1) ? ", " : "\n");
        }

        // Check if target IP is in the list
        bool target_found = false;
        for (int i = 0; i < num_ips; i++) {
            if (strcmp(addr_str, device_ips[i]) == 0) {
                target_found = true;
                break;
            }
        }

        if (!target_found) {
            fprintf(stderr, "Debug: Warning - Target IP (%s) not found in device IPs\n", addr_str);
            fprintf(stderr, "Debug: Try connecting to one of: %s\n", device_ips[0]);
            // Check if we can reach any of the device IPs from our network
            bool can_reach_any = false;
            for (int i = 0; i < num_ips; i++) {
                // Simple check: if IP is in same subnet as our interfaces
                struct ifaddrs *ifaddrs_list;
                if (getifaddrs(&ifaddrs_list) == 0) {
                    for (struct ifaddrs *ifa = ifaddrs_list; ifa != NULL; ifa = ifa->ifa_next) {
                        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
                            struct in_addr local_addr = sin->sin_addr;
                            struct in_addr target_addr;
                            if (inet_aton(device_ips[i], &target_addr) == 1) {
                                // Check if same subnet (simple heuristic: first 2 octets match)
                                if ((local_addr.s_addr & 0xFFFF0000) == (target_addr.s_addr & 0xFFFF0000)) {
                                    can_reach_any = true;
                                    fprintf(stderr, "Debug: Suggest using %s (same subnet as %s)\n",
                                           device_ips[i], ifa->ifa_name);
                                    break;
                                }
                            }
                        }
                        if (can_reach_any) break;
                    }
                    freeifaddrs(ifaddrs_list);
                }
                if (can_reach_any) break;
            }
        }
    }

    // Backward compatibility: also show WiFi IP
    bool device_reachable = get_device_ip_via_adb(device_ip, sizeof(device_ip));
    if (device_reachable && num_ips == 0) {
        printf("Debug: Device WiFi IP (via adb): %s\n", device_ip);
    }

    // Check if port is listening via adb
    bool port_listening = check_tv_receiver_listening(streamer->tv_port);
    if (port_listening) {
        printf("Debug: Port %d is listening on device (checked via adb)\n", streamer->tv_port);
    } else {
        fprintf(stderr, "Debug: Port %d is NOT listening on device (checked via adb)\n", streamer->tv_port);
    }

    // Set socket to non-blocking temporarily to check connection status
    int flags = fcntl(streamer->tv_fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl(F_GETFL)");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    if (fcntl(streamer->tv_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl(F_SETFL)");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    // Attempt connection
    int connect_result = connect(streamer->tv_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (connect_result < 0) {
        if (errno == EINPROGRESS) {
            // Connection in progress, wait for it
            fd_set write_fds;
            struct timeval timeout;
            FD_ZERO(&write_fds);
            FD_SET(streamer->tv_fd, &write_fds);
            timeout.tv_sec = 5;  // 5 second timeout
            timeout.tv_usec = 0;

            int select_result = select(streamer->tv_fd + 1, NULL, &write_fds, NULL, &timeout);
            if (select_result > 0) {
                // Check if connection succeeded
                int so_error;
                socklen_t len = sizeof(so_error);
                if (getsockopt(streamer->tv_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
                    perror("getsockopt(SO_ERROR)");
                    close(streamer->tv_fd);
                    streamer->tv_fd = -1;
                    return -1;
                }
                if (so_error != 0) {
                    errno = so_error;
                    fprintf(stderr, "Connection failed: ");
                    perror("connect");
                    fprintf(stderr, "Debug: Error code: %d (EHOSTUNREACH=%d, ECONNREFUSED=%d, ETIMEDOUT=%d)\n",
                           so_error, EHOSTUNREACH, ECONNREFUSED, ETIMEDOUT);
                    close(streamer->tv_fd);
                    streamer->tv_fd = -1;
                    return -1;
                }
                // Connection succeeded
            } else if (select_result == 0) {
                fprintf(stderr, "Connection timeout: No response from %s:%d\n", addr_str, streamer->tv_port);
                close(streamer->tv_fd);
                streamer->tv_fd = -1;
                return -1;
            } else {
                perror("select");
                close(streamer->tv_fd);
                streamer->tv_fd = -1;
                return -1;
            }
        } else {
            fprintf(stderr, "Connection failed: ");
            perror("connect");
            fprintf(stderr, "Debug: Error code: %d (EHOSTUNREACH=%d, ECONNREFUSED=%d, ETIMEDOUT=%d)\n",
                   errno, EHOSTUNREACH, ECONNREFUSED, ETIMEDOUT);
            close(streamer->tv_fd);
            streamer->tv_fd = -1;
            return -1;
        }
    }

    // Restore blocking mode
    if (fcntl(streamer->tv_fd, F_SETFL, flags) < 0) {
        perror("fcntl(F_SETFL restore)");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    printf("Connected to TV receiver\n");

    // Determine encryption policy: CLI override > interface detection
    bool wants_encryption = true;  // Default: encrypt
    bool needs_pin = true;  // Default: PIN required (unless rndis0)

    // Check if connecting via USB tethering (rndis0 interface)
    char usb_ip[INET_ADDRSTRLEN] = {0};
    bool is_usb_tethering = get_usb_tethering_ip_via_adb(usb_ip, sizeof(usb_ip)) &&
                            strcmp(addr_str, usb_ip) == 0;

    if (streamer->force_encrypt) {
        wants_encryption = true;
        printf("Encryption forced via --crypt\n");
    } else if (streamer->force_no_encrypt) {
        wants_encryption = false;
        printf("Encryption disabled via --nocrypt\n");
    } else if (is_usb_tethering) {
        wants_encryption = false;
        needs_pin = false;
        printf("USB tethering detected (rndis0) - using plaintext, no PIN\n");
    } else {
        wants_encryption = true;
        needs_pin = true;
        printf("WiFi/other interface - using encryption with PIN\n");
    }

    streamer->enable_encryption = wants_encryption;

    // Send CLIENT_HELLO as first message
    uint8_t client_hello_payload[4];  // version(1) + flags(1) + optional PIN(2)
    client_hello_payload[0] = 1;  // protocol version
    client_hello_payload[1] = wants_encryption ? 0x01 : 0x00;  // encryption flag

    size_t hello_payload_size = 2;  // version + flags

    uint16_t pin = 0xFFFF;
    if (!wants_encryption && needs_pin) {
        // PIN required in plaintext HELLO
        pin = get_pin(streamer);
        if (pin == 0xFFFF) {
            fprintf(stderr, "No PIN entered or invalid PIN.\n");
            close(streamer->tv_fd);
            streamer->tv_fd = -1;
            return -1;
        }
        uint16_t pin_be = htons(pin);
        client_hello_payload[2] = (pin_be >> 8) & 0xFF;  // High byte first
        client_hello_payload[3] = pin_be & 0xFF;         // Low byte second
        hello_payload_size = 4;
    }

    if (protocol_send_message(streamer->tv_fd, MSG_CLIENT_HELLO, client_hello_payload, hello_payload_size) < 0) {
        fprintf(stderr, "Failed to send CLIENT_HELLO\n");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }
    printf("Sent CLIENT_HELLO (encryption=%s)\n", wants_encryption ? "yes" : "no");

    // Perform Noise Protocol handshake if encryption requested
    if (wants_encryption) {
        printf("Starting Noise Protocol handshake...\n");
        streamer->noise_ctx = noise_encryption_init(true);  // Streamer is initiator
        if (!streamer->noise_ctx) {
            fprintf(stderr, "Failed to initialize Noise Protocol encryption\n");
            close(streamer->tv_fd);
            streamer->tv_fd = -1;
            return -1;
        }

        if (noise_encryption_handshake(streamer->noise_ctx, streamer->tv_fd) < 0) {
            fprintf(stderr, "Noise Protocol handshake failed\n");
            noise_encryption_cleanup(streamer->noise_ctx);
            streamer->noise_ctx = NULL;
            close(streamer->tv_fd);
            streamer->tv_fd = -1;
            return -1;
        }

        if (!noise_encryption_is_ready(streamer->noise_ctx)) {
            fprintf(stderr, "Noise Protocol handshake incomplete\n");
            noise_encryption_cleanup(streamer->noise_ctx);
            streamer->noise_ctx = NULL;
            close(streamer->tv_fd);
            streamer->tv_fd = -1;
            return -1;
        }

        printf("Noise Protocol encryption established\n");

        // Send PIN verification over encrypted channel if needed
        if (needs_pin) {
            pin = get_pin(streamer);
            if (pin == 0xFFFF) {
                fprintf(stderr, "No PIN entered or invalid PIN.\n");
                noise_encryption_cleanup(streamer->noise_ctx);
                streamer->noise_ctx = NULL;
                close(streamer->tv_fd);
                streamer->tv_fd = -1;
                return -1;
            }

            // Send PIN verification over encrypted channel
            pin_verify_t pin_msg = {.pin = htons(pin)};  // Convert to network byte order
            if (streamer_send_message(streamer, MSG_PIN_VERIFY, &pin_msg, sizeof(pin_msg)) < 0) {
                fprintf(stderr, "Failed to send PIN verification\n");
                noise_encryption_cleanup(streamer->noise_ctx);
                streamer->noise_ctx = NULL;
                close(streamer->tv_fd);
                streamer->tv_fd = -1;
                return -1;
            }

            // Wait for PIN verified response over encrypted channel
            message_header_t header;
            void *payload = NULL;
            if (streamer_receive_message(streamer, &header, &payload) <= 0 ||
                header.type != MSG_PIN_VERIFIED) {
                fprintf(stderr, "PIN verification failed\n");
                if (payload) free(payload);
                noise_encryption_cleanup(streamer->noise_ctx);
                streamer->noise_ctx = NULL;
                close(streamer->tv_fd);
                streamer->tv_fd = -1;
                return -1;
            }
            if (payload) free(payload);
            printf("PIN verified successfully\n");
        }
    } else {
        streamer->noise_ctx = NULL;
        printf("Using unencrypted connection\n");
    }

    // Receive HELLO message from receiver (display capabilities)
    printf("Waiting for HELLO message from TV receiver...\n");
    message_header_t hello_header;
    void *hello_payload = NULL;
    int hello_ret = protocol_receive_message(streamer->tv_fd, &hello_header, &hello_payload);
    if (hello_ret <= 0 || hello_header.type != MSG_HELLO) {
        fprintf(stderr, "TV receiver handshake failed: expected HELLO, got type 0x%02x\n",
                hello_ret > 0 ? hello_header.type : 0);
        if (hello_payload) free(hello_payload);
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    // Validate HELLO message structure
    if (!hello_payload || hello_header.length < sizeof(hello_message_t)) {
        fprintf(stderr, "TV receiver handshake failed: invalid HELLO message format\n");
        if (hello_payload) free(hello_payload);
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    // Store HELLO payload for tv_receiver_thread to process
    streamer->tv_conn = calloc(1, sizeof(tv_connection_t));
    if (!streamer->tv_conn) {
        fprintf(stderr, "Failed to allocate TV connection structure\n");
        if (hello_payload) free(hello_payload);
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }
    streamer->tv_conn->hello_payload = hello_payload;
    streamer->tv_conn->hello_header = hello_header;
    streamer->tv_conn->paused = false;

    // Start TV receiver thread to handle communication
    streamer->running = true;
    if (pthread_create(&streamer->tv_thread, NULL, tv_receiver_thread, streamer) != 0) {
        perror("pthread_create");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    // Refresh outputs (needed for X11 event processing, but we don't print local outputs)
    if (x11_context_refresh_outputs(streamer->x11_ctx) < 0) {
        printf("Failed to refresh outputs\n");
        return -1;
    }

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

