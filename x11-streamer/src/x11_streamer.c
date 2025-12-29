#include "x11_streamer.h"
#include "x11_output.h"
#include "drm_fb.h"
#include "protocol.h"
#include "audio_capture.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>

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
    bool running;
    x11_context_t *x11_ctx;
    tv_connection_t *tv_conn;  // Single TV connection
    pthread_mutex_t tv_mutex;
    pthread_t tv_thread;
    audio_capture_t *audio_capture;
};

static void *tv_receiver_thread(void *arg)
{
    x11_streamer_t *streamer = (x11_streamer_t *)arg;
    message_header_t header;
    void *payload = NULL;

    // TV receiver sends HELLO first

    // Receive TV HELLO
    int ret = protocol_receive_message(streamer->tv_fd, &header, &payload);
    if (ret <= 0 || header.type != MSG_HELLO) {
        printf("TV receiver handshake failed\n");
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
                printf("Created virtual output: '%s' (%dx%d@%dHz)\n",
                       tv_display_name, preferred_mode->width,
                       preferred_mode->height, preferred_mode->refresh_rate / 100);
                // Refresh outputs to get the new virtual output
                x11_context_refresh_outputs(streamer->x11_ctx);

                pthread_mutex_lock(&streamer->tv_mutex);
                if (streamer->tv_conn) {
                    streamer->tv_conn->virtual_output_id = virtual_output_id;
                    strncpy(streamer->tv_conn->display_name, tv_display_name,
                           sizeof(streamer->tv_conn->display_name) - 1);
                    streamer->tv_conn->display_name[sizeof(streamer->tv_conn->display_name) - 1] = '\0';
                }
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
            protocol_send_message(streamer->tv_fd, MSG_PONG, NULL, 0);
            break;

        case MSG_INPUT:
            // TODO: Forward input to X server
            printf("Received input message from TV receiver\n");
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

    frame_message_t frame = {
        .timestamp_us = audio_get_timestamp_us(),
        .output_id = output->output_id,
        .width = fb->width,
        .height = fb->height,
        .format = fb->format,
        .pitch = fb->pitch,
        .size = fb->size
    };

    // Send frame header
    if (protocol_send_message(streamer->tv_fd, MSG_FRAME, &frame, sizeof(frame)) < 0) {
        printf("Failed to send frame to TV receiver\n");
        streamer->running = false;
        return;
    }

    // For now, if we have DMA-BUF FD, send it via SCM_RIGHTS
    // Otherwise, send the mapped data
    if (fb->dma_fd >= 0) {
        // Send DMA-BUF FD via ancillary data
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
    } else if (fb->map) {
        // Fallback: send mapped data
        send(streamer->tv_fd, fb->map, fb->size, MSG_NOSIGNAL);
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

    // Try to export as DMA-BUF (preferred method)
    if (drm_fb_export_dma_buf(fb) < 0) {
        // Fallback: map the framebuffer
        if (drm_fb_map(fb) < 0) {
            drm_fb_close(fb);
            return;
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
        if (protocol_send_message(streamer->tv_fd, MSG_AUDIO, &audio_msg, sizeof(audio_msg)) < 0) {
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

        protocol_send_message(streamer->tv_fd, MSG_CONFIG, &config, sizeof(config));
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

        protocol_send_message(streamer->tv_fd, MSG_CONFIG, &config, sizeof(config));
        printf("Sent CONFIG to TV receiver: %s (output %s)\n",
               output->connected ? "connected" : "disconnected",
               output->name);
    }
}

x11_streamer_t *x11_streamer_create(const char *tv_host, int tv_port)
{
    x11_streamer_t *streamer = calloc(1, sizeof(x11_streamer_t));
    if (!streamer)
        return NULL;

    streamer->tv_host = strdup(tv_host ? tv_host : "localhost");
    streamer->tv_port = tv_port;
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

    if (streamer->x11_ctx)
        x11_context_destroy(streamer->x11_ctx);

    pthread_mutex_destroy(&streamer->tv_mutex);
    if (streamer->tv_host)
        free(streamer->tv_host);
    free(streamer);
}

int x11_streamer_run(x11_streamer_t *streamer)
{
    if (!streamer)
        return -1;

    // Connect to TV receiver
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(streamer->tv_port);

    if (inet_aton(streamer->tv_host, &addr.sin_addr) == 0) {
        // Try to resolve hostname
        struct hostent *he = gethostbyname(streamer->tv_host);
        if (!he) {
            fprintf(stderr, "Failed to resolve host: %s\n", streamer->tv_host);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    streamer->tv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (streamer->tv_fd < 0) {
        perror("socket");
        return -1;
    }

    printf("Connecting to TV receiver at %s:%d...\n", streamer->tv_host, streamer->tv_port);

    if (connect(streamer->tv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(streamer->tv_fd);
        streamer->tv_fd = -1;
        return -1;
    }

    printf("Connected to TV receiver\n");

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

        // Capture and send frames periodically
        static int frame_counter = 0;
        static int refresh_counter = 0;

        frame_counter++;
        if (frame_counter >= 1) {  // Capture every poll iteration
            streamer_capture_and_send_frames(streamer);
            frame_counter = 0;
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

