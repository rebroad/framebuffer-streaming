#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

typedef struct audio_capture audio_capture_t;

// Audio format constants
#define AUDIO_FORMAT_PCM_S16LE 0
#define AUDIO_FORMAT_PCM_S32LE 1

// Create audio capture instance
// sample_rate: e.g., 48000
// channels: e.g., 2 (stereo)
// format: AUDIO_FORMAT_PCM_S16LE or AUDIO_FORMAT_PCM_S32LE
audio_capture_t *audio_capture_create(uint32_t sample_rate, uint16_t channels, uint16_t format);

// Start capturing audio
int audio_capture_start(audio_capture_t *capture);

// Stop capturing audio
void audio_capture_stop(audio_capture_t *capture);

// Get captured audio data
// Returns number of bytes captured, or -1 on error
// Caller must free the returned buffer
int audio_capture_read(audio_capture_t *capture, void **data, uint32_t *size);

// Destroy audio capture instance
void audio_capture_destroy(audio_capture_t *capture);

// Get current timestamp in microseconds (monotonic clock)
uint64_t audio_get_timestamp_us(void);

#endif // AUDIO_CAPTURE_H

