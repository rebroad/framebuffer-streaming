#ifndef DIRTY_RECT_H
#define DIRTY_RECT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Dirty rectangle
typedef struct {
	uint32_t x, y;
	uint32_t width, height;
} dirty_rect_t;

// Dirty rectangle detection context
typedef struct dirty_rect_context dirty_rect_context_t;

// Create dirty rectangle detection context
dirty_rect_context_t *dirty_rect_create(uint32_t width, uint32_t height, uint32_t bpp);

// Destroy dirty rectangle detection context
void dirty_rect_destroy(dirty_rect_context_t *ctx);

// Detect dirty rectangles by comparing current frame with previous
// Returns number of dirty rectangles found
// Rectangles are stored in the provided array (max_rects)
int dirty_rect_detect(dirty_rect_context_t *ctx,
					  const void *current_frame,
					  dirty_rect_t *rectangles,
					  int max_rects);

// Get total dirty pixel count (for metrics)
uint64_t dirty_rect_get_dirty_pixel_count(dirty_rect_context_t *ctx);

// Get context dimensions
uint32_t dirty_rect_get_width(dirty_rect_context_t *ctx);
uint32_t dirty_rect_get_height(dirty_rect_context_t *ctx);

// Reset/clear previous frame (call when resolution changes)
void dirty_rect_reset(dirty_rect_context_t *ctx);

#endif // DIRTY_RECT_H

