#include "server.h"
#include "x11_output.h"
#include "drm_fb.h"
#include "protocol.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>

typedef struct client {
    int fd;
    bool active;
    pthread_t thread;
    struct client *next;
    server_t *server;  // Reference to server for frame sending
    RROutput virtual_output_id;  // Virtual XR output created for this client
    char client_name[64];
} client_t;

struct server {
    int listen_fd;
    int port;
    bool running;
    x11_context_t *x11_ctx;
    client_t *clients;
    pthread_mutex_t clients_mutex;
};

static void *client_thread(void *arg)
{
    client_t *client = (client_t *)arg;
    (void)client->server;  // Available for future use
    message_header_t header;
    void *payload = NULL;

    // Server doesn't send HELLO - client sends it first

    // Receive client HELLO
    int ret = protocol_receive_message(client->fd, &header, &payload);
    if (ret <= 0 || header.type != MSG_HELLO) {
        printf("Client handshake failed\n");
        goto cleanup;
    }

    if (payload && header.length >= sizeof(hello_message_t)) {
        hello_message_t *client_hello = (hello_message_t *)payload;
        char *display_name = NULL;
        display_mode_t *modes = NULL;

        // Parse display name (comes right after hello_message_t)
        if (client_hello->display_name_len > 0 &&
            client_hello->display_name_len < 256 &&
            header.length >= sizeof(hello_message_t) + client_hello->display_name_len) {

            display_name = (char *)payload + sizeof(hello_message_t);

            // Ensure null termination
            if (display_name[client_hello->display_name_len - 1] != '\0') {
                // Not null-terminated, allocate and copy
                char *name_buf = calloc(1, client_hello->display_name_len + 1);
                if (name_buf) {
                    memcpy(name_buf, display_name, client_hello->display_name_len);
                    display_name = name_buf;
                } else {
                    display_name = NULL;
                }
            }
        }

        // Parse display modes (come after display name)
        size_t offset = sizeof(hello_message_t) + client_hello->display_name_len;
        if (client_hello->num_modes > 0 &&
            header.length >= offset + client_hello->num_modes * sizeof(display_mode_t)) {
            modes = (display_mode_t *)((char *)payload + offset);
        }

        printf("Client connected: type=%d, version=%d, display='%s', modes=%d\n",
               client_hello->client_type, client_hello->protocol_version,
               display_name ? display_name : "(unknown)", client_hello->num_modes);

        // Store display name
        if (display_name) {
            strncpy(client->client_name, display_name,
                    sizeof(client->client_name) - 1);
            client->client_name[sizeof(client->client_name) - 1] = '\0';
        } else {
            snprintf(client->client_name, sizeof(client->client_name),
                    "Remote-Client-%d", client->fd);
        }

        // Create virtual output using the client's display name
        if (modes && client_hello->num_modes > 0) {
            // Use the first/preferred mode to create virtual output
            // TODO: Allow client to specify preferred mode or let server choose best match
            display_mode_t *preferred_mode = &modes[0];

            // Create virtual output with the exact display name (no "XR-" prefix)
            client->virtual_output_id = x11_context_create_virtual_output(
                client->server->x11_ctx,
                client->client_name,  // Use display name directly
                preferred_mode->width,
                preferred_mode->height,
                preferred_mode->refresh_rate / 100  // Convert from Hz*100 to Hz
            );

            if (client->virtual_output_id != None) {
                printf("Created virtual output: '%s' (%dx%d@%dHz)\n",
                       client->client_name, preferred_mode->width,
                       preferred_mode->height, preferred_mode->refresh_rate / 100);
                // Refresh outputs to get the new virtual output
                x11_context_refresh_outputs(client->server->x11_ctx);
            } else {
                printf("Failed to create virtual output for client\n");
            }
        }

        // Free display name if we allocated it
        if (display_name && display_name != (char *)payload + sizeof(hello_message_t)) {
            free(display_name);
        }

        free(payload);
        payload = NULL;
    } else {
        if (payload)
            free(payload);
        payload = NULL;
    }

    // Main client loop
    while (client->active) {
        ret = protocol_receive_message(client->fd, &header, &payload);
        if (ret <= 0) {
            if (ret == 0)
                printf("Client disconnected\n");
            break;
        }

        switch (header.type) {
        case MSG_PING:
            protocol_send_message(client->fd, MSG_PONG, NULL, 0);
            break;

        case MSG_INPUT:
            // TODO: Forward input to X server
            printf("Received input message\n");
            break;

        default:
            printf("Unknown message type: %d\n", header.type);
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

    close(client->fd);
    client->active = false;
    return NULL;
}

static void server_send_frame_to_client(server_t *server __attribute__((unused)),
                                         client_t *client,
                                         output_info_t *output, drm_fb_t *fb)
{
    if (!client->active || !output || !fb)
        return;

    frame_message_t frame = {
        .output_id = output->output_id,
        .width = fb->width,
        .height = fb->height,
        .format = fb->format,
        .pitch = fb->pitch,
        .size = fb->size
    };

    // Send frame header
    if (protocol_send_message(client->fd, MSG_FRAME, &frame, sizeof(frame)) < 0) {
        client->active = false;
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

        if (sendmsg(client->fd, &msg, 0) < 0) {
            printf("Failed to send DMA-BUF FD: %s\n", strerror(errno));
        }
    } else if (fb->map) {
        // Fallback: send mapped data
        send(client->fd, fb->map, fb->size, MSG_NOSIGNAL);
    }
}

static void server_capture_and_broadcast_frames(server_t *server)
{
    if (!server || !server->x11_ctx || !server->x11_ctx->outputs)
        return;

    // Check if we have any active clients and get their virtual output IDs
    pthread_mutex_lock(&server->clients_mutex);

    // Build list of virtual outputs to capture
    RROutput *output_ids = NULL;
    int num_outputs_to_capture = 0;
    client_t *client = server->clients;

    while (client) {
        if (client->active && client->virtual_output_id != None) {
            // Check if this output ID is already in our list
            bool found = false;
            for (int i = 0; i < num_outputs_to_capture; i++) {
                if (output_ids[i] == client->virtual_output_id) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                RROutput *new_list = realloc(output_ids,
                    (num_outputs_to_capture + 1) * sizeof(RROutput));
                if (new_list) {
                    output_ids = new_list;
                    output_ids[num_outputs_to_capture++] = client->virtual_output_id;
                }
            }
        }
        client = client->next;
    }

    pthread_mutex_unlock(&server->clients_mutex);

    if (num_outputs_to_capture == 0) {
        if (output_ids)
            free(output_ids);
        return;  // No virtual outputs to capture
    }

    // Capture frames from virtual outputs created for clients
    for (int i = 0; i < num_outputs_to_capture; i++) {
        output_info_t *output = x11_context_find_output(server->x11_ctx, output_ids[i]);

        if (!output || !output->connected || output->framebuffer_id == 0)
            continue;

        // Open framebuffer
        drm_fb_t *fb = drm_fb_open(output->framebuffer_id);
        if (!fb) {
            // Framebuffer might have changed, refresh outputs
            continue;
        }

        // Try to export as DMA-BUF (preferred method)
        if (drm_fb_export_dma_buf(fb) < 0) {
            // Fallback: map the framebuffer
            if (drm_fb_map(fb) < 0) {
                drm_fb_close(fb);
                continue;
            }
        }

        // Broadcast to clients that are using this virtual output
        pthread_mutex_lock(&server->clients_mutex);
        client = server->clients;
        while (client) {
            if (client->active && client->virtual_output_id == output_ids[i]) {
                server_send_frame_to_client(server, client, output, fb);
            }
            client = client->next;
        }
        pthread_mutex_unlock(&server->clients_mutex);

        // Close framebuffer (clients should have received the data/FD)
        drm_fb_close(fb);
    }

    if (output_ids)
        free(output_ids);
}

static void server_check_and_notify_output_changes(server_t *server)
{
    if (!server || !server->x11_ctx || !server->x11_ctx->outputs)
        return;

    pthread_mutex_lock(&server->clients_mutex);

    // Check each client's virtual output for changes
    client_t *client = server->clients;
    while (client) {
        if (client->active && client->virtual_output_id != None) {
            output_info_t *output = x11_context_find_output(server->x11_ctx,
                                                           client->virtual_output_id);

            if (output) {
                // Check for resolution/refresh rate changes
                if (output->width != output->prev_width ||
                    output->height != output->prev_height ||
                    output->refresh_rate != output->prev_refresh_rate) {

                    // Send CONFIG message to client
                    config_message_t config = {
                        .output_id = output->output_id,
                        .width = output->width,
                        .height = output->height,
                        .refresh_rate = output->refresh_rate
                    };

                    protocol_send_message(client->fd, MSG_CONFIG, &config, sizeof(config));
                    printf("Sent CONFIG to client: %dx%d@%dHz\n",
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

                    protocol_send_message(client->fd, MSG_CONFIG, &config, sizeof(config));
                    printf("Sent CONFIG to client: %s (output %s)\n",
                           output->connected ? "connected" : "disconnected",
                           output->name);
                }
            }
        }
        client = client->next;
    }

    pthread_mutex_unlock(&server->clients_mutex);
}

server_t *server_create(int port)
{
    server_t *server = calloc(1, sizeof(server_t));
    if (!server)
        return NULL;

    server->port = port;
    server->x11_ctx = x11_context_create();
    if (!server->x11_ctx) {
        free(server);
        return NULL;
    }

    pthread_mutex_init(&server->clients_mutex, NULL);

    // Create listening socket
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        x11_context_destroy(server->x11_ctx);
        pthread_mutex_destroy(&server->clients_mutex);
        free(server);
        return NULL;
    }

    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server->listen_fd);
        x11_context_destroy(server->x11_ctx);
        pthread_mutex_destroy(&server->clients_mutex);
        free(server);
        return NULL;
    }

    if (listen(server->listen_fd, 5) < 0) {
        close(server->listen_fd);
        x11_context_destroy(server->x11_ctx);
        pthread_mutex_destroy(&server->clients_mutex);
        free(server);
        return NULL;
    }

    return server;
}

