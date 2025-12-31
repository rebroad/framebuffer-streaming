package com.framebuffer.client;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkRequest;
import java.io.IOException;
import java.net.Socket;

public class MainActivity extends AppCompatActivity implements SurfaceHolder.Callback {
    private SurfaceView surfaceView;

    private java.net.ServerSocket serverSocket;
    private Socket clientSocket;
    private FrameReceiver frameReceiver;
    private boolean listening = false;
    private String currentDisplayIp = null;  // Store current IP for redrawing
    private int currentDisplayPort = 4321;     // Store current port for redrawing
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
    private ConnectivityManager connectivityManager;
    private ConnectivityManager.NetworkCallback networkCallback;
    private Handler continuousIpUpdateHandler;
    private Runnable continuousIpUpdateRunnable;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        android.util.Log.i("MainActivity", "onCreate() called");
        setContentView(R.layout.activity_main);

        // Keep screen on while TV receiver is running
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        surfaceView = findViewById(R.id.surfaceView);

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

        // Set up network change listener to update IP display when interfaces change
        connectivityManager = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        networkCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                android.util.Log.i("MainActivity", "Network available: " + network);
                // Update immediately - continuous polling will catch any delayed changes
                updateIpDisplay();
            }

            @Override
            public void onLost(Network network) {
                android.util.Log.i("MainActivity", "Network lost: " + network);
                // Update immediately - continuous polling will catch any delayed changes
                updateIpDisplay();
            }

            @Override
            public void onCapabilitiesChanged(Network network, android.net.NetworkCapabilities networkCapabilities) {
                android.util.Log.i("MainActivity", "Network capabilities changed: " + network);
                updateIpDisplay();
            }

            @Override
            public void onLinkPropertiesChanged(Network network, android.net.LinkProperties linkProperties) {
                android.util.Log.i("MainActivity", "Network link properties changed: " + network);
                // This fires when IP addresses are assigned or removed, so update immediately
                updateIpDisplay();
            }
        };
        NetworkRequest request = new NetworkRequest.Builder().build();
        connectivityManager.registerNetworkCallback(request, networkCallback);

        // Start continuous IP polling to catch USB tethering changes
        // USB tethering interfaces may not always trigger NetworkCallback properly
        startContinuousIpUpdate();

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
            // Tell streamer to resume sending frames now that we can render them
            sendResumeMessage();
        } else if (listening && currentDisplayIp != null) {
            // We're listening but not connected - redraw connection info
            android.util.Log.i("MainActivity", "Surface recreated while listening - redrawing connection info");
            displayConnectionInfoOnTV(currentDisplayPort, currentDisplayIp, pinCode);
        }
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

                // Display connection info on SurfaceView (yellow PIN display)
                // Show up to 3 IPs
                java.util.List<String> displayIps = new java.util.ArrayList<>();
                for (int i = 0; i < Math.min(3, localIps.size()); i++) {
                    displayIps.add(localIps.get(i));
                }
                final String displayIpText = String.join(", ", displayIps);
                // Store for redrawing when surface is recreated
                currentDisplayIp = displayIpText;
                currentDisplayPort = port;
                new Handler(Looper.getMainLooper()).post(() -> {
                    displayConnectionInfoOnTV(port, displayIpText, pinCode);
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

                        // Determine if PIN verification is required based on network interface
                        android.util.Log.i("MainActivity", "=== CONNECTION HANDLING ===");
                        android.util.Log.i("MainActivity", "Local address: " + acceptedSocket.getLocalAddress());
                        boolean requiresPin = shouldRequirePin(acceptedSocket);
                        android.util.Log.i("MainActivity", "Connection from interface requiring PIN: " + requiresPin);

                        // Send capabilities message IMMEDIATELY to tell streamer if encryption is needed
                        // This must be sent before streamer tries to do Noise handshake
                        java.io.OutputStream out = acceptedSocket.getOutputStream();
                        byte[] capabilitiesPayload = new byte[4];
                        capabilitiesPayload[0] = (byte)(requiresPin ? 1 : 0);  // requires_encryption
                        capabilitiesPayload[1] = 0;  // reserved
                        capabilitiesPayload[2] = 0;  // reserved
                        capabilitiesPayload[3] = 0;  // reserved
                        Protocol.sendMessage(out, Protocol.MSG_CAPABILITIES, capabilitiesPayload);
                        android.util.Log.i("MainActivity", "Sent CAPABILITIES message: requires_encryption=" + requiresPin);

                        // Perform Noise Protocol handshake FIRST (before any sensitive data exchange)
                        // Only if PIN is required (encryption enabled)
                        NoiseEncryption noiseEncryption = null;
                        if (requiresPin) {
                            android.util.Log.i("MainActivity", "Starting Noise Protocol handshake (PIN required)");
                            noiseEncryption = new NoiseEncryption(false);  // Receiver is responder
                            currentNoiseEncryption = noiseEncryption;  // Store for use in verifyPinFromClient
                            try {
                                android.util.Log.d("MainActivity", "Calling noiseEncryption.handshake()...");
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
                            android.util.Log.i("MainActivity", "Waiting for PIN verification from client...");
                            if (!verifyPinFromClient(acceptedSocket, noiseEncryption)) {
                                android.util.Log.w("MainActivity", "PIN verification failed, closing connection");
                                noiseEncryption.cleanup();
                                currentNoiseEncryption = null;
                                acceptedSocket.close();
                                continue; // Continue listening for next connection
                            }
                            android.util.Log.i("MainActivity", "PIN verification successful");
                        } else {
                            android.util.Log.i("MainActivity", "Skipping Noise handshake and PIN verification (trusted interface: USB tethering)");
                            currentNoiseEncryption = null;  // No encryption for USB tethering
                        }

                        new Handler(Looper.getMainLooper()).post(() -> {
                            // Connection status is shown on SurfaceView, no need for TextView
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
                        // Use getMode() for physical dimensions (modern API, available from API 23+)
                        android.view.Display.Mode mode = targetDisplay.getMode();
                        size.x = mode.getPhysicalWidth();
                        size.y = mode.getPhysicalHeight();

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

                        // Send HELLO with display name and modes
                        // Use encryption if PIN was required (and Noise handshake succeeded)
                        android.util.Log.i("MainActivity", "Sending HELLO message...");
                        android.util.Log.d("MainActivity", "noiseEncryption: " + noiseEncryption + ", isReady: " + (noiseEncryption != null ? noiseEncryption.isReady() : "N/A"));
                        if (noiseEncryption != null && noiseEncryption.isReady()) {
                            android.util.Log.i("MainActivity", "Sending HELLO encrypted");
                            // Build HELLO message
                            java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream();
                            Protocol.sendHello(baos, displayName, modes);
                            noiseEncryption.send(acceptedSocket, baos.toByteArray());
                        } else {
                            android.util.Log.i("MainActivity", "Sending HELLO unencrypted");
                            // Unencrypted (for trusted interfaces: USB tethering)
                            Protocol.sendHello(acceptedSocket.getOutputStream(), displayName, modes);
                        }
                        android.util.Log.i("MainActivity", "HELLO message sent");

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
                        // Show up to 3 IPs
                        java.util.List<String> displayIpsRestore = new java.util.ArrayList<>();
                        for (int i = 0; i < Math.min(3, localIps.size()); i++) {
                            displayIpsRestore.add(localIps.get(i));
                        }
                        final String displayIpTextRestore = String.join(", ", displayIpsRestore);
                        new Handler(Looper.getMainLooper()).post(() -> {
                            displayConnectionInfoOnTV(port, displayIpTextRestore, pinCode);
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
                    // Error shown via Toast, SurfaceView shows connection info
                });
            } catch (IOException e) {
                android.util.Log.e("MainActivity", "Server socket error", e);
                new Handler(Looper.getMainLooper()).post(() -> {
                    if (listening) {
                        Toast.makeText(MainActivity.this, "Server error: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                        // Error will be shown via Toast, SurfaceView shows connection info
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
        if (!listening) {
            android.util.Log.d("MainActivity", "stopListening() called but already stopped");
            return;  // Already stopped
        }
        android.util.Log.i("MainActivity", "stopListening() called - setting listening=false");
        // Log stack trace to see who called stopListening
        android.util.Log.i("MainActivity", "stopListening() call stack:", new Exception("Stack trace"));
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
                // Bind to IPv4 address (0.0.0.0) to receive IPv4 broadcasts
                udpSocket = new java.net.DatagramSocket();
                udpSocket.bind(new java.net.InetSocketAddress("0.0.0.0", 4321));  // Default broadcast port
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

    /**
     * Determine if PIN verification is required based on the network interface.
     * Only USB tethering (rndis0) is trusted and doesn't require PIN.
     * All other interfaces (wlan0, swlan0, etc.) require PIN verification for security.
     */
    private boolean shouldRequirePin(Socket socket) {
        try {
            java.net.InetAddress localAddr = socket.getLocalAddress();
            if (localAddr == null) {
                // Can't determine interface, require PIN for security
                return true;
            }

            // Get all network interfaces and find which one this address belongs to
            java.util.Enumeration<java.net.NetworkInterface> interfaces =
                java.net.NetworkInterface.getNetworkInterfaces();
            while (interfaces.hasMoreElements()) {
                java.net.NetworkInterface iface = interfaces.nextElement();
                java.util.Enumeration<java.net.InetAddress> addresses = iface.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    java.net.InetAddress addr = addresses.nextElement();
                    if (addr.equals(localAddr)) {
                        String ifaceName = iface.getName();
                        android.util.Log.i("MainActivity", "Connection from interface: " + ifaceName);

                        // USB tethering interface (rndis0) - trusted, no PIN required
                        if (ifaceName.equals("rndis0")) {
                            android.util.Log.i("MainActivity", "USB tethering detected - PIN not required");
                            return false;
                        }

                        // All other interfaces (wlan0, swlan0, etc.) require PIN
                        android.util.Log.i("MainActivity", "Interface " + ifaceName + " requires PIN authentication");
                        return true;
                    }
                }
            }
        } catch (Exception e) {
            android.util.Log.e("MainActivity", "Error determining network interface", e);
        }

        // Default to requiring PIN for security if we can't determine the interface
        return true;
    }

    // Check if an interface name indicates mobile data (should be filtered out)
    private boolean isMobileDataInterface(String ifaceName) {
        if (ifaceName == null) return false;
        String name = ifaceName.toLowerCase();
        // Common mobile data interface prefixes
        return name.startsWith("rmnet") ||
               name.startsWith("ccmni") ||
               name.startsWith("pdp") ||
               name.startsWith("wwan") ||
               name.startsWith("cdma") ||
               name.startsWith("umts") ||
               name.startsWith("lte") ||
               name.startsWith("gsm") ||
               name.contains("mobile");
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

                // Skip mobile data interfaces
                String ifaceName = iface.getName();
                if (isMobileDataInterface(ifaceName)) {
                    android.util.Log.d("MainActivity", "Skipping mobile data interface: " + ifaceName);
                    continue;
                }

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

    // Start continuous IP polling (runs every 2 seconds while listening)
    // This catches USB tethering changes that NetworkCallback might miss
    private void startContinuousIpUpdate() {
        if (continuousIpUpdateHandler == null) {
            continuousIpUpdateHandler = new Handler(Looper.getMainLooper());
        }

        // Stop any existing continuous updates
        stopContinuousIpUpdate();

        continuousIpUpdateRunnable = new Runnable() {
            @Override
            public void run() {
                if (listening) {
                    updateIpDisplay();
                    // Poll every 2 seconds continuously
                    continuousIpUpdateHandler.postDelayed(this, 2000);
                }
            }
        };

        // Start polling after a short delay
        continuousIpUpdateHandler.postDelayed(continuousIpUpdateRunnable, 2000);
    }

    // Stop continuous IP updates
    private void stopContinuousIpUpdate() {
        if (continuousIpUpdateHandler != null && continuousIpUpdateRunnable != null) {
            continuousIpUpdateHandler.removeCallbacks(continuousIpUpdateRunnable);
            continuousIpUpdateRunnable = null;
        }
    }

    // Update IP display when network interfaces change
    private void updateIpDisplay() {
        if (!listening) return;

        new Handler(Looper.getMainLooper()).post(() -> {
            // Get updated IP addresses
            java.util.List<String> localIps = getAllLocalIpAddresses();
            android.util.Log.i("MainActivity", "Network changed - Updated IP addresses: " + String.join(", ", localIps));

            // Update display with up to 3 IPs
            java.util.List<String> displayIps = new java.util.ArrayList<>();
            for (int i = 0; i < Math.min(3, localIps.size()); i++) {
                displayIps.add(localIps.get(i));
            }
            String displayIpText = String.join(", ", displayIps);

            // Store for redrawing when surface is recreated
            currentDisplayIp = displayIpText;

            // Update the display if we have a surface
            if (surfaceView != null && surfaceView.getHolder() != null) {
                displayConnectionInfoOnTV(currentDisplayPort, displayIpText, pinCode);
            }
            if (tvSurfaceView != null && tvSurfaceView.getHolder() != null) {
                displayConnectionInfoOnTV(currentDisplayPort, displayIpText, pinCode);
            }
        });
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
                y += 90;  // Increased gap between port and PIN
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
        // Stop continuous IP updates
        stopContinuousIpUpdate();

        // Unregister network callback
        if (connectivityManager != null && networkCallback != null) {
            connectivityManager.unregisterNetworkCallback(networkCallback);
        }

        super.onDestroy();
        // Stop listening only when activity is actually being destroyed
        android.util.Log.i("MainActivity", "onDestroy() called - stopping listening");
        // Clear screen wake lock
        getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        stopListening();
        if (displayManager != null && displayListener != null) {
            displayManager.unregisterDisplayListener(displayListener);
        }
        if (tvPresentation != null) {
            tvPresentation.dismiss();
            tvPresentation = null;
        }
    }

    private boolean isTvConnected() {
        if (displayManager == null) {
            return false;
        }
        android.view.Display defaultDisplay = displayManager.getDisplay(android.view.Display.DEFAULT_DISPLAY);
        if (defaultDisplay == null) {
            return false;
        }
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
                // Use getMode() for physical dimensions (modern API, available from API 23+)
                android.view.Display.Mode mode = display.getMode();
                size.x = mode.getPhysicalWidth();
                size.y = mode.getPhysicalHeight();
                // If it's a different size or has presentation flag, it's likely external
                android.graphics.Point defaultSize = new android.graphics.Point();
                android.view.Display.Mode defaultMode = defaultDisplay.getMode();
                defaultSize.x = defaultMode.getPhysicalWidth();
                defaultSize.y = defaultMode.getPhysicalHeight();
                if (!size.equals(defaultSize)) {
                    return true;
                }
            }
        }
        return false;
    }

    private android.view.Display getTargetDisplay() {
        if (displayManager == null) {
            // Fallback if displayManager is not available - use Context.getDisplay() (API 30+)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                android.view.Display display = getDisplay();
                if (display != null) {
                    return display;
                }
            }
            // Last resort: use deprecated API (should not happen in practice)
            @SuppressWarnings("deprecation")
            android.view.Display fallbackDisplay = getWindowManager().getDefaultDisplay();
            return fallbackDisplay;
        }

        android.view.Display defaultDisplay = displayManager.getDisplay(android.view.Display.DEFAULT_DISPLAY);
        if (defaultDisplay == null) {
            return null;
        }
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
                // Use getMode() for physical dimensions (modern API, available from API 23+)
                android.view.Display.Mode mode = display.getMode();
                size.x = mode.getPhysicalWidth();
                size.y = mode.getPhysicalHeight();
                android.graphics.Point defaultSize = new android.graphics.Point();
                android.view.Display.Mode defaultMode = defaultDisplay.getMode();
                defaultSize.x = defaultMode.getPhysicalWidth();
                defaultSize.y = defaultMode.getPhysicalHeight();
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

        android.view.Display defaultDisplay = displayManager != null ?
            displayManager.getDisplay(android.view.Display.DEFAULT_DISPLAY) : null;
        if (defaultDisplay == null) {
            // Fallback if DisplayManager fails - use Context.getDisplay() (API 30+)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                defaultDisplay = getDisplay();
            }
            if (defaultDisplay == null) {
                // Last resort: use deprecated API (should not happen in practice)
                @SuppressWarnings("deprecation")
                android.view.Display fallbackDisplay = getWindowManager().getDefaultDisplay();
                defaultDisplay = fallbackDisplay;
            }
        }
        if (tvConnected && tvDisplay != null && defaultDisplay != null && tvDisplay.getDisplayId() != defaultDisplay.getDisplayId()) {
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

