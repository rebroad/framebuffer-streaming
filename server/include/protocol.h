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
    MSG_INPUT = 0x04,
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

// HELLO message
typedef struct __attribute__((packed)) {
    uint16_t protocol_version;
    uint16_t client_type;  // 0=Android, 1=RaspberryPi
    char capabilities[32];
} hello_message_t;

// FRAME message
typedef struct __attribute__((packed)) {
    uint32_t output_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;  // DRM format (e.g., DRM_FORMAT_ARGB8888)
    uint32_t pitch;
    uint32_t size;    // Size of frame data
    // Followed by frame data
} frame_message_t;

// CONFIG message
typedef struct __attribute__((packed)) {
    uint32_t output_id;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;
} config_message_t;

// INPUT message (from client)
typedef struct __attribute__((packed)) {
    uint8_t input_type;  // 0=touch, 1=key, 2=mouse
    uint32_t data_size;
    // Followed by input data
} input_message_t;

int protocol_send_message(int fd, message_type_t type, const void *data, size_t data_len);
int protocol_receive_message(int fd, message_header_t *header, void **payload);

#endif // PROTOCOL_H

