package com.framebuffer.client;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Typeface;
import android.view.SurfaceHolder;
import java.io.IOException;
import java.io.InputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

public class FrameReceiver extends Thread {
    public interface ConfigCallback {
        void onConfigChanged(Protocol.ConfigMessage config);
    }

    private Socket socket;
    private SurfaceHolder surfaceHolder;
    private boolean running = false;
    private ConfigCallback configCallback;
    private android.content.Context context;
    private AudioReceiver audioReceiver;
    private NoiseEncryption noiseEncryption;  // Noise Protocol encryption context

    // Current configuration state
    private int currentWidth = 0;
    private int currentHeight = 0;
    private boolean connected = false;
    private float savedBrightness = -1.0f;  // Store original brightness
    private Bitmap currentFrameBitmap;  // Store current frame for dirty rectangle compositing
    private H264Decoder h264Decoder;  // H.264 decoder for encoded frames

    public FrameReceiver(Socket socket, SurfaceHolder surfaceHolder, android.content.Context context, NoiseEncryption noiseEncryption) {
        this.socket = socket;
        this.surfaceHolder = surfaceHolder;
        this.context = context;
        this.noiseEncryption = noiseEncryption;
    }

    public void setConfigCallback(ConfigCallback callback) {
        this.configCallback = callback;
    }

    public void updateSurfaceHolder(SurfaceHolder newSurfaceHolder) {
        // Update the SurfaceHolder when surface is recreated (e.g., app comes back to foreground)
        synchronized (this) {
            this.surfaceHolder = newSurfaceHolder;
            // If H.264 decoder exists, we need to recreate it with the new surface
            if (h264Decoder != null) {
                h264Decoder.release();
                h264Decoder = null; // Will be recreated on next frame with new surface
            }
        }
    }

    public void stopReceiving() {
        running = false;
        if (audioReceiver != null) {
            audioReceiver.stop();
            audioReceiver = null;
        }
        if (h264Decoder != null) {
            h264Decoder.release();
            h264Decoder = null;
        }
        interrupt();
    }

