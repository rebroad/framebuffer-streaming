#include "drm_fb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <dirent.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define DRM_DEVICE_PATH "/dev/dri"

static int drm_open_device(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    return fd;
}

static bool drm_device_has_fb(int fd, uint32_t fb_id)
{
    drmModeFBPtr fb = drmModeGetFB(fd, fb_id);
    if (fb) {
        drmModeFreeFB(fb);
        return true;
    }
    return false;
}

drm_device_t *drm_find_device_by_fb_id(uint32_t fb_id)
{
    DIR *dir = opendir(DRM_DEVICE_PATH);
    if (!dir)
        return NULL;

    struct dirent *entry;
    drm_device_t *dev = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "card", 4) != 0 &&
            strncmp(entry->d_name, "renderD", 7) != 0)
            continue;

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", DRM_DEVICE_PATH, entry->d_name);

        int fd = drm_open_device(path);
        if (fd < 0)
            continue;

        if (drm_device_has_fb(fd, fb_id)) {
            dev = calloc(1, sizeof(drm_device_t));
            if (dev) {
                dev->fd = fd;
                dev->path = strdup(path);
            } else {
                close(fd);
            }
            break;
        }

        close(fd);
    }

    closedir(dir);
    return dev;
}

void drm_device_destroy(drm_device_t *dev)
{
    if (!dev)
        return;

    if (dev->fd >= 0)
        close(dev->fd);

    if (dev->path)
        free(dev->path);

    free(dev);
}

drm_fb_t *drm_fb_open(uint32_t fb_id)
{
    drm_fb_t *fb = calloc(1, sizeof(drm_fb_t));
    if (!fb)
        return NULL;

    fb->fb_id = fb_id;

    // Find the DRM device that has this framebuffer
    drm_device_t *dev = drm_find_device_by_fb_id(fb_id);
    if (!dev) {
        free(fb);
        return NULL;
    }

    fb->fd = dev->fd;
    free(dev->path);
    free(dev);

    // Get framebuffer info
    drmModeFBPtr fb_info = drmModeGetFB(fb->fd, fb_id);
    if (!fb_info) {
        close(fb->fd);
        free(fb);
        return NULL;
    }

    fb->width = fb_info->width;
    fb->height = fb_info->height;
    fb->pitch = fb_info->pitch;
    fb->bpp = fb_info->bpp;
    fb->size = fb_info->height * fb_info->pitch;
    // Default to ARGB8888 format (most common)
    fb->format = 0x34325241;  // DRM_FORMAT_ARGB8888

    drmModeFreeFB(fb_info);

    return fb;
}

void drm_fb_close(drm_fb_t *fb)
{
    if (!fb)
        return;

    drm_fb_unmap(fb);

    if (fb->dma_fd >= 0)
        close(fb->dma_fd);

    if (fb->fd >= 0)
        close(fb->fd);

    free(fb);
}

int drm_fb_export_dma_buf(drm_fb_t *fb)
{
    if (!fb || fb->fd < 0)
        return -1;

    // Get the handle for the framebuffer
    drmModeFBPtr fb_info = drmModeGetFB(fb->fd, fb->fb_id);
    if (!fb_info)
        return -1;

    uint32_t handle = fb_info->handle;
    drmModeFreeFB(fb_info);

    // Export as DMA-BUF
    struct drm_prime_handle prime_handle = {
        .handle = handle,
        .flags = DRM_CLOEXEC | DRM_RDWR,
        .fd = -1
    };

    int ret = drmIoctl(fb->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle);
    if (ret < 0)
        return -1;

    fb->dma_fd = prime_handle.fd;
    return 0;
}

int drm_fb_map(drm_fb_t *fb)
{
    if (!fb || fb->fd < 0 || fb->map)
        return -1;

    // If we have a DMA-BUF FD, map it for CPU access
    // This is needed when sending over network (can't pass FDs over TCP)
    if (fb->dma_fd >= 0) {
        size_t size = fb->height * fb->pitch;
        void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fb->dma_fd, 0);
        if (map == MAP_FAILED) {
            return -1;
        }
        fb->map = map;
        fb->size = size;
        return 0;
    }

    // For framebuffers without DMA-BUF support (e.g., dumb buffers from Xorg/X11Libre),
    // fall back to mapping via DRM_IOCTL_MODE_MAP_DUMB
    // 1. Get the buffer handle from drmModeGetFB()->handle
    drmModeFBPtr fb_info = drmModeGetFB(fb->fd, fb->fb_id);
    if (!fb_info)
        return -1;

    uint32_t handle = fb_info->handle;
    drmModeFreeFB(fb_info);

    // 2. Use DRM_IOCTL_MODE_MAP_DUMB to get a mapping offset
    struct drm_mode_map_dumb map_arg = {
        .handle = handle
    };

    if (drmIoctl(fb->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg) < 0) {
        // Not a dumb buffer or mapping failed
        return -1;
    }

    // 3. mmap() the DRM device FD with that offset
    void *map = mmap(NULL, fb->size, PROT_READ, MAP_SHARED, fb->fd, map_arg.offset);
    if (map == MAP_FAILED) {
        return -1;
    }

    fb->map = map;
    return 0;
}

void drm_fb_unmap(drm_fb_t *fb)
{
    if (!fb || !fb->map)
        return;

    munmap(fb->map, fb->size);
    fb->map = NULL;
}

