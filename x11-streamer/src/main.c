#include "x11_streamer.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static x11_streamer_t *g_streamer = NULL;

static void signal_handler(int sig)
{
    (void)sig;
    if (g_streamer) {
        printf("\nShutting down X11 streamer...\n");
        x11_streamer_stop(g_streamer);
    }
}

int main(int argc, char *argv[])
{
    const char *tv_host = "localhost";
    int tv_port = DEFAULT_TV_PORT;

    if (argc > 1) {
        tv_host = argv[1];
    }
    if (argc > 2) {
        tv_port = atoi(argv[2]);
        if (tv_port <= 0 || tv_port > 65535) {
            fprintf(stderr, "Invalid port number: %s\n", argv[2]);
            return 1;
        }
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    g_streamer = x11_streamer_create(tv_host, tv_port);
    if (!g_streamer) {
        fprintf(stderr, "Failed to create X11 streamer\n");
        return 1;
    }

    printf("X11 Framebuffer Streamer connecting to TV receiver at %s:%d\n", tv_host, tv_port);
    printf("Press Ctrl+C to stop\n");

    int ret = x11_streamer_run(g_streamer);

    x11_streamer_destroy(g_streamer);
    return ret;
}

