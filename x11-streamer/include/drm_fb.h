#ifndef DRM_FB_H
#define DRM_FB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct drm_fb {
    int fd;
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t format;
    void *map;
    size_t size;
} drm_fb_t;

typedef struct drm_device {
    int fd;
    char *path;
} drm_device_t;

drm_device_t *drm_find_device_by_fb_id(uint32_t fb_id);
void drm_device_destroy(drm_device_t *dev);

drm_fb_t *drm_fb_open(uint32_t fb_id);
void drm_fb_close(drm_fb_t *fb);
int drm_fb_map(drm_fb_t *fb);
void drm_fb_unmap(drm_fb_t *fb);

#endif // DRM_FB_H

