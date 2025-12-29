package com.framebuffer.client;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.view.Surface;
import java.io.IOException;
import java.nio.ByteBuffer;

public class H264Decoder {
    private MediaCodec decoder;
    private boolean initialized = false;
    private int width;
    private int height;
    private Surface surface;

    public int getWidth() { return width; }
    public int getHeight() { return height; }

    public H264Decoder(int width, int height, Surface surface) {
        this.width = width;
        this.height = height;
        this.surface = surface;
    }

    public boolean initialize() {
        try {
            decoder = MediaCodec.createDecoderByType("video/avc");

            MediaFormat format = MediaFormat.createVideoFormat("video/avc", width, height);
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, width * height);

            decoder.configure(format, surface, null, 0);
            decoder.start();
            initialized = true;
            return true;
        } catch (IOException e) {
            e.printStackTrace();
            return false;
        }
    }

    public void decode(byte[] h264Data, int offset, int length) {
        if (!initialized || decoder == null)
            return;

        try {
            int inputBufferIndex = decoder.dequeueInputBuffer(10000);
            if (inputBufferIndex >= 0) {
                ByteBuffer inputBuffer = decoder.getInputBuffer(inputBufferIndex);
                if (inputBuffer != null) {
                    inputBuffer.clear();
                    inputBuffer.put(h264Data, offset, length);
                    decoder.queueInputBuffer(inputBufferIndex, 0, length, 0, 0);
                }
            }

            MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();
            int outputBufferIndex = decoder.dequeueOutputBuffer(bufferInfo, 0);
            if (outputBufferIndex >= 0) {
                decoder.releaseOutputBuffer(outputBufferIndex, true);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    public void release() {
        if (decoder != null) {
            try {
                decoder.stop();
                decoder.release();
            } catch (Exception e) {
                e.printStackTrace();
            }
            decoder = null;
            initialized = false;
        }
    }

    public void flush() {
        if (decoder != null && initialized) {
            decoder.flush();
        }
    }
}

