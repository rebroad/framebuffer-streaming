#include "dirty_rect.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct dirty_rect_context {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;  // Bytes per pixel
    uint32_t pitch;
    void *previous_frame;
    size_t frame_size;
};

dirty_rect_context_t *dirty_rect_create(uint32_t width, uint32_t height, uint32_t bpp)
{
    dirty_rect_context_t *ctx = calloc(1, sizeof(dirty_rect_context_t));
    if (!ctx)
        return NULL;

    ctx->width = width;
    ctx->height = height;
    ctx->bpp = bpp;
    ctx->pitch = width * bpp;  // Assume no padding for now
    ctx->frame_size = ctx->pitch * height;

    ctx->previous_frame = calloc(1, ctx->frame_size);
    if (!ctx->previous_frame) {
        free(ctx);
        return NULL;
    }

    return ctx;
}

void dirty_rect_destroy(dirty_rect_context_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->previous_frame)
        free(ctx->previous_frame);
    free(ctx);
}

void dirty_rect_reset(dirty_rect_context_t *ctx)
{
    if (!ctx || !ctx->previous_frame)
        return;

    memset(ctx->previous_frame, 0, ctx->frame_size);
}

static inline bool pixels_differ(const uint8_t *a, const uint8_t *b, uint32_t bpp)
{
    // Compare pixel by pixel
    for (uint32_t i = 0; i < bpp; i++) {
        if (a[i] != b[i])
            return true;
    }
    return false;
}

// Simple algorithm: scan for changed pixels and create rectangles
// This is a basic implementation - could be optimized with better algorithms
int dirty_rect_detect(dirty_rect_context_t *ctx,
                      const void *current_frame,
                      dirty_rect_t *rectangles,
                      int max_rects)
{
    if (!ctx || !current_frame || !rectangles || max_rects <= 0)
        return 0;

    if (!ctx->previous_frame) {
        // First frame - mark entire screen as dirty
        dirty_rect_t *rect = &rectangles[0];
        rect->x = 0;
        rect->y = 0;
        rect->width = ctx->width;
        rect->height = ctx->height;

        // Save current frame as previous
        memcpy(ctx->previous_frame, current_frame, ctx->frame_size);
        return 1;
    }

    const uint8_t *current = (const uint8_t *)current_frame;
    const uint8_t *previous = (const uint8_t *)ctx->previous_frame;
    int rect_count = 0;

    // Simple approach: divide screen into tiles and check each tile
    // For better performance, we use 32x32 pixel tiles
    const uint32_t tile_size = 32;
    uint32_t tiles_x = (ctx->width + tile_size - 1) / tile_size;
    uint32_t tiles_y = (ctx->height + tile_size - 1) / tile_size;

    // Track which tiles are dirty
    bool *dirty_tiles = calloc(tiles_x * tiles_y, sizeof(bool));
    if (!dirty_tiles)
        return 0;

    // Check each tile
    for (uint32_t ty = 0; ty < tiles_y && rect_count < max_rects; ty++) {
        for (uint32_t tx = 0; tx < tiles_x && rect_count < max_rects; tx++) {
            uint32_t x_start = tx * tile_size;
            uint32_t y_start = ty * tile_size;
            uint32_t x_end = (x_start + tile_size < ctx->width) ? x_start + tile_size : ctx->width;
            uint32_t y_end = (y_start + tile_size < ctx->height) ? y_start + tile_size : ctx->height;

            bool tile_dirty = false;
            for (uint32_t y = y_start; y < y_end && !tile_dirty; y++) {
                for (uint32_t x = x_start; x < x_end && !tile_dirty; x++) {
                    const uint8_t *curr_pixel = current + (y * ctx->pitch + x * ctx->bpp);
                    const uint8_t *prev_pixel = previous + (y * ctx->pitch + x * ctx->bpp);

                    if (pixels_differ(curr_pixel, prev_pixel, ctx->bpp)) {
                        tile_dirty = true;
                    }
                }
            }

            if (tile_dirty) {
                dirty_tiles[ty * tiles_x + tx] = true;
            }
        }
    }

    // Merge adjacent dirty tiles into rectangles
    // Simple greedy algorithm: find contiguous regions
    bool *processed = calloc(tiles_x * tiles_y, sizeof(bool));
    if (!processed) {
        free(dirty_tiles);
        return 0;
    }

    for (uint32_t ty = 0; ty < tiles_y && rect_count < max_rects; ty++) {
        for (uint32_t tx = 0; tx < tiles_x && rect_count < max_rects; tx++) {
            if (!dirty_tiles[ty * tiles_x + tx] || processed[ty * tiles_x + tx])
                continue;

            // Start a new rectangle
            uint32_t rect_x = tx * tile_size;
            uint32_t rect_y = ty * tile_size;
            uint32_t rect_w = tile_size;
            uint32_t rect_h = tile_size;

            // Try to expand right
            uint32_t expand_x = tx + 1;
            while (expand_x < tiles_x &&
                   dirty_tiles[ty * tiles_x + expand_x] &&
                   !processed[ty * tiles_x + expand_x]) {
                rect_w += tile_size;
                processed[ty * tiles_x + expand_x] = true;
                expand_x++;
            }

            // Try to expand down
            uint32_t expand_y = ty + 1;
            bool can_expand_down = true;
            while (expand_y < tiles_y && can_expand_down && rect_count < max_rects) {
                // Check if entire row is dirty
                for (uint32_t check_x = tx; check_x < expand_x; check_x++) {
                    if (!dirty_tiles[expand_y * tiles_x + check_x] ||
                        processed[expand_y * tiles_x + check_x]) {
                        can_expand_down = false;
                        break;
                    }
                }

                if (can_expand_down) {
                    rect_h += tile_size;
                    for (uint32_t mark_x = tx; mark_x < expand_x; mark_x++) {
                        processed[expand_y * tiles_x + mark_x] = true;
                    }
                    expand_y++;
                }
            }

            // Clamp to screen bounds
            if (rect_x + rect_w > ctx->width)
                rect_w = ctx->width - rect_x;
            if (rect_y + rect_h > ctx->height)
                rect_h = ctx->height - rect_y;

            rectangles[rect_count].x = rect_x;
            rectangles[rect_count].y = rect_y;
            rectangles[rect_count].width = rect_w;
            rectangles[rect_count].height = rect_h;
            rect_count++;

            processed[ty * tiles_x + tx] = true;
        }
    }

    free(dirty_tiles);
    free(processed);

    // Save current frame as previous for next comparison
    memcpy(ctx->previous_frame, current_frame, ctx->frame_size);

    return rect_count;
}

uint64_t dirty_rect_get_dirty_pixel_count(dirty_rect_context_t *ctx)
{
    if (!ctx)
        return 0;

    // This is a placeholder - would need to track during detection
    // For now, return 0 and calculate from rectangles
    return 0;
}

uint32_t dirty_rect_get_width(dirty_rect_context_t *ctx)
{
    return ctx ? ctx->width : 0;
}

uint32_t dirty_rect_get_height(dirty_rect_context_t *ctx)
{
    return ctx ? ctx->height : 0;
}

