#include "audio_capture.h"
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

struct audio_capture {
    pa_simple *pa;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t format;
    bool running;
    pthread_mutex_t mutex;
};

static pa_sample_format_t format_to_pa_format(uint16_t format)
{
    switch (format) {
        case AUDIO_FORMAT_PCM_S16LE:
            return PA_SAMPLE_S16LE;
        case AUDIO_FORMAT_PCM_S32LE:
            return PA_SAMPLE_S32LE;
        default:
            return PA_SAMPLE_S16LE;
    }
}

audio_capture_t *audio_capture_create(uint32_t sample_rate, uint16_t channels, uint16_t format)
{
    audio_capture_t *capture = calloc(1, sizeof(audio_capture_t));
    if (!capture)
        return NULL;

    capture->sample_rate = sample_rate;
    capture->channels = channels;
    capture->format = format;
    capture->running = false;
    pthread_mutex_init(&capture->mutex, NULL);

    return capture;
}

int audio_capture_start(audio_capture_t *capture)
{
    if (!capture)
        return -1;

    pthread_mutex_lock(&capture->mutex);

    if (capture->running) {
        pthread_mutex_unlock(&capture->mutex);
        return 0;  // Already running
    }

    pa_sample_spec ss = {
        .format = format_to_pa_format(capture->format),
        .rate = capture->sample_rate,
        .channels = capture->channels
    };

    pa_buffer_attr ba = {
        .maxlength = (uint32_t)-1,
        .tlength = (uint32_t)-1,
        .prebuf = (uint32_t)-1,
        .minreq = (uint32_t)-1,
        .fragsize = capture->sample_rate * capture->channels * sizeof(int16_t) / 10  // 100ms fragments for low latency
    };

    int error;
    capture->pa = pa_simple_new(NULL,                    // server
                                 "x11-streamer",         // application name
                                 PA_STREAM_RECORD,       // direction
                                 NULL,                   // device (NULL = default)
                                 "Audio capture",        // stream name
                                 &ss,                    // sample spec
                                 NULL,                   // channel map (NULL = default)
                                 &ba,                    // buffer attributes
                                 &error);

    if (!capture->pa) {
        fprintf(stderr, "Failed to create PulseAudio connection: %s\n", pa_strerror(error));
        pthread_mutex_unlock(&capture->mutex);
        return -1;
    }

    capture->running = true;
    pthread_mutex_unlock(&capture->mutex);

    return 0;
}

void audio_capture_stop(audio_capture_t *capture)
{
    if (!capture)
        return;

    pthread_mutex_lock(&capture->mutex);
    if (capture->pa) {
        pa_simple_free(capture->pa);
        capture->pa = NULL;
    }
    capture->running = false;
    pthread_mutex_unlock(&capture->mutex);
}

int audio_capture_read(audio_capture_t *capture, void **data, uint32_t *size)
{
    if (!capture || !data || !size)
        return -1;

    pthread_mutex_lock(&capture->mutex);

    if (!capture->running || !capture->pa) {
        pthread_mutex_unlock(&capture->mutex);
        return -1;
    }

    // Read a small chunk for low latency (about 10ms of audio)
    size_t bytes_per_sample = (capture->format == AUDIO_FORMAT_PCM_S32LE) ? 4 : 2;
    size_t chunk_size = capture->sample_rate * capture->channels * bytes_per_sample / 100;  // 10ms

    void *buffer = malloc(chunk_size);
    if (!buffer) {
        pthread_mutex_unlock(&capture->mutex);
        return -1;
    }

    int error;
    int ret = pa_simple_read(capture->pa, buffer, chunk_size, &error);
    pthread_mutex_unlock(&capture->mutex);

    if (ret < 0) {
        if (error == PA_ERR_NOENTITY) {
            // No data available yet, return 0
            free(buffer);
            return 0;
        }
        fprintf(stderr, "PulseAudio read error: %s\n", pa_strerror(error));
        free(buffer);
        return -1;
    }

    *data = buffer;
    *size = chunk_size;
    return chunk_size;
}

void audio_capture_destroy(audio_capture_t *capture)
{
    if (!capture)
        return;

    audio_capture_stop(capture);
    pthread_mutex_destroy(&capture->mutex);
    free(capture);
}

uint64_t audio_get_timestamp_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
    }
    return 0;
}

