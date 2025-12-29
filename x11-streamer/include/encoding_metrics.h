#ifndef ENCODING_METRICS_H
#define ENCODING_METRICS_H

#include <stdint.h>
#include <stdbool.h>

// Encoding metrics for adaptive switching
typedef struct {
    uint64_t frame_count;
    uint64_t total_bytes_sent;
    uint64_t last_frame_time_us;
    uint64_t last_metrics_reset_us;

    // Current metrics (averaged over window)
    double actual_fps;
    double bandwidth_mbps;
    double dirty_region_percent;
    uint64_t encoding_time_us;

    // State tracking for switching
    int consecutive_high_change_frames;  // Frames with >50% dirty region
    int consecutive_low_change_frames;  // Frames with <20% dirty region
    int consecutive_low_fps_frames;     // Frames with <80% target FPS
    int consecutive_good_fps_frames;    // Frames with >=95% target FPS

    // Window size for averaging (in frames)
    int window_size;
    int window_index;
    double *fps_history;
    double *bandwidth_history;
    double *dirty_percent_history;
} encoding_metrics_t;

// Create metrics tracker
encoding_metrics_t *encoding_metrics_create(int window_size);

// Destroy metrics tracker
void encoding_metrics_destroy(encoding_metrics_t *metrics);

// Record a frame
// bytes_sent: bytes sent for this frame
// dirty_pixels: number of dirty pixels (0 if full frame)
// total_pixels: total pixels in frame
// encoding_time_us: time taken to encode/process frame
void encoding_metrics_record_frame(encoding_metrics_t *metrics,
                                   uint64_t bytes_sent,
                                   uint64_t dirty_pixels,
                                   uint64_t total_pixels,
                                   uint64_t encoding_time_us,
                                   int target_fps);

// Get current metrics
double encoding_metrics_get_fps(encoding_metrics_t *metrics);
double encoding_metrics_get_bandwidth_mbps(encoding_metrics_t *metrics);
double encoding_metrics_get_dirty_percent(encoding_metrics_t *metrics);
uint64_t encoding_metrics_get_encoding_time_us(encoding_metrics_t *metrics);

// Check if we should switch to H.264
// Returns true if conditions met for switching to H.264
bool encoding_metrics_should_switch_to_h264(encoding_metrics_t *metrics, int target_fps);

// Check if we should switch back to dirty rectangles
// Returns true if conditions met for switching back
bool encoding_metrics_should_switch_to_dirty_rects(encoding_metrics_t *metrics, int target_fps);

// Reset metrics (call when switching modes)
void encoding_metrics_reset(encoding_metrics_t *metrics);

#endif // ENCODING_METRICS_H

