#ifndef X11_STREAMER_H
#define X11_STREAMER_H

#include <stdint.h>
#include <stdbool.h>

#define DEFAULT_TV_PORT 8888

typedef struct x11_streamer x11_streamer_t;

// Create X11 streamer that connects to TV receiver
x11_streamer_t *x11_streamer_create(const char *tv_host, int tv_port);
void x11_streamer_destroy(x11_streamer_t *streamer);
int x11_streamer_run(x11_streamer_t *streamer);
void x11_streamer_stop(x11_streamer_t *streamer);

#endif // X11_STREAMER_H

