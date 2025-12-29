#include "encoding_metrics.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define SWITCH_TO_H264_THRESHOLD_FRAMES 5

static uint64_t encoding_metrics_get_timestamp_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
    }
    return 0;
}
#define SWITCH_TO_DIRTY_RECTS_THRESHOLD_FRAMES 10
#define DIRTY_REGION_HIGH_THRESHOLD 0.5  // 50%
#define DIRTY_REGION_LOW_THRESHOLD 0.2   // 20%
#define FPS_LOW_THRESHOLD 0.8  // 80% of target
#define FPS_GOOD_THRESHOLD 0.95  // 95% of target
#define BANDWIDTH_HIGH_THRESHOLD_MBPS 100.0
#define BANDWIDTH_LOW_THRESHOLD_MBPS 50.0

encoding_metrics_t *encoding_metrics_create(int window_size)
{
    encoding_metrics_t *metrics = calloc(1, sizeof(encoding_metrics_t));
    if (!metrics)
        return NULL;

    metrics->window_size = window_size > 0 ? window_size : 60;  // Default: 60 frames (1 second at 60 FPS)
    metrics->window_index = 0;

    metrics->fps_history = calloc(metrics->window_size, sizeof(double));
    metrics->bandwidth_history = calloc(metrics->window_size, sizeof(double));
    metrics->dirty_percent_history = calloc(metrics->window_size, sizeof(double));

    if (!metrics->fps_history || !metrics->bandwidth_history || !metrics->dirty_percent_history) {
        encoding_metrics_destroy(metrics);
        return NULL;
    }

    return metrics;
}

void encoding_metrics_destroy(encoding_metrics_t *metrics)
{
    if (!metrics)
        return;

    if (metrics->fps_history)
        free(metrics->fps_history);
    if (metrics->bandwidth_history)
        free(metrics->bandwidth_history);
    if (metrics->dirty_percent_history)
        free(metrics->dirty_percent_history);

    free(metrics);
}

void encoding_metrics_record_frame(encoding_metrics_t *metrics,
                                   uint64_t bytes_sent,
                                   uint64_t dirty_pixels,
                                   uint64_t total_pixels,
                                   uint64_t encoding_time_us,
                                   int target_fps)
{
    if (!metrics)
        return;

    uint64_t now_us = encoding_metrics_get_timestamp_us();

    // Calculate frame time delta
    double frame_time_sec = 0.0;
    if (metrics->last_frame_time_us > 0) {
        frame_time_sec = (now_us - metrics->last_frame_time_us) / 1000000.0;
    }

    // Calculate FPS for this frame
    double frame_fps = (frame_time_sec > 0) ? 1.0 / frame_time_sec : target_fps;

    // Calculate bandwidth (bytes per second)
    double frame_bandwidth_mbps = 0.0;
    if (frame_time_sec > 0) {
        frame_bandwidth_mbps = (bytes_sent / frame_time_sec) / (1024.0 * 1024.0);
    }

    // Calculate dirty region percentage
    double dirty_percent = 0.0;
    if (total_pixels > 0) {
        dirty_percent = (double)dirty_pixels / (double)total_pixels;
    }

    // Store in history
    metrics->fps_history[metrics->window_index] = frame_fps;
    metrics->bandwidth_history[metrics->window_index] = frame_bandwidth_mbps;
    metrics->dirty_percent_history[metrics->window_index] = dirty_percent;

    metrics->window_index = (metrics->window_index + 1) % metrics->window_size;

    // Calculate averages
    double fps_sum = 0.0, bandwidth_sum = 0.0, dirty_sum = 0.0;
    int count = 0;

    for (int i = 0; i < metrics->window_size; i++) {
        if (metrics->fps_history[i] > 0) {
            fps_sum += metrics->fps_history[i];
            bandwidth_sum += metrics->bandwidth_history[i];
            dirty_sum += metrics->dirty_percent_history[i];
            count++;
        }
    }

    if (count > 0) {
        metrics->actual_fps = fps_sum / count;
        metrics->bandwidth_mbps = bandwidth_sum / count;
        metrics->dirty_region_percent = dirty_sum / count;
    }

    metrics->encoding_time_us = encoding_time_us;
    metrics->total_bytes_sent += bytes_sent;
    metrics->frame_count++;
    metrics->last_frame_time_us = now_us;

    // Update consecutive frame counters
    if (metrics->last_metrics_reset_us == 0) {
        metrics->last_metrics_reset_us = now_us;
    }
    if (dirty_percent > DIRTY_REGION_HIGH_THRESHOLD) {
        metrics->consecutive_high_change_frames++;
        metrics->consecutive_low_change_frames = 0;
    } else if (dirty_percent < DIRTY_REGION_LOW_THRESHOLD) {
        metrics->consecutive_low_change_frames++;
        metrics->consecutive_high_change_frames = 0;
    } else {
        metrics->consecutive_high_change_frames = 0;
        metrics->consecutive_low_change_frames = 0;
    }

    if (target_fps > 0) {
        double fps_ratio = metrics->actual_fps / target_fps;
        if (fps_ratio < FPS_LOW_THRESHOLD) {
            metrics->consecutive_low_fps_frames++;
            metrics->consecutive_good_fps_frames = 0;
        } else if (fps_ratio >= FPS_GOOD_THRESHOLD) {
            metrics->consecutive_good_fps_frames++;
            metrics->consecutive_low_fps_frames = 0;
        } else {
            metrics->consecutive_low_fps_frames = 0;
            metrics->consecutive_good_fps_frames = 0;
        }
    }
}

