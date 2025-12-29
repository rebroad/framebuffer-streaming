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

                // Query display capabilities
                android.view.Display display = getWindowManager().getDefaultDisplay();
                android.graphics.Point size = new android.graphics.Point();
                display.getRealSize(size);

                // Get display name
                String displayName = "Unknown Display";
                try {
                    android.hardware.display.DisplayManager dm =
                        (android.hardware.display.DisplayManager) getSystemService(DISPLAY_SERVICE);
                    android.view.Display[] displays = dm.getDisplays();
                    if (displays.length > 0) {
                        android.view.Display d = displays[0];
                        // Try to get display name from system properties or display info
                        displayName = d.getName();
                        if (displayName == null || displayName.isEmpty()) {
                            displayName = "Android Display";
                        }
                    }
                } catch (Exception e) {
                    e.printStackTrace();
                }

                // Get refresh rate
                float refreshRate = display.getRefreshRate();
                int refreshRateInt = (int)(refreshRate * 100); // Convert to Hz * 100

                // Create display modes (for now, just the current resolution)
                Protocol.DisplayMode[] modes = new Protocol.DisplayMode[1];
                modes[0] = new Protocol.DisplayMode();
                modes[0].width = size.x;
                modes[0].height = size.y;
                modes[0].refreshRate = refreshRateInt;

                // Send HELLO with display name and modes
                Protocol.sendHello(socket.getOutputStream(), displayName, modes);

                // Start frame receiver
                frameReceiver = new FrameReceiver(socket, surfaceView.getHolder());
                frameReceiver.setConfigCallback(config -> {
                    // Handle config changes on main thread
                    new Handler(Looper.getMainLooper()).post(() -> {
                        if (config.width == 0 || config.height == 0) {
                            // Display disconnected
                            Toast.makeText(MainActivity.this,
                                "Display disconnected (no signal)",
                                Toast.LENGTH_SHORT).show();
                        } else {
                            // Display resolution changed
                            Toast.makeText(MainActivity.this,
                                String.format("Resolution changed: %dx%d@%dHz",
                                    config.width, config.height, config.refreshRate),
                                Toast.LENGTH_SHORT).show();
                        }
                    });
                });
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

