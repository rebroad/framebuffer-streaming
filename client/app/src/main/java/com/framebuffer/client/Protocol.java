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
}

