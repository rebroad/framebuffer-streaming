#ifndef X11_OUTPUT_H
#define X11_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

typedef struct output_info {
    RROutput output_id;
    char *name;
    uint32_t framebuffer_id;
    uint32_t pixmap_id;  // For virtual outputs
    int width;
    int height;
    bool connected;
    bool is_virtual;
} output_info_t;

typedef struct x11_context {
    Display *display;
    Window root;
    int screen;
    XRRScreenResources *screen_resources;
    output_info_t *outputs;
    int num_outputs;
} x11_context_t;

x11_context_t *x11_context_create(void);
void x11_context_destroy(x11_context_t *ctx);
int x11_context_refresh_outputs(x11_context_t *ctx);
output_info_t *x11_context_find_output(x11_context_t *ctx, RROutput output_id);
void x11_context_free_outputs(x11_context_t *ctx);

#endif // X11_OUTPUT_H

