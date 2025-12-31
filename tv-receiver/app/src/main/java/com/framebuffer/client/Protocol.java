package com.framebuffer.client;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class Protocol {
    public static final byte MSG_CLIENT_HELLO = 0x00;  // Streamer's initial hello
    public static final byte MSG_HELLO = 0x01;  // Receiver's capabilities hello
    public static final byte MSG_FRAME = 0x02;
    public static final byte MSG_AUDIO = 0x03;
    public static final byte MSG_CONFIG = 0x05;
    public static final byte MSG_PING = 0x06;
    public static final byte MSG_PONG = 0x07;
    public static final byte MSG_PAUSE = 0x08;        // Pause frame sending (receiver has no surface)
    public static final byte MSG_RESUME = 0x09;      // Resume frame sending (receiver has surface again)
    public static final byte MSG_DISCOVERY_REQUEST = 0x10;  // UDP broadcast discovery request
    public static final byte MSG_DISCOVERY_RESPONSE = 0x11;  // UDP broadcast discovery response
    public static final byte MSG_PIN_VERIFY = 0x12;         // PIN verification request
    public static final byte MSG_PIN_VERIFIED = 0x13;        // PIN verification success
    public static final byte MSG_CAPABILITIES = 0x14;       // Capabilities message (sent immediately after connection)
    public static final byte MSG_ERROR = (byte)0xFF;

    public static class MessageHeader {
        public byte type;
        public int length;
        public int sequence;
    }

    // Encoding modes
    public static final byte ENCODING_MODE_FULL_FRAME = 0;
    public static final byte ENCODING_MODE_DIRTY_RECTS = 1;
    public static final byte ENCODING_MODE_H264 = 2;

    public static class DirtyRectangle {
        public int x, y;
        public int width, height;
        public int dataSize;
    }

    public static class FrameMessage {
        public long timestampUs;  // Timestamp for synchronization
        public int outputId;
        public int width;
        public int height;
        public int format;
        public int pitch;
        public int size;
        public byte encodingMode;  // 0=full frame, 1=dirty rectangles, 2=H.264
        public byte numRegions;   // Number of dirty rectangles (if encodingMode=1)
    }

    public static class AudioMessage {
        public long timestampUs;  // Timestamp for synchronization
        public int sampleRate;
        public int channels;
        public int format;
        public int dataSize;
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
        ByteBuffer header = ByteBuffer.allocate(9).order(ByteOrder.BIG_ENDIAN);
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
        ByteBuffer payload = ByteBuffer.allocate(payloadSize).order(ByteOrder.BIG_ENDIAN);

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

    public static MessageHeader parseHeader(byte[] headerBytes) {
        ByteBuffer header = ByteBuffer.wrap(headerBytes).order(ByteOrder.BIG_ENDIAN);
        MessageHeader msg = new MessageHeader();
        msg.type = header.get();
        msg.length = header.getInt();
        msg.sequence = header.getInt();
        return msg;
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

        ByteBuffer header = ByteBuffer.wrap(headerBytes).order(ByteOrder.BIG_ENDIAN);
        MessageHeader msg = new MessageHeader();
        msg.type = header.get();
        msg.length = header.getInt();
        msg.sequence = header.getInt();
        return msg;
    }

    public static FrameMessage parseFrameMessage(byte[] data) {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN);
        FrameMessage frame = new FrameMessage();
        frame.timestampUs = buf.getLong();
        frame.outputId = buf.getInt();
        frame.width = buf.getInt();
        frame.height = buf.getInt();
        frame.format = buf.getInt();
        frame.pitch = buf.getInt();
        frame.size = buf.getInt();
        if (buf.remaining() >= 2) {
            frame.encodingMode = buf.get();
            frame.numRegions = buf.get();
        } else {
            // Backward compatibility: default to full frame
            frame.encodingMode = ENCODING_MODE_FULL_FRAME;
            frame.numRegions = 0;
        }
        return frame;
    }

    public static DirtyRectangle parseDirtyRectangle(byte[] data) {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN);
        DirtyRectangle rect = new DirtyRectangle();
        rect.x = buf.getInt();
        rect.y = buf.getInt();
        rect.width = buf.getInt();
        rect.height = buf.getInt();
        rect.dataSize = buf.getInt();
        return rect;
    }

    public static AudioMessage parseAudioMessage(byte[] data) {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN);
        AudioMessage audio = new AudioMessage();
        audio.timestampUs = buf.getLong();
        audio.sampleRate = buf.getInt();
        audio.channels = buf.getShort() & 0xFFFF;
        audio.format = buf.getShort() & 0xFFFF;
        audio.dataSize = buf.getInt();
        return audio;
    }

    public static ConfigMessage parseConfigMessage(byte[] data) {
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN);
        ConfigMessage config = new ConfigMessage();
        config.outputId = buf.getInt();
        config.width = buf.getInt();
        config.height = buf.getInt();
        config.refreshRate = buf.getInt();
        return config;
    }
}

