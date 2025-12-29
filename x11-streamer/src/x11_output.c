#include "x11_output.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <unistd.h>
#include <poll.h>

x11_context_t *x11_context_create(void)
{
    x11_context_t *ctx = calloc(1, sizeof(x11_context_t));
    if (!ctx)
        return NULL;

    ctx->display = XOpenDisplay(NULL);
    if (!ctx->display) {
        free(ctx);
        return NULL;
    }

    ctx->screen = DefaultScreen(ctx->display);
    ctx->root = RootWindow(ctx->display, ctx->screen);

    // Initialize RandR extension
    int rr_major, rr_minor;
    if (!XRRQueryExtension(ctx->display, &ctx->rr_event_base, &ctx->rr_error_base)) {
        printf("RandR extension not available\n");
        XCloseDisplay(ctx->display);
        free(ctx);
        return NULL;
    }

    if (!XRRQueryVersion(ctx->display, &rr_major, &rr_minor)) {
        printf("Failed to query RandR version\n");
        XCloseDisplay(ctx->display);
        free(ctx);
        return NULL;
    }

    // Select RandR events to monitor output and CRTC changes
    XRRSelectInput(ctx->display, ctx->root,
                   RROutputChangeNotifyMask | RRCrtcChangeNotifyMask |
                   RRScreenChangeNotifyMask);

    return ctx;
}

void x11_context_destroy(x11_context_t *ctx)
{
    if (!ctx)
        return;

    x11_context_free_outputs(ctx);

    if (ctx->screen_resources)
        XRRFreeScreenResources(ctx->screen_resources);

    if (ctx->display)
        XCloseDisplay(ctx->display);

    free(ctx);
}

static Atom get_atom(Display *display, const char *name)
{
    return XInternAtom(display, name, False);
}

static bool get_output_property(Display *display, RROutput output,
                                const char *prop_name, uint32_t *value)
{
    Atom atom = get_atom(display, prop_name);
    if (atom == None)
        return false;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop_data = NULL;

    int status = XRRGetOutputProperty(display, output, atom,
                                      0, 4, False, False,
                                      XA_INTEGER, &actual_type, &actual_format,
                                      &nitems, &bytes_after, &prop_data);

    if (status != Success || actual_type != XA_INTEGER ||
        actual_format != 32 || nitems != 1 || !prop_data) {
        if (prop_data)
            XFree(prop_data);
        return false;
    }

    *value = *((uint32_t *)prop_data);
    XFree(prop_data);
    return true;
}