void server_destroy(server_t *server)
{
    if (!server)
        return;

    server_stop(server);

    // Close all clients
    pthread_mutex_lock(&server->clients_mutex);
    client_t *client = server->clients;
    while (client) {
        client_t *next = client->next;
        if (client->active) {
            client->active = false;
            close(client->fd);
            pthread_join(client->thread, NULL);
        }
        free(client);
        client = next;
    }
    pthread_mutex_unlock(&server->clients_mutex);

    if (server->listen_fd >= 0)
        close(server->listen_fd);

    if (server->x11_ctx)
        x11_context_destroy(server->x11_ctx);

    pthread_mutex_destroy(&server->clients_mutex);
    free(server);
}

int server_run(server_t *server)
{
    if (!server)
        return -1;

    server->running = true;

    // Refresh outputs
    if (x11_context_refresh_outputs(server->x11_ctx) < 0) {
        printf("Failed to refresh outputs\n");
        return -1;
    }

    printf("Server listening on port %d\n", server->port);
    printf("Found %d outputs\n", server->x11_ctx->num_outputs);

    // Get X11 display file descriptor for polling
    int x11_fd = x11_context_get_fd(server->x11_ctx);

    // Main server loop
    while (server->running) {
        struct pollfd pfds[2];
        int num_fds = 0;

        pfds[num_fds].fd = server->listen_fd;
        pfds[num_fds].events = POLLIN;
        num_fds++;

        if (x11_fd >= 0) {
            pfds[num_fds].fd = x11_fd;
            pfds[num_fds].events = POLLIN;
            num_fds++;
        }

        int ret = poll(pfds, num_fds, 100);  // 100ms timeout for better responsiveness
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        // Process X11/RandR events
        if (x11_fd >= 0 && ret > 0 && (pfds[1].revents & POLLIN)) {
            int changes = x11_context_process_events(server->x11_ctx);
            if (changes > 0) {
                // Output configuration changed, check for changes and notify clients
                server_check_and_notify_output_changes(server);
            }
        }

        if (ret > 0 && (pfds[0].revents & POLLIN)) {
            // Accept new client
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(server->listen_fd,
                                   (struct sockaddr *)&client_addr,
                                   &addr_len);

            if (client_fd >= 0) {
                printf("New client connected from %s\n",
                       inet_ntoa(client_addr.sin_addr));

                client_t *client = calloc(1, sizeof(client_t));
                if (client) {
                    client->fd = client_fd;
                    client->active = true;
                    client->server = server;
                    client->next = server->clients;

                    pthread_mutex_lock(&server->clients_mutex);
                    server->clients = client;
                    pthread_mutex_unlock(&server->clients_mutex);

                    if (pthread_create(&client->thread, NULL,
                                       client_thread, client) != 0) {
                        pthread_mutex_lock(&server->clients_mutex);
                        server->clients = client->next;
                        pthread_mutex_unlock(&server->clients_mutex);
                        close(client_fd);
                        free(client);
                    }
                } else {
                    close(client_fd);
                }
            }
        }

        // Capture and broadcast frames periodically
        static int frame_counter = 0;
        static int refresh_counter = 0;

        // Capture frames at ~30 FPS (every ~33ms, but we poll every 1000ms, so every poll)
        // For better frame rate, we'd need a separate thread or use timerfd
        frame_counter++;
        if (frame_counter >= 1) {  // Capture every poll iteration (~1 FPS for now, can be optimized)
            server_capture_and_broadcast_frames(server);
            frame_counter = 0;
        }

        // Refresh outputs occasionally (every 60 seconds)
        refresh_counter++;
        if (refresh_counter >= 60) {
            x11_context_refresh_outputs(server->x11_ctx);
            refresh_counter = 0;
        }
    }

    return 0;
}

void server_stop(server_t *server)
{
    if (server)
        server->running = false;
}

