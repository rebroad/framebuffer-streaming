#ifndef X11_STREAMER_H
#define X11_STREAMER_H

#include <stdint.h>
#include <stdbool.h>

#define DEFAULT_TV_PORT 4321

typedef struct x11_streamer x11_streamer_t;

// Discovery options
typedef struct {
    bool use_broadcast;      // Use broadcast discovery (default: true)
    const char *host;        // Direct host to connect to (if not NULL, disables broadcast)
    int port;                // Port number (default: DEFAULT_TV_PORT)
    int broadcast_timeout_ms; // Timeout for broadcast discovery in milliseconds (default: 5000)
    const char *program_name; // Program name (for error messages, extracted from argv[0])
    bool force_encrypt;      // Add for handshake flag
    bool force_no_encrypt;   // Add for handshake flag
    uint16_t pin;            // PIN from command line (0xFFFF if not provided, valid PINs are 0-9999)
} streamer_discovery_options_t;

// Create X11 streamer that connects to TV receiver
// If options->host is NULL and options->use_broadcast is true, uses broadcast discovery
// If options->host is set, connects directly to that host (broadcast disabled)
x11_streamer_t *x11_streamer_create(const streamer_discovery_options_t *options);
void x11_streamer_destroy(x11_streamer_t *streamer);
int x11_streamer_run(x11_streamer_t *streamer);
void x11_streamer_stop(x11_streamer_t *streamer);

#endif // X11_STREAMER_H
