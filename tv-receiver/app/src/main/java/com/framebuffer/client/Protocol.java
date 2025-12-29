package com.framebuffer.client;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class Protocol {
    public static final byte MSG_HELLO = 0x01;
    public static final byte MSG_FRAME = 0x02;
    public static final byte MSG_AUDIO = 0x03;
    public static final byte MSG_INPUT = 0x04;
    public static final byte MSG_CONFIG = 0x05;
    public static final byte MSG_PING = 0x06;
    public static final byte MSG_PONG = 0x07;
    public static final byte MSG_ERROR = (byte)0xFF;

    public static class MessageHeader {
        public byte type;
        public int length;
        public int sequence;
    }

    public static class FrameMessage {
        public int outputId;
        public int width;
        public int height;
        public int format;
        public int pitch;
        public int size;
    }

    public static class DisplayMode {
        public int width;
        public int height;
        public int refreshRate;  // Hz * 100 (e.g., 6000 = 60.00 Hz)
    }

    public static class ConfigMessage {
        public int outputId;
        public int width;
        public int height;
        public int refreshRate;  // Hz
    }

    public static int sendMessage(OutputStream out, byte type, byte[] data) throws IOException {
        ByteBuffer header = ByteBuffer.allocate(9).order(ByteOrder.LITTLE_ENDIAN);
        header.put(type);
        header.putInt(data != null ? data.length : 0);
        header.putInt(0); // sequence
        out.write(header.array());

        if (data != null && data.length > 0) {
            out.write(data);
        }

        return 9 + (data != null ? data.length : 0);
    }

    public static int sendHello(OutputStream out, String displayName, DisplayMode[] modes) throws IOException {
        // Build HELLO message
        byte[] displayNameBytes = displayName.getBytes("UTF-8");
        int displayNameLen = displayNameBytes.length + 1; // Include null terminator

        int payloadSize = 4 + displayNameLen + (modes != null ? modes.length * 12 : 0);
        ByteBuffer payload = ByteBuffer.allocate(payloadSize).order(ByteOrder.LITTLE_ENDIAN);

        // Protocol version
        payload.putShort((short)1);
        // Number of modes
        payload.putShort((short)(modes != null ? modes.length : 0));
        // Display name length
        payload.putShort((short)displayNameLen);
        // Display name (null-terminated)
        payload.put(displayNameBytes);
        payload.put((byte)0);
        // Display modes
        if (modes != null) {
            for (DisplayMode mode : modes) {
                payload.putInt(mode.width);
                payload.putInt(mode.height);
                payload.putInt(mode.refreshRate);
            }
        }

        return sendMessage(out, MSG_HELLO, payload.array());
    }

    public static MessageHeader receiveHeader(InputStream in) throws IOException {
        byte[] headerBytes = new byte[9];
        int read = 0;
        while (read < 9) {
            int n = in.read(headerBytes, read, 9 - read);
            if (n < 0) {
                throw new IOException("Connection closed");
            }
            read += n;
        }

        ByteBuffer header = ByteBuffer.wrap(headerBytes).order(ByteOrder.LITTLE_ENDIAN);
        MessageHeader msg = new MessageHeader();
        msg.type = header.get();
        msg.length = header.getInt();
        msg.sequence = header.getInt();
        return msg;
    }

    public static FrameMessage parseFrameMessage(byte[] data) {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        FrameMessage frame = new FrameMessage();
        frame.outputId = buf.getInt();
        frame.width = buf.getInt();
        frame.height = buf.getInt();
        frame.format = buf.getInt();
        frame.pitch = buf.getInt();
        frame.size = buf.getInt();
        return frame;
    }

    public static ConfigMessage parseConfigMessage(byte[] data) {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        ConfigMessage config = new ConfigMessage();
        config.outputId = buf.getInt();
        config.width = buf.getInt();
        config.height = buf.getInt();
        config.refreshRate = buf.getInt();
        return config;
    }
}

