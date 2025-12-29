# Adaptive Encoding System Design

## Overview
The system starts with dirty rectangles (low latency, good for desktop) and automatically switches to H.264 compression when needed, then switches back when conditions improve.

## Encoding Modes

### Mode 1: Dirty Rectangles (Default)
- **Use case**: Desktop usage, mostly static content
- **Method**: Compare frames, send only changed rectangular regions
- **Advantages**: Low latency, simple, works well for desktop
- **Disadvantages**: High bandwidth when large regions change

### Mode 2: H.264 Compression (Fallback)
- **Use case**: Video content, high change rate, bandwidth constraints
- **Method**: Encode full frame with H.264
- **Advantages**: High compression, good for video
- **Disadvantages**: Encoding latency, CPU/GPU intensive

## Metrics Tracked

1. **Frame Rate**: Actual frames sent per second vs target refresh rate
2. **Dirty Region Percentage**: Percentage of screen that changed
3. **Bandwidth Usage**: Bytes sent per second
4. **Encoding Time**: Time to encode/process frame (for H.264)
5. **Change Rate**: How much of screen is changing per frame

## Switching Logic

### Dirty Rectangles → H.264
Switch when **any** of these conditions are met for **N consecutive frames** (N=5):

1. **Dirty region > 50%** of screen
   - Too many changes, might as well send full frame
   - Threshold: `dirty_pixels > (width * height * 0.5)`

2. **Frame rate < 80%** of target refresh rate
   - Can't keep up with changes, falling behind
   - Threshold: `actual_fps < refresh_rate * 0.8`

3. **Bandwidth > threshold** (e.g., 100 MB/s)
   - Network saturation
   - Threshold: `bytes_per_second > 100 * 1024 * 1024`

4. **Dirty rectangles overhead > full frame**
   - If sending many small rectangles, full frame might be smaller
   - Threshold: `dirty_data_size > full_frame_size * 0.9`

### H.264 → Dirty Rectangles
Switch back when **all** of these conditions are met for **N consecutive frames** (N=10):

1. **Dirty region < 20%** of screen
   - Small changes, dirty rectangles more efficient
   - Threshold: `dirty_pixels < (width * height * 0.2)`

2. **Frame rate stable >= 95%** of target
   - System can keep up with changes
   - Threshold: `actual_fps >= refresh_rate * 0.95`

3. **Low bandwidth usage**
   - Network not saturated
   - Threshold: `bytes_per_second < 50 * 1024 * 1024`

4. **Encoding latency acceptable**
   - H.264 encoding not causing delays
   - Threshold: `encode_time_ms < 16` (for 60 FPS)

## Implementation Details

### Frame Message Extension
Add encoding mode flag to frame message:
```c
typedef struct __attribute__((packed)) {
    uint64_t timestamp_us;
    uint32_t output_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t pitch;
    uint32_t size;
    uint8_t encoding_mode;  // 0=dirty rectangles, 1=H.264, 2=full frame
    uint8_t num_regions;   // For dirty rectangles mode
    // Followed by:
    // - For dirty rectangles: array of rectangles + data
    // - For H.264: encoded video data
    // - For full frame: raw pixel data
} frame_message_t;
```

### Dirty Rectangle Format
```c
typedef struct __attribute__((packed)) {
    uint32_t x, y, width, height;
    uint32_t data_size;
    // Followed by pixel data for this rectangle
} dirty_rectangle_t;
```

### Metrics Structure
```c
typedef struct {
    uint64_t frame_count;
    uint64_t total_bytes_sent;
    uint64_t last_frame_time_us;
    double actual_fps;
    double bandwidth_mbps;
    double dirty_region_percent;
    uint64_t encoding_time_us;
    int consecutive_high_change_frames;
    int consecutive_low_change_frames;
} encoding_metrics_t;
```

## State Machine

```
[Dirty Rectangles] ←─────────────────┐
    │                                │
    │ (conditions met)               │ (conditions improve)
    ↓                                │
[H.264 Encoding] ────────────────────┘
```

## Hysteresis
Use different thresholds for switching up vs down to prevent oscillation:
- **Switch to H.264**: Aggressive (50% dirty region)
- **Switch back**: Conservative (20% dirty region, stable for longer)

## Performance Targets

- **Latency**: < 50ms end-to-end (dirty rectangles)
- **Frame Rate**: Match display refresh rate (60 FPS typical)
- **Bandwidth**: < 100 MB/s for desktop, < 200 MB/s for video
- **CPU Usage**: < 20% for encoding

