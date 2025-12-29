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
    private EditText listenPortEdit;
    private Button startStopButton;
    private android.widget.TextView statusText;

    private java.net.ServerSocket serverSocket;
    private Socket clientSocket;
    private FrameReceiver frameReceiver;
    private boolean listening = false;
    private Thread serverThread;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        surfaceView = findViewById(R.id.surfaceView);
        listenPortEdit = findViewById(R.id.serverPort);  // Reuse port field
        startStopButton = findViewById(R.id.connectButton);  // Reuse button
        statusText = findViewById(R.id.statusText);  // We'll need to add this

        surfaceView.getHolder().addCallback(this);

        // Set default port
        listenPortEdit.setText("8888");
        listenPortEdit.setHint("Listen Port");

        startStopButton.setOnClickListener(v -> {
            if (listening) {
                stopListening();
            } else {
                startListening();
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
        stopListening();
    }

    private void startListening() {
        String portStr = listenPortEdit.getText().toString().trim();

        if (portStr.isEmpty()) {
            Toast.makeText(this, "Please enter listen port", Toast.LENGTH_SHORT).show(); // TODO jus pick a spare port, and display this on the TV until the server has connected to it.
            return;
        }

        int port;
        try {
            port = Integer.parseInt(portStr);
        } catch (NumberFormatException e) {
            Toast.makeText(this, "Invalid port number", Toast.LENGTH_SHORT).show();
            return;
        }

        listening = true;
        startStopButton.setText("Stop Listening");
        listenPortEdit.setEnabled(false);

        serverThread = new Thread(() -> {
            try {
                serverSocket = new java.net.ServerSocket(port);

                // Get local IP address for display
                String localIp = getLocalIpAddress();

                new Handler(Looper.getMainLooper()).post(() -> {
                    Toast.makeText(MainActivity.this,
                        "Listening on port " + port + "\nIP: " + localIp,
                        Toast.LENGTH_LONG).show();
                });

                // Wait for X11 server to connect
                clientSocket = serverSocket.accept();

                new Handler(Looper.getMainLooper()).post(() -> {
                    Toast.makeText(MainActivity.this, "X11 server connected", Toast.LENGTH_SHORT).show();
                });

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
                Protocol.sendHello(clientSocket.getOutputStream(), displayName, modes);

                // Start frame receiver
                frameReceiver = new FrameReceiver(clientSocket, surfaceView.getHolder(), MainActivity.this);
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

            } catch (IOException e) {
                e.printStackTrace();
                new Handler(Looper.getMainLooper()).post(() -> {
                    if (listening) {
                        Toast.makeText(MainActivity.this, "Server error: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                    }
                });
            } finally {
                listening = false;
                new Handler(Looper.getMainLooper()).post(() -> {
                    startStopButton.setText("Start Listening");
                    listenPortEdit.setEnabled(true);
                });
            }
        });
        serverThread.start();
    }

    private void stopListening() {
        listening = false;

        if (frameReceiver != null) {
            frameReceiver.stopReceiving();
            frameReceiver = null;
        }

        if (clientSocket != null) {
            try {
                clientSocket.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
            clientSocket = null;
        }

        if (serverSocket != null) {
            try {
                serverSocket.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
            serverSocket = null;
        }

        if (serverThread != null) {
            try {
                serverThread.join(1000);
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
            serverThread = null;
        }

        startStopButton.setText("Start Listening");
        listenPortEdit.setEnabled(true);
        Toast.makeText(this, "Stopped listening", Toast.LENGTH_SHORT).show();
    }

    private String getLocalIpAddress() {
        try {
            java.util.Enumeration<java.net.NetworkInterface> interfaces =
                java.net.NetworkInterface.getNetworkInterfaces();
            while (interfaces.hasMoreElements()) {
                java.net.NetworkInterface iface = interfaces.nextElement();
                java.util.Enumeration<java.net.InetAddress> addresses = iface.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    java.net.InetAddress addr = addresses.nextElement();
                    if (!addr.isLoopbackAddress() && addr instanceof java.net.Inet4Address) {
                        return addr.getHostAddress();
                    }
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        return "Unknown";
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopListening();
    }
}

