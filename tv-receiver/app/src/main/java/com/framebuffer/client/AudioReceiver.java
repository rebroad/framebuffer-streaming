package com.framebuffer.client;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;

public class AudioReceiver {
    private AudioTrack audioTrack;
    private int sampleRate;
    private int channels;
    private int format;
    private boolean running = false;
    private Thread playbackThread;
    private BlockingQueue<byte[]> audioQueue;
    private long baseTimestampUs = 0;
    private long basePlaybackTimeUs = 0;

    public AudioReceiver(int sampleRate, int channels, int format) {
        this.sampleRate = sampleRate;
        this.channels = channels;
        this.format = format;
        this.audioQueue = new LinkedBlockingQueue<>();
    }

    public void start() {
        if (running) return;

        this.running = true;

        // Create AudioTrack for low-latency playback
        int channelConfig = (channels == 2) ? AudioFormat.CHANNEL_OUT_STEREO : AudioFormat.CHANNEL_OUT_MONO;
        int audioFormat = (format == 0) ? AudioFormat.ENCODING_PCM_16BIT : AudioFormat.ENCODING_PCM_FLOAT;

        int bufferSize = AudioTrack.getMinBufferSize(sampleRate, channelConfig, audioFormat);
        // Use a small buffer for low latency (about 20ms)
        int preferredBufferSize = sampleRate * channels * 2 / 50; // 20ms
        if (preferredBufferSize < bufferSize) {
            preferredBufferSize = bufferSize;
        }

        AudioAttributes attributes = new AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
            .build();

        AudioFormat audioFormatObj = new AudioFormat.Builder()
            .setSampleRate(sampleRate)
            .setEncoding(audioFormat)
            .setChannelMask(channelConfig)
            .build();

        audioTrack = new AudioTrack.Builder()
            .setAudioAttributes(attributes)
            .setAudioFormat(audioFormatObj)
            .setBufferSizeInBytes(preferredBufferSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build();

        if (audioTrack.getState() == AudioTrack.STATE_INITIALIZED) {
            audioTrack.play();
            playbackThread = new Thread(this::playbackLoop);
            playbackThread.start();
        } else {
            running = false;
        }
    }

    public void stop() {
        running = false;
        if (playbackThread != null) {
            try {
                playbackThread.join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            playbackThread = null;
        }
        if (audioTrack != null) {
            if (audioTrack.getState() == AudioTrack.STATE_INITIALIZED) {
                audioTrack.stop();
            }
            audioTrack.release();
            audioTrack = null;
        }
    }

    public void addAudioData(byte[] audioData) {
        if (running && audioQueue != null) {
            audioQueue.offer(audioData);
        }
    }

    private void playbackLoop() {
        while (running) {
            try {
                // Get audio data from queue (blocking with timeout)
                byte[] audioData = audioQueue.poll();
                if (audioData != null && audioTrack != null && audioTrack.getState() == AudioTrack.STATE_INITIALIZED) {
                    int written = audioTrack.write(audioData, 0, audioData.length);
                    if (written < 0) {
                        // Error writing to AudioTrack
                        break;
                    }
                } else if (audioData == null && !running) {
                    // Stopped
                    break;
                }
                // Small sleep to avoid busy-waiting
                Thread.sleep(1);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
    }

    // Synchronize audio with video using timestamps
    public void syncWithTimestamp(long audioTimestampUs, long videoTimestampUs) {
        if (baseTimestampUs == 0) {
            baseTimestampUs = audioTimestampUs;
            basePlaybackTimeUs = System.nanoTime() / 1000; // Convert to microseconds
        }

        // Calculate when this audio should be played
        long delayUs = audioTimestampUs - videoTimestampUs;
        // For now, we'll just play immediately (simple sync)
        // In a more sophisticated implementation, we'd buffer and schedule playback
    }
}

