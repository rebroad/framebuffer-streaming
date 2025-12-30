package com.framebuffer.client;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;
import java.io.IOException;
import java.net.Socket;

public class MainActivity extends AppCompatActivity implements SurfaceHolder.Callback {
    private SurfaceView surfaceView;
    private android.widget.TextView statusText;

    private java.net.ServerSocket serverSocket;
    private Socket clientSocket;
    private FrameReceiver frameReceiver;
    private boolean listening = false;
    private Thread serverThread;
    private NoiseEncryption currentNoiseEncryption;  // Current Noise encryption context for active connection
    private android.hardware.display.DisplayManager displayManager;
    private android.hardware.display.DisplayManager.DisplayListener displayListener;
    private android.app.Presentation tvPresentation;
    private SurfaceView tvSurfaceView;
    private int pinCode;  // 4-digit PIN (0-9999)
    private java.net.DatagramSocket udpSocket;
    private Thread udpListenerThread;
    private int tcpPort;  // TCP port for connections

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        surfaceView = findViewById(R.id.surfaceView);
        statusText = findViewById(R.id.statusText);

        surfaceView.getHolder().addCallback(this);

        // Set up display manager to detect TV/external display
        displayManager = (android.hardware.display.DisplayManager) getSystemService(DISPLAY_SERVICE);
        displayListener = new android.hardware.display.DisplayManager.DisplayListener() {
            @Override
            public void onDisplayAdded(int displayId) {
                updateDisplayVisibility();
            }

            @Override
            public void onDisplayRemoved(int displayId) {
                updateDisplayVisibility();
            }

            @Override
            public void onDisplayChanged(int displayId) {
                updateDisplayVisibility();
            }
        };
        displayManager.registerDisplayListener(displayListener, new Handler(Looper.getMainLooper()));

        // Initial display visibility check
        updateDisplayVisibility();

