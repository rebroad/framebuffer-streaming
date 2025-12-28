package com.framebuffer.client;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Button;
import android.widget.EditText;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;
import java.io.IOException;
import java.net.Socket;

public class MainActivity extends AppCompatActivity implements SurfaceHolder.Callback {
    private SurfaceView surfaceView;
    private EditText serverAddressEdit;
    private EditText serverPortEdit;
    private Button connectButton;

    private Socket socket;
    private FrameReceiver frameReceiver;
    private boolean connected = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        surfaceView = findViewById(R.id.surfaceView);
        serverAddressEdit = findViewById(R.id.serverAddress);
        serverPortEdit = findViewById(R.id.serverPort);
        connectButton = findViewById(R.id.connectButton);

        surfaceView.getHolder().addCallback(this);

        connectButton.setOnClickListener(v -> {
            if (connected) {
                disconnect();
            } else {
                connect();
            }
        });
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // Surface is ready
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // Surface size changed
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        disconnect();
    }

    private void connect() {
        String address = serverAddressEdit.getText().toString().trim();
        String portStr = serverPortEdit.getText().toString().trim();

        if (address.isEmpty() || portStr.isEmpty()) {
            Toast.makeText(this, "Please enter server address and port", Toast.LENGTH_SHORT).show();
            return;
        }

        int port;
        try {
            port = Integer.parseInt(portStr);
        } catch (NumberFormatException e) {
            Toast.makeText(this, "Invalid port number", Toast.LENGTH_SHORT).show();
            return;
        }

        new Thread(() -> {
            try {
                socket = new Socket(address, port);
                connected = true;

                // Send HELLO
                byte[] hello = new byte[36];
                java.nio.ByteBuffer buf = java.nio.ByteBuffer.wrap(hello).order(java.nio.ByteOrder.LITTLE_ENDIAN);
                buf.putShort((short)1); // protocol version
                buf.putShort((short)0); // client type: Android
                // capabilities (32 bytes, all zeros for now)

                socket.getOutputStream().write(new byte[]{Protocol.MSG_HELLO, 0, 0, 0, 36, 0, 0, 0, 0});
                socket.getOutputStream().write(hello);

                // Start frame receiver
                frameReceiver = new FrameReceiver(socket, surfaceView.getHolder());
                frameReceiver.start();

                new Handler(Looper.getMainLooper()).post(() -> {
                    connectButton.setText("Disconnect");
                    Toast.makeText(this, "Connected", Toast.LENGTH_SHORT).show();
                });

            } catch (IOException e) {
                e.printStackTrace();
                new Handler(Looper.getMainLooper()).post(() -> {
                    Toast.makeText(this, "Connection failed: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                });
                connected = false;
            }
        }).start();
    }

    private void disconnect() {
        connected = false;

        if (frameReceiver != null) {
            frameReceiver.stopReceiving();
            frameReceiver = null;
        }

        if (socket != null) {
            try {
                socket.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
            socket = null;
        }

        connectButton.setText("Connect");
        Toast.makeText(this, "Disconnected", Toast.LENGTH_SHORT).show();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        disconnect();
    }
}