int x11_context_refresh_outputs(x11_context_t *ctx)
{
    if (!ctx || !ctx->display)
        return -1;

    // Free existing outputs
    x11_context_free_outputs(ctx);

    // Get screen resources
    if (ctx->screen_resources)
        XRRFreeScreenResources(ctx->screen_resources);

    ctx->screen_resources = XRRGetScreenResources(ctx->display, ctx->root);
    if (!ctx->screen_resources)
        return -1;

    // Count connected outputs
    int connected_count = 0;
    for (int i = 0; i < ctx->screen_resources->noutput; i++) {
        XRROutputInfo *output_info = XRRGetOutputInfo(ctx->display,
                                                       ctx->screen_resources,
                                                       ctx->screen_resources->outputs[i]);
        if (output_info && output_info->connection == RR_Connected) {
            connected_count++;
        }
        if (output_info)
            XRRFreeOutputInfo(output_info);
    }

    if (connected_count == 0) {
        ctx->num_outputs = 0;
        ctx->outputs = NULL;
        return 0;
    }

    // Allocate output array
    ctx->outputs = calloc(connected_count, sizeof(output_info_t));
    if (!ctx->outputs)
        return -1;

    // Populate outputs
    int idx = 0;
    for (int i = 0; i < ctx->screen_resources->noutput; i++) {
        XRROutputInfo *output_info = XRRGetOutputInfo(ctx->display,
                                                       ctx->screen_resources,
                                                       ctx->screen_resources->outputs[i]);
        if (!output_info || output_info->connection != RR_Connected) {
            if (output_info)
                XRRFreeOutputInfo(output_info);
            continue;
        }

        output_info_t *out = &ctx->outputs[idx];
        out->output_id = ctx->screen_resources->outputs[i];
        out->name = strdup(output_info->name);
        out->connected = (output_info->connection == RR_Connected);

        // Store previous state for change detection
        output_info_t *prev_out = x11_context_find_output(ctx, out->output_id);
        if (prev_out) {
            out->prev_width = prev_out->width;
            out->prev_height = prev_out->height;
            out->prev_refresh_rate = prev_out->refresh_rate;
            out->prev_connected = prev_out->connected;
        } else {
            out->prev_width = 0;
            out->prev_height = 0;
            out->prev_refresh_rate = 0;
            out->prev_connected = false;
        }

        // Check if it's a virtual output (not a physical connector)
        // Virtual outputs are created via CREATE_XR_OUTPUT, so they won't have
        // standard physical connector names. We'll identify them by checking
        // if they're not in the standard physical output list.
        // For now, we'll check if it's NOT the XR-Manager output
        out->is_virtual = (strcmp(output_info->name, "XR-Manager") != 0);

        // Get FRAMEBUFFER_ID property
        if (!get_output_property(ctx->display, out->output_id,
                                 "FRAMEBUFFER_ID", &out->framebuffer_id)) {
            out->framebuffer_id = 0;
        }

        // Get actual resolution and refresh rate from current mode
        out->width = 0;
        out->height = 0;
        out->refresh_rate = 0;

        if (output_info->crtc != None) {
            XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(ctx->display,
                                                    ctx->screen_resources,
                                                    output_info->crtc);
            if (crtc_info) {
                out->width = crtc_info->width;
                out->height = crtc_info->height;

                // Get refresh rate from current mode
                if (crtc_info->mode != None) {
                    for (int j = 0; j < output_info->nmode; j++) {
                        if (output_info->modes[j] == crtc_info->mode) {
                            // Mode ID found, get mode info
                            XRRModeInfo *mode_info = NULL;
                            for (int k = 0; k < ctx->screen_resources->nmode; k++) {
                                if (ctx->screen_resources->modes[k].id == crtc_info->mode) {
                                    mode_info = &ctx->screen_resources->modes[k];
                                    break;
                                }
                            }
                            if (mode_info && mode_info->hTotal > 0 && mode_info->vTotal > 0) {
                                // Calculate refresh rate: (dot clock * 1000) / (hTotal * vTotal)
                                double refresh = ((double)mode_info->dotClock * 1000.0) /
                                                ((double)mode_info->hTotal * (double)mode_info->vTotal);
                                out->refresh_rate = (int)(refresh + 0.5);  // Round to nearest Hz
                            }
                            break;
                        }
                    }
                }

                XRRFreeCrtcInfo(crtc_info);
            }
        }

        idx++;
        XRRFreeOutputInfo(output_info);
    }

    ctx->num_outputs = idx;
    return 0;
}

output_info_t *x11_context_find_output(x11_context_t *ctx, RROutput output_id)
{
    if (!ctx || !ctx->outputs)
        return NULL;

    for (int i = 0; i < ctx->num_outputs; i++) {
        if (ctx->outputs[i].output_id == output_id)
            return &ctx->outputs[i];
    }

    return NULL;
}

void x11_context_free_outputs(x11_context_t *ctx)
{
    if (!ctx || !ctx->outputs)
        return;

    for (int i = 0; i < ctx->num_outputs; i++) {
        if (ctx->outputs[i].name)
            free(ctx->outputs[i].name);
    }

    free(ctx->outputs);
    ctx->outputs = NULL;
    ctx->num_outputs = 0;
}

