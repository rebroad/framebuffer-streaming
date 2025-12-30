# Framebuffer Streaming

Stream X11 framebuffer content from all outputs (including standard Xorg outputs and virtual XR outputs) to remote clients (Android, Raspberry Pi, etc.) for remote display.

Virtual displays created by clients behave exactly like physical displays - you can use `xrandr` to change resolution, position, turn them on/off, etc., and the changes are automatically forwarded to the client.

## Project Structure

- `server/` - Linux X11 framebuffer server (C/C++)
- `client/` - Android client application (Java/Kotlin)
- `PLAN.md` - Detailed implementation plan
- `HLD.md` - High-Level Design document (architecture and design overview)
- `USB_TETHERING_ADB.md` - Guide for enabling USB tethering on Android devices via ADB

## Server

The server component runs on a Linux system with X11 and captures framebuffers from all X11 outputs using:
- RandR to query outputs and read `FRAMEBUFFER_ID` properties
- Direct DRM access via `drmModeGetFB()` → `drmPrimeHandleToFD()` for DMA-BUF export
- TCP server for streaming to clients

See `server/README.md` for build and usage instructions.

## Client

The Android client connects to the server and displays the streamed framebuffer content.

See `client/README.md` for build and usage instructions.

## Features

- ✅ Stream all X11 framebuffers (standard and virtual XR outputs)
- ✅ Zero-copy DMA-BUF support (when available)
- ✅ Multi-client support
- ✅ Virtual displays behave like physical displays (full xrandr support)
- ✅ Automatic configuration change detection and forwarding
- ✅ Display name preservation (uses client's actual display name)
- 🔄 Audio streaming (planned)
- 🔄 Input forwarding from Android clients (planned)
- 🔄 Compression (video/audio codecs)
- 🔄 Dirty rectangle updates

## How It Works

1. **Client connects** and sends its display capabilities (name, resolution, refresh rate)
2. **Server creates virtual output** with the client's display name
3. **Server captures frames** from the virtual framebuffer and streams to client
4. **When you use xrandr** to change resolution or turn display on/off:
   - Server detects the change via RandR events
   - Server sends CONFIG message to client
   - Client updates display accordingly (shows "NO SIGNAL" when off)

See `HLD.md` for detailed architecture and design documentation.

## Quick Start

### Server

```bash
cd server
mkdir build && cd build
cmake ..
make
./x11-fb-server  # Runs on port 8888 by default
```

### Android Client

```bash
cd client
./gradlew installDebug  # Install to connected device
```

1. Open the app on your Android device
2. Enter server IP address and port (default: 8888)
3. Tap "Connect"
4. Your Android display will appear as a virtual output on the X server
5. Use `xrandr` to configure it like any physical display

### Example: Using xrandr

```bash
# List all outputs (including virtual ones)
xrandr

# Change resolution
xrandr --output "Samsung Electric Company 55\"" --mode 1920x1080

# Turn display off (client shows "NO SIGNAL")
xrandr --output "Samsung Electric Company 55\"" --off

# Turn display back on
xrandr --output "Samsung Electric Company 55\"" --auto
```

## Building

### Server

```bash
cd server
mkdir build && cd build
cmake ..
make
```

### Client

```bash
cd client
./gradlew build
```

## License

[To be determined]