    @Override
    public void run() {
        running = true;
        connected = true;  // Assume connected initially until we receive a CONFIG message saying otherwise
        try {
            InputStream in = socket.getInputStream();

            while (running && !isInterrupted()) {
                // Receive header (encrypted if Noise is ready)
                Protocol.MessageHeader header;
                if (noiseEncryption != null && noiseEncryption.isReady()) {
                    byte[] headerBytes = noiseEncryption.recv(socket, 9);
                    if (headerBytes == null || headerBytes.length != 9) {
                        break; // Connection closed
                    }
                    header = Protocol.parseHeader(headerBytes);
                } else {
                    header = Protocol.receiveHeader(in);
                }

                if (header.type == Protocol.MSG_FRAME) {
                    // Read frame message (now 34 bytes: 32 + encoding_mode + num_regions) - encrypted
                    byte[] frameData;
                    if (noiseEncryption != null && noiseEncryption.isReady()) {
                        frameData = noiseEncryption.recv(socket, 34);
                        if (frameData == null || frameData.length != 34) {
                            break; // Connection closed
                        }
                    } else {
                        frameData = new byte[34];
                        int read = 0;
                        while (read < 34) {
                            int n = in.read(frameData, read, 34 - read);
                            if (n < 0) break;
                            read += n;
                        }
                    }

                    Protocol.FrameMessage frame = Protocol.parseFrameMessage(frameData);

                    // Only process frames if display is connected
                    if (connected && frame.width > 0 && frame.height > 0) {
                        if (frame.encodingMode == Protocol.ENCODING_MODE_H264) {
                            // Handle H.264 encoded frame
                            drawH264Frame(frame, in);
                        } else if (frame.encodingMode == Protocol.ENCODING_MODE_DIRTY_RECTS && frame.numRegions > 0) {
                            // Handle dirty rectangles
                            drawDirtyRectangles(frame, in);
                        } else {
                            // Handle full frame (backward compatible)
                            byte[] pixels = new byte[frame.size];
                            int read = 0;
                            while (read < frame.size) {
                                int n = in.read(pixels, read, frame.size - read);
                                if (n < 0) break;
                                read += n;
                            }

                            // Draw to surface
                            drawFrame(frame, pixels);
                        }
                    } else {
                        // Skip frame data if display is disconnected
                        if (frame.size > 0) {
                            in.skip(frame.size);
                        }
                    }

                } else if (header.type == Protocol.MSG_CONFIG) {
                    // Read config message
                    byte[] configData = new byte[16]; // ConfigMessage size
                    int read = 0;
                    while (read < 16) {
                        int n = in.read(configData, read, 16 - read);
                        if (n < 0) break;
                        read += n;
                    }

                    Protocol.ConfigMessage config = Protocol.parseConfigMessage(configData);

                    // Update state
                    boolean wasConnected = connected;
                    currentWidth = config.width;
                    currentHeight = config.height;
                    connected = (config.width > 0 && config.height > 0);

                    // Handle connection state changes
                    if (wasConnected != connected) {
                        if (!connected) {
                            // Display turned off - turn off screen brightness
                            turnOffDisplay();
                        } else {
                            // Display turned on - restore screen brightness
                            turnOnDisplay();
                        }
                    }

                    // Notify callback
                    if (configCallback != null) {
                        configCallback.onConfigChanged(config);
                    }

                } else if (header.type == Protocol.MSG_AUDIO) {
                    // Read audio message
                    byte[] audioData = new byte[20]; // AudioMessage size
                    int read = 0;
                    while (read < 20) {
                        int n = in.read(audioData, read, 20 - read);
                        if (n < 0) break;
                        read += n;
                    }

                    Protocol.AudioMessage audio = Protocol.parseAudioMessage(audioData);

                    // Initialize audio receiver if needed
                    if (audioReceiver == null && audio.sampleRate > 0 && audio.channels > 0) {
                        audioReceiver = new AudioReceiver(audio.sampleRate, audio.channels, audio.format);
                        audioReceiver.start();
                    }

                    // Read audio data and forward to AudioReceiver
                    if (audio.dataSize > 0 && audioReceiver != null) {
                        byte[] audioBytes = new byte[audio.dataSize];
                        int audioRead = 0;
                        while (audioRead < audio.dataSize) {
                            int n = in.read(audioBytes, audioRead, audio.dataSize - audioRead);
                            if (n < 0) break;
                            audioRead += n;
                        }
                        if (audioRead == audio.dataSize) {
                            audioReceiver.addAudioData(audioBytes);
                        }
                    } else if (audio.dataSize > 0) {
                        // Skip if audio receiver not initialized
                        in.skip(audio.dataSize);
                    }

                } else if (header.type == Protocol.MSG_PING) {
                    // Respond to ping
                    socket.getOutputStream().write(new byte[]{Protocol.MSG_PONG, 0, 0, 0, 0, 0, 0, 0, 0});
                } else {
                    // Skip unknown message
                    if (header.length > 0) {
                        in.skip(header.length);
                    }
                }
            }
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    private void drawH264Frame(Protocol.FrameMessage frame, InputStream in) {
        SurfaceHolder holder;
        synchronized (this) {
            holder = this.surfaceHolder;
        }
        if (holder == null) return;

        try {
            // Initialize or recreate H.264 decoder if needed
            if (h264Decoder == null ||
                h264Decoder.getWidth() != frame.width ||
                h264Decoder.getHeight() != frame.height) {
                if (h264Decoder != null) {
                    h264Decoder.release();
                }

                // Get Surface from SurfaceHolder
                android.view.Surface surface = holder.getSurface();
                if (surface != null && surface.isValid()) {
                    h264Decoder = new H264Decoder(frame.width, frame.height, surface);
                    if (!h264Decoder.initialize()) {
                        h264Decoder = null;
                        return;
                    }
                } else {
                    return;
                }
            }

            // Read H.264 encoded data
            byte[] h264Data = new byte[frame.size];
            int read = 0;
            while (read < frame.size) {
                int n = in.read(h264Data, read, frame.size - read);
                if (n < 0) break;
                read += n;
            }

            if (read == frame.size && h264Decoder != null) {
                // Decode and display
                h264Decoder.decode(h264Data, 0, frame.size);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private void drawDirtyRectangles(Protocol.FrameMessage frame, InputStream in) {
        SurfaceHolder holder;
        synchronized (this) {
            holder = this.surfaceHolder;
        }
        if (holder == null) return;

        Canvas canvas = null;
        try {
            canvas = holder.lockCanvas();
            if (canvas == null) return;

            // Initialize or resize frame bitmap if needed
            if (currentFrameBitmap == null ||
                currentFrameBitmap.getWidth() != frame.width ||
                currentFrameBitmap.getHeight() != frame.height) {
                if (currentFrameBitmap != null) {
                    currentFrameBitmap.recycle();
                }
                currentFrameBitmap = Bitmap.createBitmap(frame.width, frame.height, Bitmap.Config.ARGB_8888);
                // Clear to black initially
                currentFrameBitmap.eraseColor(Color.BLACK);
            }

            // Read and draw each dirty rectangle
            Canvas frameCanvas = new Canvas(currentFrameBitmap);
            for (int i = 0; i < frame.numRegions; i++) {
                // Read rectangle header (20 bytes)
                byte[] rectData = new byte[20];
                int read = 0;
                while (read < 20) {
                    int n = in.read(rectData, read, 20 - read);
                    if (n < 0) break;
                    read += n;
                }

                Protocol.DirtyRectangle rect = Protocol.parseDirtyRectangle(rectData);

                // Read rectangle pixel data
                byte[] rectPixels = new byte[rect.dataSize];
                read = 0;
                while (read < rect.dataSize) {
                    int n = in.read(rectPixels, read, rect.dataSize - read);
                    if (n < 0) break;
                    read += n;
                }

                // Convert rectangle pixels to Bitmap
                Bitmap rectBitmap = Bitmap.createBitmap(rect.width, rect.height, Bitmap.Config.ARGB_8888);
                int[] pixelArray = new int[rect.width * rect.height];
                ByteBuffer.wrap(rectPixels).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer().get(pixelArray);
                rectBitmap.setPixels(pixelArray, 0, rect.width, 0, 0, rect.width, rect.height);

                // Update the stored frame bitmap with this rectangle
                frameCanvas.drawBitmap(rectBitmap, rect.x, rect.y, null);
            }

            // Draw the complete updated frame to surface
            float scaleX = (float)canvas.getWidth() / frame.width;
            float scaleY = (float)canvas.getHeight() / frame.height;
            float scale = Math.min(scaleX, scaleY);

            int scaledWidth = (int)(frame.width * scale);
            int scaledHeight = (int)(frame.height * scale);
            int x = (canvas.getWidth() - scaledWidth) / 2;
            int y = (canvas.getHeight() - scaledHeight) / 2;

            canvas.drawBitmap(currentFrameBitmap, null,
                new android.graphics.Rect(x, y, x + scaledWidth, y + scaledHeight), null);
        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            if (canvas != null) {
                holder.unlockCanvasAndPost(canvas);
            }
        }
    }

    private void drawFrame(Protocol.FrameMessage frame, byte[] pixels) {
        SurfaceHolder holder;
        synchronized (this) {
            holder = this.surfaceHolder;
        }
        if (holder == null) return;

        Canvas canvas = null;
        try {
            canvas = holder.lockCanvas();
            if (canvas == null) return;

            canvas.drawColor(0, PorterDuff.Mode.CLEAR);

            // Convert ARGB8888 to Bitmap
            // Note: pixels are in ARGB8888 format, but we need to handle endianness
            Bitmap bitmap = Bitmap.createBitmap(frame.width, frame.height, Bitmap.Config.ARGB_8888);
            int[] pixelArray = new int[frame.width * frame.height];
            ByteBuffer.wrap(pixels).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer().get(pixelArray);
            bitmap.setPixels(pixelArray, 0, frame.width, 0, 0, frame.width, frame.height);

            // Scale to fit surface
            float scaleX = (float)canvas.getWidth() / frame.width;
            float scaleY = (float)canvas.getHeight() / frame.height;
            float scale = Math.min(scaleX, scaleY);

            int scaledWidth = (int)(frame.width * scale);
            int scaledHeight = (int)(frame.height * scale);
            int x = (canvas.getWidth() - scaledWidth) / 2;
            int y = (canvas.getHeight() - scaledHeight) / 2;

            canvas.drawBitmap(bitmap, null,
                new android.graphics.Rect(x, y, x + scaledWidth, y + scaledHeight), null);
        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            if (canvas != null) {
                holder.unlockCanvasAndPost(canvas);
            }
        }
    }

    private void turnOffDisplay() {
        if (context == null) return;

        // Turn off display by setting brightness to 0
        try {
            // Save current brightness if not already saved
            if (savedBrightness < 0) {
                android.view.Window window = ((android.app.Activity) context).getWindow();
                if (window != null) {
                    android.view.WindowManager.LayoutParams params = window.getAttributes();
                    savedBrightness = params.screenBrightness;
                    if (savedBrightness < 0) {
                        // Window uses system brightness, get it from settings
                        try {
                            int systemBrightness = android.provider.Settings.System.getInt(
                                context.getContentResolver(),
                                android.provider.Settings.System.SCREEN_BRIGHTNESS, 128);
                            savedBrightness = systemBrightness / 255.0f;
                        } catch (Exception e) {
                            // Fallback to default
                            savedBrightness = 0.5f;
                        }
                    }
                } else {
                    savedBrightness = 0.5f;  // Default if window not available
                }
            }

            // Set brightness to 0 (effectively turns off display)
            android.view.Window window = ((android.app.Activity) context).getWindow();
            if (window != null) {
                android.view.WindowManager.LayoutParams params = window.getAttributes();
                params.screenBrightness = 0.0f;
                window.setAttributes(params);
            }

            // Also clear the surface to black
            clearSurface();
        } catch (Exception e) {
            e.printStackTrace();
            // Fallback: clear surface to black
            clearSurface();
        }
    }

    private void turnOnDisplay() {
        if (context == null) return;

        // Restore display brightness
        try {
            android.view.Window window = ((android.app.Activity) context).getWindow();
            if (window != null) {
                android.view.WindowManager.LayoutParams params = window.getAttributes();
                // Restore saved brightness, or use default if not saved
                if (savedBrightness >= 0) {
                    params.screenBrightness = savedBrightness;
                } else {
                    params.screenBrightness = -1.0f;  // Use system default
                }
                window.setAttributes(params);
            }
            savedBrightness = -1.0f;  // Reset saved brightness
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private void clearSurface() {
        SurfaceHolder holder;
        synchronized (this) {
            holder = this.surfaceHolder;
        }
        if (holder == null) return;

        Canvas canvas = null;
        try {
            canvas = holder.lockCanvas();
            if (canvas == null) return;
            canvas.drawColor(Color.BLACK);
        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            if (canvas != null) {
                holder.unlockCanvasAndPost(canvas);
            }
        }
    }
}

