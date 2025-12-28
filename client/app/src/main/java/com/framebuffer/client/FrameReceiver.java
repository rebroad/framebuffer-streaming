package com.framebuffer.client;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.PorterDuff;
import android.view.SurfaceHolder;
import java.io.IOException;
import java.io.InputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class FrameReceiver extends Thread {
    private Socket socket;
    private SurfaceHolder surfaceHolder;
    private boolean running = false;

    public FrameReceiver(Socket socket, SurfaceHolder surfaceHolder) {
        this.socket = socket;
        this.surfaceHolder = surfaceHolder;
    }

    public void stopReceiving() {
        running = false;
        interrupt();
    }

    @Override
    public void run() {
        running = true;
        try {
            InputStream in = socket.getInputStream();

            while (running && !isInterrupted()) {
                Protocol.MessageHeader header = Protocol.receiveHeader(in);

                if (header.type == Protocol.MSG_FRAME) {
                    // Read frame message
                    byte[] frameData = new byte[24]; // FrameMessage size
                    int read = 0;
                    while (read < 24) {
                        int n = in.read(frameData, read, 24 - read);
                        if (n < 0) break;
                        read += n;
                    }

                    Protocol.FrameMessage frame = Protocol.parseFrameMessage(frameData);

                    // Read frame pixel data
                    byte[] pixels = new byte[frame.size];
                    read = 0;
                    while (read < frame.size) {
                        int n = in.read(pixels, read, frame.size - read);
                        if (n < 0) break;
                        read += n;
                    }

                    // Draw to surface
                    drawFrame(frame, pixels);

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

    private void drawFrame(Protocol.FrameMessage frame, byte[] pixels) {
        if (surfaceHolder == null) return;

        Canvas canvas = null;
        try {
            canvas = surfaceHolder.lockCanvas();
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
                surfaceHolder.unlockCanvasAndPost(canvas);
            }
        }
    }
}

