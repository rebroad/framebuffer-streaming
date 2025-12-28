# X11 Framebuffer Streaming - Implementation Plan

## Overview
Stream X11 framebuffer content from all outputs (including standard Xorg outputs and virtual XR outputs) to remote clients (Android, Raspberry Pi, etc.) for remote display.

## Architecture

### Server Side (x11-framebuffer-server)
- Monitor all X11 outputs via RandR (both physical and virtual XR outputs)
- Capture framebuffer data from DRM framebuffer IDs
- Capture audio from X11/PulseAudio/ALSA
- Support multiple concurrent outputs
- Encode/compress framebuffer data (initially raw, later add compression)
- Stream video and audio over TCP/IP to multiple client types
- Handle multiple concurrent clients
- Forward input events from clients back to X server (Android only)

### Client Side
Multiple client implementations:
- **Android Client** (android-framebuffer-client)
  - Connect to server via TCP/IP
  - Receive framebuffer data
  - Decode and display on Android Surface/TextureView
  - Receive audio stream and output to connected TV/display
  - Capture touch/input events and send to server
  - Handle reconnection and error recovery
- **Raspberry Pi Client** (future)
  - Connect to server via TCP/IP
  - Receive framebuffer data
  - Display using DRM/KMS or framebuffer device
  - Receive audio stream and output to connected TV/display via HDMI/audio output
  - Display-only (no input handling)

## Implementation Phases

### Phase 1: Basic Server Infrastructure
1. Set up build system (CMake/Meson)
2. X11/RandR integration to query all outputs (physical and virtual)
3. DRM integration to read framebuffer data from all outputs
4. Support selecting which output(s) to stream
5. Basic TCP server for client connections
6. Simple protocol: width, height, format, raw pixel data

### Phase 2: Basic Android Client
1. Android project setup (Kotlin/Java)
2. TCP client connection
3. Receive and parse protocol messages
4. Display framebuffer on SurfaceView/TextureView
5. Basic touch input capture and forwarding
6. Audio streaming support (receive and output to TV/display)

### Phase 3: Optimization
1. Add frame rate limiting/throttling
2. Implement dirty rectangle updates (only send changed regions)
3. Add compression (H.264, VP8, or custom for video)
4. Add audio compression (AAC, Opus, etc.)
5. Handle resolution changes dynamically
6. Synchronize audio/video streams
7. Add connection status indicators

### Phase 4: Polish
1. Error handling and recovery
2. Authentication/security
3. Multiple output support
4. Performance monitoring
5. Configuration options

## Technical Details

### Framebuffer Access
- Query all outputs via RandR (RRGetOutputs)
- **For all outputs (virtual XR and standard Xorg) - preferred method**:
  - Query `FRAMEBUFFER_ID` property from RandR output (now available on all outputs)
  - Find DRM device containing that framebuffer ID
  - Use `drmModeGetFB()` → `drmPrimeHandleToFD()` to export DMA-BUF FD (zero-copy, proven approach)
  - This unified approach works for both virtual and standard outputs
- **Alternative method (if FRAMEBUFFER_ID not available)**:
  - Query `PIXMAP_ID` property (virtual outputs only) and use DRI3 `DRI3BufferFromPixmap` protocol
  - Or use root window pixmap via X11 extensions (XShm, XFixes)
- Use DMA-BUF FDs for zero-copy access (preferred method)
- Fall back to CPU mapping via `drmModeMapDumb()` only for dumb buffers (non-GBM path)
- Handle format conversion if needed

### Protocol Design
```
Message Types:
- HELLO: Client/server handshake
- FRAME: Framebuffer data (width, height, format, data)
- AUDIO: Audio data (format, sample rate, channels, data)
- INPUT: Input event from client (touch, key, etc.) - Android only
- CONFIG: Configuration changes (resolution, audio format, etc.)
- PING/PONG: Keepalive
```

### Client Display
- **Android**: Use SurfaceView or TextureView for rendering, consider OpenGL ES
- **Raspberry Pi**: Use DRM/KMS or framebuffer device (/dev/fb0)
- Handle orientation/resolution changes
- Support different screen densities/resolutions

## Dependencies

### Server
- X11 libraries (libX11, libXrandr)
- DRM libraries (libdrm)
- Glamor (for DMA-BUF export via `glamor_fd_from_pixmap()`)
- Audio capture (PulseAudio, ALSA, or X11 audio)
- Networking (standard sockets)

### Android Client
- Android SDK
- Networking (java.net or OkHttp)
- Graphics (SurfaceView/TextureView, possibly OpenGL ES)
- Audio playback (AudioTrack or MediaPlayer for TV output)

### Raspberry Pi Client (future)
- Linux system libraries
- DRM/KMS libraries (libdrm)
- Networking (standard sockets)
- Audio output (ALSA, PulseAudio for HDMI/audio output)