double encoding_metrics_get_fps(encoding_metrics_t *metrics)
{
    return metrics ? metrics->actual_fps : 0.0;
}

double encoding_metrics_get_bandwidth_mbps(encoding_metrics_t *metrics)
{
    return metrics ? metrics->bandwidth_mbps : 0.0;
}

double encoding_metrics_get_dirty_percent(encoding_metrics_t *metrics)
{
    return metrics ? metrics->dirty_region_percent : 0.0;
}

uint64_t encoding_metrics_get_encoding_time_us(encoding_metrics_t *metrics)
{
    return metrics ? metrics->encoding_time_us : 0;
}

bool encoding_metrics_should_switch_to_h264(encoding_metrics_t *metrics, int target_fps)
{
    (void)target_fps;  // Used in consecutive_low_fps_frames check below

    if (!metrics)
        return false;

    // Switch if ANY condition is met for N consecutive frames
    if (metrics->consecutive_high_change_frames >= SWITCH_TO_H264_THRESHOLD_FRAMES) {
        return true;  // Dirty region > 50% for N frames
    }

    if (metrics->consecutive_low_fps_frames >= SWITCH_TO_H264_THRESHOLD_FRAMES) {
        return true;  // Frame rate < 80% of target for N frames
    }

    if (metrics->bandwidth_mbps > BANDWIDTH_HIGH_THRESHOLD_MBPS) {
        return true;  // Bandwidth too high
    }

    // If dirty rectangles overhead is high (many small rectangles)
    // This would need additional tracking - for now, use dirty percent as proxy
    if (metrics->dirty_region_percent > 0.9) {
        return true;  // Almost full screen dirty, might as well send full frame
    }

    return false;
}

bool encoding_metrics_should_switch_to_dirty_rects(encoding_metrics_t *metrics, int target_fps)
{
    if (!metrics)
        return false;

    // Switch back if ALL conditions are met for N consecutive frames
    if (metrics->consecutive_low_change_frames < SWITCH_TO_DIRTY_RECTS_THRESHOLD_FRAMES) {
        return false;  // Need stable low change rate
    }

    if (target_fps > 0) {
        double fps_ratio = metrics->actual_fps / target_fps;
        if (fps_ratio < FPS_GOOD_THRESHOLD) {
            return false;  // Frame rate not stable enough
        }
    }

    if (metrics->bandwidth_mbps > BANDWIDTH_LOW_THRESHOLD_MBPS) {
        return false;  // Bandwidth still high
    }

    if (metrics->encoding_time_us > 16000) {  // 16ms for 60 FPS
        return false;  // Encoding latency too high
    }

    return true;
}

void encoding_metrics_reset(encoding_metrics_t *metrics)
{
    if (!metrics)
        return;

    metrics->consecutive_high_change_frames = 0;
    metrics->consecutive_low_change_frames = 0;
    metrics->consecutive_low_fps_frames = 0;
    metrics->consecutive_good_fps_frames = 0;
    metrics->last_metrics_reset_us = encoding_metrics_get_timestamp_us();
}

