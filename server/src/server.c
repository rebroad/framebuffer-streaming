#include "server.h"
#include "x11_output.h"
#include "drm_fb.h"
#include "protocol.h"
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

    // Send HELLO
    hello_message_t hello = {
        .protocol_version = 1,
        .client_type = 0,  // Will be set by client
        .capabilities = {0}
    };
    protocol_send_message(client->fd, MSG_HELLO, &hello, sizeof(hello));

    // Receive client HELLO
    int ret = protocol_receive_message(client->fd, &header, &payload);
    if (ret <= 0 || header.type != MSG_HELLO) {
        printf("Client handshake failed\n");
        goto cleanup;
    }

    if (payload) {
        hello_message_t *client_hello = (hello_message_t *)payload;
        printf("Client connected: type=%d, version=%d\n",
               client_hello->client_type, client_hello->protocol_version);
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

static void server_broadcast_frame(server_t *server, output_info_t *output, drm_fb_t *fb)
{
    pthread_mutex_lock(&server->clients_mutex);

    client_t *client = server->clients;
    while (client) {
        if (client->active) {
            server_send_frame_to_client(server, client, output, fb);
        }
        client = client->next;
    }

    pthread_mutex_unlock(&server->clients_mutex);
}

static void server_capture_and_broadcast_frames(server_t *server)
{
    if (!server || !server->x11_ctx || !server->x11_ctx->outputs)
        return;

    // Check if we have any active clients
    pthread_mutex_lock(&server->clients_mutex);
    bool has_clients = (server->clients != NULL);
    pthread_mutex_unlock(&server->clients_mutex);

    if (!has_clients)
        return;  // No clients connected, skip capture

    // Capture frames from all outputs
    for (int i = 0; i < server->x11_ctx->num_outputs; i++) {
        output_info_t *output = &server->x11_ctx->outputs[i];

        if (!output->connected || output->framebuffer_id == 0)
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

        // Broadcast to all clients
        server_broadcast_frame(server, output, fb);

        // Close framebuffer (clients should have received the data/FD)
        drm_fb_close(fb);
    }
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

    // Main server loop
    while (server->running) {
        struct pollfd pfd = {
            .fd = server->listen_fd,
            .events = POLLIN
        };

        int ret = poll(&pfd, 1, 1000);  // 1 second timeout
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
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

