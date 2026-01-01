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
    int width;
    int height;
    int refresh_rate;  // Hz
    bool connected;
    bool is_virtual;
    // Track previous state for change detection
    int prev_width;
    int prev_height;
    int prev_refresh_rate;
    bool prev_connected;
} output_info_t;

typedef struct x11_context {
    Display *display;
    Window root;
    int screen;
    XRRScreenResources *screen_resources;
    output_info_t *outputs;
    int num_outputs;
    int rr_event_base;
    int rr_error_base;
} x11_context_t;

x11_context_t *x11_context_create(void);
void x11_context_destroy(x11_context_t *ctx);
int x11_context_refresh_outputs(x11_context_t *ctx);
output_info_t *x11_context_find_output(x11_context_t *ctx, RROutput output_id);
output_info_t *x11_context_get_primary_output(x11_context_t *ctx);
void x11_context_free_outputs(x11_context_t *ctx);
RROutput x11_context_create_virtual_output(x11_context_t *ctx, const char *name,
                                            int width, int height, int refresh);
void x11_context_set_virtual_output_modes(x11_context_t *ctx, RROutput output_id,
                                          const int *widths, const int *heights,
                                          const int *refresh_rates, int num_modes);
void x11_context_delete_virtual_output(x11_context_t *ctx, RROutput output_id);
int x11_context_get_fd(x11_context_t *ctx);
int x11_context_process_events(x11_context_t *ctx);

#endif // X11_OUTPUT_H