RROutput x11_context_create_virtual_output(x11_context_t *ctx, const char *name,
                                            int width, int height, int refresh)
{
    if (!ctx || !ctx->display || !name)
        return None;

    // Find the XR-Manager output
    if (!ctx->screen_resources) {
        ctx->screen_resources = XRRGetScreenResources(ctx->display, ctx->root);
        if (!ctx->screen_resources)
            return None;
    }

    RROutput manager_output = None;
    for (int i = 0; i < ctx->screen_resources->noutput; i++) {
        XRROutputInfo *output_info = XRRGetOutputInfo(ctx->display,
                                                       ctx->screen_resources,
                                                       ctx->screen_resources->outputs[i]);
        if (output_info && strcmp(output_info->name, "XR-Manager") == 0) {
            manager_output = ctx->screen_resources->outputs[i];
            XRRFreeOutputInfo(output_info);
            break;
        }
        if (output_info)
            XRRFreeOutputInfo(output_info);
    }

    if (manager_output == None) {
        printf("XR-Manager output not found\n");
        return None;
    }

    // Create virtual output by setting CREATE_XR_OUTPUT property
    // Format: "NAME:WIDTH:HEIGHT[:REFRESH]"
    char create_cmd[256];
    if (refresh > 0) {
        snprintf(create_cmd, sizeof(create_cmd), "%s:%d:%d:%d", name, width, height, refresh);
    } else {
        snprintf(create_cmd, sizeof(create_cmd), "%s:%d:%d", name, width, height);
    }

    Atom create_atom = get_atom(ctx->display, "CREATE_XR_OUTPUT");
    if (create_atom == None) {
        printf("CREATE_XR_OUTPUT atom not found\n");
        return None;
    }

    // Set the property to trigger virtual output creation
    XRRChangeOutputProperty(ctx->display, manager_output, create_atom,
                           XA_STRING, 8, PropModeReplace,
                           (unsigned char *)create_cmd, strlen(create_cmd));

    XSync(ctx->display, False);

    // Wait a bit for the output to be created, then refresh outputs
    usleep(100000);  // 100ms
    x11_context_refresh_outputs(ctx);

    // Find the newly created output
    for (int i = 0; i < ctx->num_outputs; i++) {
        if (ctx->outputs[i].is_virtual &&
            strncmp(ctx->outputs[i].name, name, strlen(name)) == 0) {
            printf("Created virtual output: %s (%dx%d@%dHz)\n",
                   ctx->outputs[i].name, width, height, refresh);
            return ctx->outputs[i].output_id;
        }
    }

    printf("Virtual output created but not found after refresh\n");
    return None;
}

void x11_context_delete_virtual_output(x11_context_t *ctx, RROutput output_id)
{
    if (!ctx || !ctx->display || output_id == None)
        return;

    // Find the XR-Manager output
    XRRScreenResources *res = XRRGetScreenResources(ctx->display, ctx->root);
    if (!res)
        return;

    RROutput manager_output = None;
    for (int i = 0; i < res->noutput; i++) {
        XRROutputInfo *output_info = XRRGetOutputInfo(ctx->display, res, res->outputs[i]);
        if (output_info && output_info->connection == RR_Connected) {
            if (strstr(output_info->name, "XR-Manager") != NULL) {
                manager_output = res->outputs[i];
                XRRFreeOutputInfo(output_info);
                break;
            }
        }
        if (output_info)
            XRRFreeOutputInfo(output_info);
    }

    if (manager_output == None) {
        XRRFreeScreenResources(res);
        return;
    }

    // Get the DELETE_XR_OUTPUT atom
    Atom delete_atom = get_atom(ctx->display, "DELETE_XR_OUTPUT");
    if (delete_atom == None) {
        XRRFreeScreenResources(res);
        return;
    }

    // Format: "DELETE_XR_OUTPUT <output_id>"
    char delete_cmd[64];
    snprintf(delete_cmd, sizeof(delete_cmd), "%lu", (unsigned long)output_id);

    // Set the property to trigger virtual output deletion
    XRRChangeOutputProperty(ctx->display, manager_output, delete_atom,
                           XA_STRING, 8, PropModeReplace,
                           (unsigned char *)delete_cmd, strlen(delete_cmd));

    XSync(ctx->display, False);

    // Wait a bit for the output to be deleted, then refresh outputs
    usleep(100000);  // 100ms
    x11_context_refresh_outputs(ctx);

    XRRFreeScreenResources(res);
}

int x11_context_get_fd(x11_context_t *ctx)
{
    if (!ctx || !ctx->display)
        return -1;
    return ConnectionNumber(ctx->display);
}

int x11_context_process_events(x11_context_t *ctx)
{
    if (!ctx || !ctx->display)
        return -1;

    // Check if events are available
    if (XPending(ctx->display) == 0)
        return 0;

    XEvent event;
    bool output_changed = false;

    while (XCheckTypedEvent(ctx->display, ctx->rr_event_base + RRScreenChangeNotify, &event) ||
           XCheckTypedEvent(ctx->display, ctx->rr_event_base + RRNotify, &event)) {

        if (event.type == ctx->rr_event_base + RRScreenChangeNotify) {
            // Screen configuration changed
            output_changed = true;
        } else if (event.type == ctx->rr_event_base + RRNotify) {
            XRRNotifyEvent *rr_event = (XRRNotifyEvent *)&event;

            if (rr_event->subtype == RRNotify_OutputChange ||
                rr_event->subtype == RRNotify_CrtcChange) {
                output_changed = true;
            }
        }
    }

    // Refresh outputs if changes detected
    if (output_changed) {
        x11_context_refresh_outputs(ctx);
        return 1;  // Indicate changes detected
    }

    return 0;
}
