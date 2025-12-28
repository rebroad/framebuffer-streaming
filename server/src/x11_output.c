#include "x11_output.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <X11/Xatom.h>

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
        out->connected = true;
        out->width = output_info->mm_width;
        out->height = output_info->mm_height;

        // Check if it's a virtual output (starts with "XR-")
        out->is_virtual = (strncmp(output_info->name, "XR-", 3) == 0);

        // Get FRAMEBUFFER_ID property
        if (!get_output_property(ctx->display, out->output_id,
                                 "FRAMEBUFFER_ID", &out->framebuffer_id)) {
            out->framebuffer_id = 0;
        }

        // Get PIXMAP_ID property (for virtual outputs)
        if (out->is_virtual) {
            if (!get_output_property(ctx->display, out->output_id,
                                     "PIXMAP_ID", &out->pixmap_id)) {
                out->pixmap_id = 0;
            }
        } else {
            out->pixmap_id = 0;
        }

        // Get actual resolution from current mode
        if (output_info->crtc != None) {
            XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(ctx->display,
                                                    ctx->screen_resources,
                                                    output_info->crtc);
            if (crtc_info) {
                out->width = crtc_info->width;
                out->height = crtc_info->height;
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

