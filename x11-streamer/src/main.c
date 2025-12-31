#include "x11_streamer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s [HOST:PORT] [OPTIONS]\n", prog_name);
    fprintf(stderr, "\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  HOST:PORT            Connect directly to HOST:PORT (e.g., 192.168.1.100:4321)\n");
    fprintf(stderr, "                       If omitted, uses broadcast discovery\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --port PORT          Port number for broadcast discovery (default: %d)\n", DEFAULT_TV_PORT);
    fprintf(stderr, "  --broadcast-timeout MS  Broadcast discovery timeout in milliseconds (default: 5000)\n");
    fprintf(stderr, "  --crypt              Force encryption for session (overrides autodetect)\n");
    fprintf(stderr, "  --nocrypt            Disable encryption for session (overrides autodetect)\n");
    fprintf(stderr, "  --pin PIN            PIN code (4 digits, avoids prompt)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s                           # Broadcast discovery on port %d\n", prog_name, DEFAULT_TV_PORT);
    fprintf(stderr, "  %s 192.168.1.100:4321        # Connect directly to IP:port\n", prog_name);
    fprintf(stderr, "  %s --port 8888               # Broadcast discovery on port 8888\n", prog_name);
    fprintf(stderr, "\n");
}

int main(int argc, char *argv[])
{
    streamer_discovery_options_t options = {
        .use_broadcast = true,  // Default: use broadcast
        .host = NULL,
        .port = DEFAULT_TV_PORT,
        .broadcast_timeout_ms = 5000,
        .program_name = argv[0],  // Pass program name for error messages
        .force_encrypt = false,
        .force_no_encrypt = false,
        .pin = 0xFFFF  // No PIN provided by default (0xFFFF = sentinel, valid PINs are 0-9999)
    };
    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --port requires an argument\n");
                print_usage(argv[0]);
                return 1;
            }
            options.port = atoi(argv[++i]);
            if (options.port <= 0 || options.port > 65535) {
                fprintf(stderr, "Error: Invalid port number: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--broadcast-timeout") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --broadcast-timeout requires an argument\n");
                print_usage(argv[0]);
                return 1;
            }
            options.broadcast_timeout_ms = atoi(argv[++i]);
            if (options.broadcast_timeout_ms <= 0) {
                fprintf(stderr, "Error: Invalid timeout: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--crypt") == 0) {
            options.force_encrypt = true;
        } else if (strcmp(argv[i], "--nocrypt") == 0) {
            options.force_no_encrypt = true;
        } else if (strcmp(argv[i], "--pin") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --pin requires an argument\n");
                print_usage(argv[0]);
                return 1;
            }
            int pin_val = atoi(argv[++i]);
            if (pin_val < 0 || pin_val > 9999) {
                fprintf(stderr, "Error: Invalid PIN (must be 0000-9999)\n");
                return 1;
            }
            options.pin = (uint16_t)pin_val;
        } else if (argv[i][0] != '-') {
            // Positional argument: HOST:PORT or HOST
            char *host_port = argv[i];
            char *colon = strchr(host_port, ':');

            if (colon) {
                // Format: HOST:PORT
                *colon = '\0';
                options.host = host_port;
                options.port = atoi(colon + 1);
                if (options.port <= 0 || options.port > 65535) {
                    fprintf(stderr, "Error: Invalid port number in %s\n", argv[i]);
                    return 1;
                }
                options.use_broadcast = false;  // Host specified, disable broadcast
            } else {
                // Format: HOST (use default port)
                options.host = host_port;
                options.port = DEFAULT_TV_PORT;
                options.use_broadcast = false;  // Host specified, disable broadcast
            }
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    // If host is specified, broadcast is automatically disabled
    // No validation needed - if host is NULL, broadcast will be used

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    g_streamer = x11_streamer_create(&options);
    if (!g_streamer) {
        fprintf(stderr, "Failed to create X11 streamer\n");
        return 1;
    }

    if (options.use_broadcast && !options.host) {
        printf("X11 Framebuffer Streamer: Broadcast discovery enabled (port %d)\n", options.port);
    } else {
        printf("X11 Framebuffer Streamer: Connecting to %s:%d\n", options.host, options.port);
    }
    printf("Press Ctrl+C to stop\n");

    int ret = x11_streamer_run(g_streamer);

    x11_streamer_destroy(g_streamer);
    return ret;
}

