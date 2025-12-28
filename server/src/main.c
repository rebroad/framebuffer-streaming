#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static server_t *g_server = NULL;

static void signal_handler(int sig)
{
    (void)sig;
    if (g_server) {
        printf("\nShutting down server...\n");
        server_stop(g_server);
    }
}

int main(int argc, char *argv[])
{
    int port = SERVER_DEFAULT_PORT;

    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number: %s\n", argv[1]);
            return 1;
        }
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    g_server = server_create(port);
    if (!g_server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    printf("X11 Framebuffer Server starting on port %d\n", port);
    printf("Press Ctrl+C to stop\n");

    int ret = server_run(g_server);

    server_destroy(g_server);
    return ret;
}

