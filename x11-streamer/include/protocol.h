#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Message types
typedef enum {
    MSG_HELLO = 0x01,
    MSG_FRAME = 0x02,
    MSG_AUDIO = 0x03,
    MSG_CONFIG = 0x05,
    MSG_PING = 0x06,
    MSG_PONG = 0x07,
    MSG_ERROR = 0xFF
} message_type_t;

// Message header (all messages start with this)
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint32_t length;  // Length of payload (not including header)
    uint32_t sequence;
} message_header_t;

// Display mode capability
typedef struct __attribute__((packed)) {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;  // Hz * 100 (e.g., 6000 = 60.00 Hz)
} display_mode_t;

// HELLO message
typedef struct __attribute__((packed)) {
    uint16_t protocol_version;
    uint16_t num_modes;    // Number of supported display modes
    uint16_t display_name_len;  // Length of display name string
    // Followed by:
    // 1. Display name string (display_name_len bytes, null-terminated)
    // 2. display_mode_t array (num_modes entries)
} hello_message_t;

// Encoding modes
#define ENCODING_MODE_FULL_FRAME    0
#define ENCODING_MODE_DIRTY_RECTS   1
#define ENCODING_MODE_H264          2

// Dirty rectangle (for dirty rectangles mode)
typedef struct __attribute__((packed)) {
    uint32_t x, y;
    uint32_t width, height;
    uint32_t data_size;  // Size of pixel data for this rectangle
} dirty_rectangle_t;

// FRAME message
typedef struct __attribute__((packed)) {
    uint64_t timestamp_us;  // Microseconds since epoch (monotonic clock) for sync
    uint32_t output_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;  // DRM format (e.g., DRM_FORMAT_ARGB8888)
    uint32_t pitch;
    uint32_t size;    // Size of frame data
    uint8_t encoding_mode;  // 0=full frame, 1=dirty rectangles, 2=H.264
    uint8_t num_regions;    // Number of dirty rectangles (if encoding_mode=1)
    // Followed by:
    // - For full frame: raw pixel data
    // - For dirty rectangles: array of dirty_rectangle_t + pixel data for each
    // - For H.264: encoded video data
} frame_message_t;

// CONFIG message
typedef struct __attribute__((packed)) {
    uint32_t output_id;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;
} config_message_t;

// AUDIO message
typedef struct __attribute__((packed)) {
    uint64_t timestamp_us;  // Microseconds since epoch (monotonic clock)
    uint32_t sample_rate;   // e.g., 48000
    uint16_t channels;      // e.g., 2 (stereo)
    uint16_t format;        // 0=PCM_S16LE, 1=PCM_S32LE, etc.
    uint32_t data_size;     // Size of audio data in bytes
    // Followed by audio data (PCM samples)
} audio_message_t;

int protocol_send_message(int fd, message_type_t type, const void *data, size_t data_len);
int protocol_receive_message(int fd, message_header_t *header, void **payload);

#endif // PROTOCOL_H