        // Automatically start listening when app opens
        startListening();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // Surface is recreated (e.g., app came back to foreground)
        // Update FrameReceiver with new SurfaceHolder if it exists
        if (frameReceiver != null) {
            frameReceiver.updateSurfaceHolder(holder);
            android.util.Log.i("MainActivity", "Updated FrameReceiver with new SurfaceHolder");
        }
        // Tell streamer to resume sending frames now that we can render them
        sendResumeMessage();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // Surface size changed
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Surface can be destroyed when app goes to background, but we keep listening
        // Only stop listening when activity is actually destroyed (onDestroy)
        // Tell streamer to pause sending frames since we can't render them
        sendPauseMessage();
    }

    private void startListening() {
        // Use default port
        int port = 4321;
        tcpPort = port;
        listening = true;
        android.util.Log.i("MainActivity", "startListening() called, port=" + port);

        // Generate random 4-digit PIN (0-9999)
        pinCode = new java.util.Random().nextInt(10000);
        android.util.Log.i("MainActivity", "Generated PIN: " + String.format("%04d", pinCode));

        serverThread = new Thread(() -> {
            try {
                android.util.Log.i("MainActivity", "Creating ServerSocket on port " + port);
                // Bind to IPv4 explicitly to ensure compatibility
                serverSocket = new java.net.ServerSocket();
                serverSocket.bind(new java.net.InetSocketAddress("0.0.0.0", port));
                android.util.Log.i("MainActivity", "ServerSocket created successfully, listening on port " + port);
                android.util.Log.i("MainActivity", "ServerSocket local address: " + serverSocket.getLocalSocketAddress());

                // Get all local IP addresses for display
                java.util.List<String> localIps = getAllLocalIpAddresses();
                android.util.Log.i("MainActivity", "Local IP addresses: " + String.join(", ", localIps));

                // Start UDP broadcast listener for discovery (use first IP for discovery)
                String firstIp = localIps.isEmpty() ? "0.0.0.0" : localIps.get(0);
                startUdpBroadcastListener(port, firstIp);

                // Update status text with all IPs
                final String ipList = String.join("\n", localIps);
                new Handler(Looper.getMainLooper()).post(() -> {
                    String displayText = ipList + "\nPort: " + port + "\nPIN: " + String.format("%04d", pinCode);
                    statusText.setText(displayText);
                    // Display connection info on TV (use first IP for TV display)
                    displayConnectionInfoOnTV(port, firstIp, pinCode);
                });

                // Keep accepting connections in a loop
                android.util.Log.i("MainActivity", "Starting to accept connections on port " + port);
                while (listening) {
                    Socket acceptedSocket = null;
                    try {
                        // Wait for X11 server to connect
                        android.util.Log.d("MainActivity", "Waiting for connection on " + serverSocket.getLocalSocketAddress());
                        acceptedSocket = serverSocket.accept();
                        android.util.Log.i("MainActivity", "Connection accepted from " + acceptedSocket.getRemoteSocketAddress());
                        clientSocket = acceptedSocket;

                        // Perform Noise Protocol handshake FIRST (before any sensitive data exchange)
                        NoiseEncryption noiseEncryption = new NoiseEncryption(false);  // Receiver is responder
                        currentNoiseEncryption = noiseEncryption;  // Store for use in verifyPinFromClient
                        try {
                            if (!noiseEncryption.handshake(acceptedSocket)) {
                                android.util.Log.e("MainActivity", "Noise Protocol handshake failed");
                                noiseEncryption.cleanup();
                                currentNoiseEncryption = null;
                                acceptedSocket.close();
                                continue; // Continue listening for next connection
                            }
                            if (!noiseEncryption.isReady()) {
                                android.util.Log.e("MainActivity", "Noise Protocol handshake incomplete");
                                noiseEncryption.cleanup();
                                currentNoiseEncryption = null;
                                acceptedSocket.close();
                                continue; // Continue listening for next connection
                            }
                            android.util.Log.i("MainActivity", "Noise Protocol encryption established");
                        } catch (IOException e) {
                            android.util.Log.e("MainActivity", "Noise Protocol handshake error", e);
                            noiseEncryption.cleanup();
                            currentNoiseEncryption = null;
                            acceptedSocket.close();
                            continue; // Continue listening for next connection
                        }

                        // Now verify PIN over encrypted channel
                        if (!verifyPinFromClient(acceptedSocket, noiseEncryption)) {
                            android.util.Log.w("MainActivity", "PIN verification failed, closing connection");
                            noiseEncryption.cleanup();
                            currentNoiseEncryption = null;
                            acceptedSocket.close();
                            continue; // Continue listening for next connection
                        }

                        new Handler(Looper.getMainLooper()).post(() -> {
                            statusText.setText("Connected to X11 streamer");
                            Toast.makeText(MainActivity.this, "X11 server connected", Toast.LENGTH_SHORT).show();
                            // Clear the connection info from TV (frame receiver will start drawing)
                            SurfaceHolder holder = surfaceView.getHolder();
                            android.graphics.Canvas canvas = null;
                            try {
                                canvas = holder.lockCanvas();
                                if (canvas != null) {
                                    canvas.drawColor(android.graphics.Color.BLACK);
                                    holder.unlockCanvasAndPost(canvas);
                                }
                            } catch (Exception e) {
                                e.printStackTrace();
                                if (canvas != null) {
                                    try {
                                        holder.unlockCanvasAndPost(canvas);
                                    } catch (Exception e2) {
                                        e2.printStackTrace();
                                    }
                                }
                            }
                        });

                        // Query display capabilities - use TV if connected, otherwise phone display
                        android.view.Display targetDisplay = getTargetDisplay();
                        android.graphics.Point size = new android.graphics.Point();
                        targetDisplay.getRealSize(size);

                        // Get refresh rate from target display
                        float refreshRate = targetDisplay.getRefreshRate();
                        int refreshRateInt = (int)(refreshRate * 100); // Convert to Hz * 100

                        String displayName = null;
                        Protocol.DisplayMode[] modes = null;

                        // Try to get EDID information (Option 2: System Properties, Option 4: DRM)
                        byte[] edidData = null;

                        // Option 4: Try to get EDID from DRM/KMS directly (most reliable)
                        try {
                            edidData = EdidParser.getEdidFromDrm();
                            if (edidData != null && edidData.length > 0) {
                                android.util.Log.d("MainActivity", "Got EDID from DRM, size: " + edidData.length);
                            }
                        } catch (Exception e) {
                            android.util.Log.w("MainActivity", "Failed to get EDID from DRM: " + e.getMessage());
                        }

                        // Option 2: Fallback to system properties
                        if (edidData == null || edidData.length == 0) {
                            try {
                                edidData = EdidParser.getEdidFromSystemProperties();
                                if (edidData != null && edidData.length > 0) {
                                    android.util.Log.d("MainActivity", "Got EDID from system properties, size: " + edidData.length);
                                }
                            } catch (Exception e) {
                                android.util.Log.w("MainActivity", "Failed to get EDID from system properties: " + e.getMessage());
                            }
                        }

                        // Parse EDID if we got it
                        if (edidData != null && edidData.length > 0) {
                            try {
                                EdidParser.EdidInfo edidInfo = EdidParser.parseEdid(edidData);
                                if (edidInfo != null && edidInfo.modes != null && edidInfo.modes.length > 0) {
                                    displayName = edidInfo.displayName;
                                    modes = edidInfo.modes;
                                    android.util.Log.d("MainActivity", "Parsed EDID: " + displayName + ", " + modes.length + " modes");
                                }
                            } catch (Exception e) {
                                android.util.Log.w("MainActivity", "Failed to parse EDID: " + e.getMessage());
                            }
                        }

                        // Fallback: Use only what Android provides (current mode only)
                        if (modes == null || modes.length == 0) {
                            android.util.Log.d("MainActivity", "No EDID available, using current mode only");

                            // Get display name from the target display
                            displayName = targetDisplay.getName();
                            if (displayName == null || displayName.isEmpty()) {
                                if (isTvConnected()) {
                                    displayName = "TV Display";
                                } else {
                                    displayName = "Phone Display";
                                }
                            }

                            // Only report current mode (what we actually know)
                            modes = new Protocol.DisplayMode[1];
                            modes[0] = new Protocol.DisplayMode();
                            modes[0].width = size.x;
                            modes[0].height = size.y;
                            modes[0].refreshRate = refreshRateInt;
                        }

                        // Send HELLO with display name and modes (encrypted)
                        if (noiseEncryption != null && noiseEncryption.isReady()) {
                            // Build HELLO message
                            java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream();
                            Protocol.sendHello(baos, displayName, modes);
                            noiseEncryption.send(acceptedSocket, baos.toByteArray());
                        } else {
                            // Fallback to unencrypted (should not happen)
                            Protocol.sendHello(acceptedSocket.getOutputStream(), displayName, modes);
                        }

                        // Start frame receiver - use appropriate SurfaceHolder
                        // If TV is connected, use TV's SurfaceView; otherwise use phone's SurfaceView
                        SurfaceHolder targetHolder;
                        if (tvSurfaceView != null && tvSurfaceView.getHolder() != null) {
                            // TV is connected - use TV SurfaceView
                            targetHolder = tvSurfaceView.getHolder();
                        } else {
                            // No TV - use phone SurfaceView
                            targetHolder = surfaceView.getHolder();
                        }
                        frameReceiver = new FrameReceiver(acceptedSocket, targetHolder, MainActivity.this, noiseEncryption);
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

                        // Wait for frame receiver to finish (connection closed)
                        try {
                            frameReceiver.join();
                        } catch (InterruptedException e) {
                            android.util.Log.w("MainActivity", "Frame receiver thread interrupted");
                        }

                        // Connection ended - clean up and continue listening
                        frameReceiver = null;
                        clientSocket = null;

                        // Restore listening display
                        final String ipListRestore = String.join("\n", localIps);
                        new Handler(Looper.getMainLooper()).post(() -> {
                            String displayText = ipListRestore + "\nPort: " + port + "\nPIN: " + String.format("%04d", pinCode);
                            statusText.setText(displayText);
                            displayConnectionInfoOnTV(port, firstIp, pinCode);
                        });

                    } catch (IOException e) {
                        android.util.Log.e("MainActivity", "Error accepting connection", e);
                        if (acceptedSocket != null) {
                            try {
                                acceptedSocket.close();
                            } catch (IOException e2) {
                                // Ignore
                            }
                        }
                        // Continue listening for next connection
                    } catch (Exception e) {
                        android.util.Log.e("MainActivity", "Unexpected error handling connection", e);
                        if (acceptedSocket != null) {
                            try {
                                acceptedSocket.close();
                            } catch (IOException e2) {
                                // Ignore
                            }
                        }
                        // Continue listening for next connection
                    }
                }
                android.util.Log.i("MainActivity", "Stopped accepting connections (listening=" + listening + ")");
            } catch (java.net.BindException e) {
                android.util.Log.e("MainActivity", "Failed to bind ServerSocket to port " + port + ": " + e.getMessage(), e);
                new Handler(Looper.getMainLooper()).post(() -> {
                    Toast.makeText(MainActivity.this, "Failed to bind to port " + port + ": " + e.getMessage(), Toast.LENGTH_LONG).show();
                    statusText.setText("Error: Failed to bind to port " + port);
                });
            } catch (IOException e) {
                android.util.Log.e("MainActivity", "Server socket error", e);
                new Handler(Looper.getMainLooper()).post(() -> {
                    if (listening) {
                        Toast.makeText(MainActivity.this, "Server error: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                        statusText.setText("Error: " + e.getMessage());
                    }
                });
            }
        });
        serverThread.start();
    }

    private void sendPauseMessage() {
        if (clientSocket == null || clientSocket.isClosed()) {
            return;
        }
        try {
            if (currentNoiseEncryption != null && currentNoiseEncryption.isReady()) {
                // Build proper protocol message with header
                java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream();
                Protocol.sendMessage(baos, Protocol.MSG_PAUSE, null);
                currentNoiseEncryption.send(clientSocket, baos.toByteArray());
            } else {
                Protocol.sendMessage(clientSocket.getOutputStream(), Protocol.MSG_PAUSE, null);
            }
            android.util.Log.i("MainActivity", "Sent PAUSE message to streamer");
        } catch (IOException e) {
            android.util.Log.w("MainActivity", "Failed to send PAUSE message", e);
        }
    }

    private void sendResumeMessage() {
        if (clientSocket == null || clientSocket.isClosed()) {
            return;
        }
        try {
            if (currentNoiseEncryption != null && currentNoiseEncryption.isReady()) {
                // Build proper protocol message with header
                java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream();
                Protocol.sendMessage(baos, Protocol.MSG_RESUME, null);
                currentNoiseEncryption.send(clientSocket, baos.toByteArray());
            } else {
                Protocol.sendMessage(clientSocket.getOutputStream(), Protocol.MSG_RESUME, null);
            }
            android.util.Log.i("MainActivity", "Sent RESUME message to streamer");
        } catch (IOException e) {
            android.util.Log.w("MainActivity", "Failed to send RESUME message", e);
        }
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

        // Stop UDP listener
        stopUdpBroadcastListener();

        Toast.makeText(this, "Stopped listening", Toast.LENGTH_SHORT).show();
    }

    private void startUdpBroadcastListener(int tcpPort, String localIp) {
        udpListenerThread = new Thread(() -> {
            try {
                udpSocket = new java.net.DatagramSocket(4321);  // Default broadcast port
                udpSocket.setBroadcast(true);
                udpSocket.setReuseAddress(true);

                byte[] buffer = new byte[1024];
                java.net.DatagramPacket packet = new java.net.DatagramPacket(buffer, buffer.length);

                while (listening) {
                    try {
                        udpSocket.receive(packet);

                        // Check if it's a discovery request
                        if (packet.getLength() >= 9) {  // Minimum header size
                            byte[] data = packet.getData();
                            if (data[0] == Protocol.MSG_DISCOVERY_REQUEST) {
                                // Parse header
                                int length = (data[1] & 0xFF) | ((data[2] & 0xFF) << 8) |
                                             ((data[3] & 0xFF) << 16) | ((data[4] & 0xFF) << 24);

                                // Send discovery response
                                sendDiscoveryResponse(packet.getAddress(), packet.getPort(), tcpPort, localIp);
                            }
                        }
                    } catch (java.io.IOException e) {
                        if (listening) {
                            android.util.Log.e("MainActivity", "UDP receive error", e);
                        }
                    }
                }
            } catch (java.net.SocketException e) {
                if (listening) {
                    android.util.Log.e("MainActivity", "Failed to create UDP socket", e);
                }
            }
        });
        udpListenerThread.start();
    }

    private void stopUdpBroadcastListener() {
        if (udpSocket != null) {
            udpSocket.close();
            udpSocket = null;
        }
        if (udpListenerThread != null) {
            try {
                udpListenerThread.join(1000);
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
            udpListenerThread = null;
        }
    }

    private void sendDiscoveryResponse(java.net.InetAddress clientAddr, int clientPort, int tcpPort, String displayName) {
        try {
            java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream();
            java.io.DataOutputStream dos = new java.io.DataOutputStream(baos);

            // Message header
            dos.writeByte(Protocol.MSG_DISCOVERY_RESPONSE);
            dos.writeInt(0);  // Length placeholder
            dos.writeInt(0);  // Sequence

            // Discovery response payload
            dos.writeShort(tcpPort);
            byte[] nameBytes = displayName.getBytes("UTF-8");
            dos.writeShort(nameBytes.length);
            dos.write(nameBytes);

            // Update length
            byte[] data = baos.toByteArray();
            int length = data.length - 9;  // Exclude header
            data[1] = (byte)(length & 0xFF);
            data[2] = (byte)((length >> 8) & 0xFF);
            data[3] = (byte)((length >> 16) & 0xFF);
            data[4] = (byte)((length >> 24) & 0xFF);

            java.net.DatagramPacket response = new java.net.DatagramPacket(
                data, data.length, clientAddr, clientPort);
            udpSocket.send(response);
        } catch (java.io.IOException e) {
            android.util.Log.e("MainActivity", "Failed to send discovery response", e);
        }
    }

    private boolean verifyPinFromClient(Socket socket, NoiseEncryption noiseEncryption) {
        try {
            java.io.InputStream in = socket.getInputStream();
            java.io.OutputStream out = socket.getOutputStream();

            // Read PIN verification message header (encrypted)
            byte[] headerBytes;
            if (noiseEncryption != null && noiseEncryption.isReady()) {
                // Read encrypted header
                headerBytes = noiseEncryption.recv(socket, 9);
                if (headerBytes == null || headerBytes.length != 9) {
                    return false;
                }
            } else {
                // Fallback to unencrypted (should not happen after handshake)
                headerBytes = new byte[9];
                int read = 0;
                while (read < 9) {
                    int n = in.read(headerBytes, read, 9 - read);
                    if (n < 0) return false;
                    read += n;
                }
            }

            if (headerBytes[0] != Protocol.MSG_PIN_VERIFY) {
                return false;
            }

            // Read PIN (encrypted)
            int length = (headerBytes[1] & 0xFF) | ((headerBytes[2] & 0xFF) << 8) |
                         ((headerBytes[3] & 0xFF) << 16) | ((headerBytes[4] & 0xFF) << 24);
            if (length < 2) return false;

            byte[] pinData;
            if (noiseEncryption != null && noiseEncryption.isReady()) {
                pinData = noiseEncryption.recv(socket, 2);
                if (pinData == null || pinData.length != 2) {
                    return false;
                }
            } else {
                // Fallback to unencrypted
                pinData = new byte[2];
                int read = 0;
                while (read < 2) {
                    int n = in.read(pinData, read, 2 - read);
                    if (n < 0) return false;
                    read += n;
                }
            }

            int receivedPin = (pinData[0] & 0xFF) | ((pinData[1] & 0xFF) << 8);

            // Verify PIN
            if (receivedPin == pinCode) {
                // Send PIN verified response (encrypted)
                java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream();
                baos.write(Protocol.MSG_PIN_VERIFIED);
                baos.write(new byte[4]);  // Length = 0
                baos.write(new byte[4]);  // Sequence = 0
                byte[] response = baos.toByteArray();

                if (noiseEncryption != null && noiseEncryption.isReady()) {
                    noiseEncryption.send(socket, response);
                } else {
                    // Fallback to unencrypted
                    out.write(response);
                    out.flush();
                }
                return true;
            } else {
                return false;
            }
        } catch (java.io.IOException e) {
            android.util.Log.e("MainActivity", "PIN verification error", e);
            return false;
        }
    }

    private java.util.List<String> getAllLocalIpAddresses() {
        java.util.List<String> ipList = new java.util.ArrayList<>();
        try {
            java.util.Enumeration<java.net.NetworkInterface> interfaces =
                java.net.NetworkInterface.getNetworkInterfaces();
            while (interfaces.hasMoreElements()) {
                java.net.NetworkInterface iface = interfaces.nextElement();
                // Skip interfaces that are down
                if (!iface.isUp()) continue;

                java.util.Enumeration<java.net.InetAddress> addresses = iface.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    java.net.InetAddress addr = addresses.nextElement();
                    if (!addr.isLoopbackAddress() && addr instanceof java.net.Inet4Address) {
                        ipList.add(addr.getHostAddress());
                    }
                }
            }
        } catch (java.net.SocketException e) {
            android.util.Log.e("MainActivity", "Error getting network interfaces", e);
        }
        if (ipList.isEmpty()) {
            ipList.add("0.0.0.0");
        }
        return ipList;
    }

    private void displayConnectionInfoOnTV(int port, String ip, int pin) {
        // Draw connection info on the SurfaceView so it's visible on TV
        SurfaceHolder holder = surfaceView.getHolder();
        if (tvSurfaceView != null && tvSurfaceView.getHolder() != null) {
            holder = tvSurfaceView.getHolder();
        }
        android.graphics.Canvas canvas = null;
        try {
            canvas = holder.lockCanvas();
            if (canvas != null) {
                canvas.drawColor(android.graphics.Color.BLACK);

                android.graphics.Paint paint = new android.graphics.Paint();
                paint.setColor(android.graphics.Color.WHITE);
                paint.setTextSize(60);
                paint.setAntiAlias(true);
                paint.setTextAlign(android.graphics.Paint.Align.CENTER);

                int centerX = canvas.getWidth() / 2;
                int y = canvas.getHeight() / 2 - 150;

                canvas.drawText("Waiting for X11 streamer...", centerX, y, paint);
                y += 80;
                paint.setTextSize(48);
                canvas.drawText("IP: " + ip, centerX, y, paint);
                y += 60;
                canvas.drawText("Port: " + port, centerX, y, paint);
                y += 60;
                paint.setTextSize(72);
                paint.setColor(android.graphics.Color.YELLOW);
                paint.setFakeBoldText(true);
                canvas.drawText("PIN: " + String.format("%04d", pin), centerX, y, paint);

                holder.unlockCanvasAndPost(canvas);
            }
        } catch (Exception e) {
            e.printStackTrace();
            if (canvas != null) {
                try {
                    holder.unlockCanvasAndPost(canvas);
                } catch (Exception e2) {
                    e2.printStackTrace();
                }
            }
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // Stop listening only when activity is actually being destroyed
        stopListening();
        if (displayManager != null && displayListener != null) {
            displayManager.unregisterDisplayListener(displayListener);
        }
        if (tvPresentation != null) {
            tvPresentation.dismiss();
            tvPresentation = null;
        }
        stopListening();
    }

    private boolean isTvConnected() {
        if (displayManager == null) {
            return false;
        }
        android.view.Display defaultDisplay = getWindowManager().getDefaultDisplay();
        int defaultDisplayId = defaultDisplay.getDisplayId();

        android.view.Display[] displays = displayManager.getDisplays();

        // Check if there are any displays other than the default (phone) display
        for (android.view.Display display : displays) {
            if (display.getDisplayId() != defaultDisplayId) {
                // Check if it's a presentation/external display
                if ((display.getFlags() & android.view.Display.FLAG_PRESENTATION) != 0) {
                    return true;
                }
                // Also check if it's HDMI or other external connection
                // (HDMI displays typically have different characteristics)
                android.graphics.Point size = new android.graphics.Point();
                display.getRealSize(size);
                // If it's a different size or has presentation flag, it's likely external
                android.graphics.Point defaultSize = new android.graphics.Point();
                defaultDisplay.getRealSize(defaultSize);
                if (!size.equals(defaultSize)) {
                    return true;
                }
            }
        }
        return false;
    }

    private android.view.Display getTargetDisplay() {
        if (displayManager == null) {
            return getWindowManager().getDefaultDisplay();
        }

        android.view.Display defaultDisplay = getWindowManager().getDefaultDisplay();
        int defaultDisplayId = defaultDisplay.getDisplayId();

        android.view.Display[] displays = displayManager.getDisplays();

        // First, try to find an external/TV display
        for (android.view.Display display : displays) {
            if (display.getDisplayId() != defaultDisplayId) {
                // Check if it's a presentation/external display
                if ((display.getFlags() & android.view.Display.FLAG_PRESENTATION) != 0) {
                    return display;
                }
                // Also check if it's HDMI or other external connection
                android.graphics.Point size = new android.graphics.Point();
                display.getRealSize(size);
                android.graphics.Point defaultSize = new android.graphics.Point();
                defaultDisplay.getRealSize(defaultSize);
                if (!size.equals(defaultSize)) {
                    return display;
                }
            }
        }

        // If no external display, use the default (phone) display
        return defaultDisplay;
    }

    private void updateDisplayVisibility() {
        boolean tvConnected = isTvConnected();
        android.view.Display tvDisplay = getTargetDisplay();

        if (tvConnected && tvDisplay != null && tvDisplay.getDisplayId() != getWindowManager().getDefaultDisplay().getDisplayId()) {
            // TV is connected - create Presentation on TV display
            if (tvPresentation == null) {
                tvPresentation = new TvPresentation(this, tvDisplay);
                tvPresentation.show();
                tvSurfaceView = tvPresentation.findViewById(R.id.tvSurfaceView);
            }
            // Hide phone SurfaceView
            surfaceView.setVisibility(android.view.View.GONE);
        } else {
            // No TV - dismiss Presentation and show phone SurfaceView
            if (tvPresentation != null) {
                tvPresentation.dismiss();
                tvPresentation = null;
                tvSurfaceView = null;
            }
            surfaceView.setVisibility(android.view.View.VISIBLE);
        }
    }

    // Presentation class for TV display
    private static class TvPresentation extends android.app.Presentation {
        public TvPresentation(android.content.Context context, android.view.Display display) {
            super(context, display);
        }

        @Override
        protected void onCreate(android.os.Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            setContentView(R.layout.presentation_tv);
        }
    }
}

