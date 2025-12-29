# High-Level Design (HLD)

## Overview

This document describes the high-level architecture and design of the framebuffer streaming system. The system enables remote display of X11 framebuffers to Android and Raspberry Pi clients, making virtual displays appear as if they are directly connected to the X server.

## Architecture

```
┌─────────────┐
│ X Server    │
│─────────────│
│ Std Output  │
│ (FB_ID)     │
│             │
│ Virtual XR  │
│ (FB_ID)     │
└────┬───┬────┘
     │   │
     └───┴────► Framebuffer Streaming Server
                • RandR/DRM/DRI3 access
                • DMA-BUF, Memory Map, TCP
                     │
                     │ TCP/IP
                     │
        ┌────────────┴─────────────┐
        │                          │
┌───────▼────────┐        ┌────────▼────────┐
│ Android Client │        │ Raspberry Pi    │
│                │        │ Client          │
│ • Display      │        │ • Display only  │
│ • Input        │        │ • Audio         │
│ • Audio        │        │                 │
└────────────────┘        └─────────────────┘
```

## Core Components

### 1. X Server Integration

The system integrates with the X server through:

- **RandR Extension**: Used to query outputs, monitor changes, and manage virtual displays
- **Custom RandR Properties**:
  - `FRAMEBUFFER_ID`: Exposed on all outputs (standard and virtual) to provide the DRM framebuffer ID
- **Virtual Output Creation**: Clients can request creation of virtual outputs with specific resolutions and refresh rates

### 2. Framebuffer Access Methods

The server supports multiple methods for accessing framebuffer data:

#### Method 1: Direct DRM Access (Primary Method)
- Uses `FRAMEBUFFER_ID` property to get the DRM framebuffer ID (works for all outputs)
- Calls `drmModeGetFB()` to get framebuffer information
- Uses `drmPrimeHandleToFD()` to export as DMA-BUF file descriptor
- Zero-copy transfer to clients via `SCM_RIGHTS`
- Unified approach that works for both standard and virtual outputs

#### Method 2: Memory Mapping (Fallback)
- Maps framebuffer memory directly
- Copies pixel data over TCP
- Used when DMA-BUF is not available

### 3. Server Components

#### 3.1 X11 Output Management (`x11_output.c`)
- **Purpose**: Interface with X11/RandR to discover and monitor outputs
- **Key Functions**:
  - `x11_context_create()`: Initialize X11 connection and RandR extension
  - `x11_context_refresh_outputs()`: Query all connected outputs and their `FRAMEBUFFER_ID` properties
  - `x11_context_create_virtual_output()`: Create virtual outputs based on client capabilities
  - `x11_context_process_events()`: Monitor RandR events for configuration changes
- **Event Monitoring**: Uses `XRRSelectInput()` to receive notifications for:
  - Output property changes (resolution, refresh rate)
  - Connection status changes (on/off)
  - CRTC changes (mode sets)

#### 3.2 DRM Framebuffer Access (`drm_fb.c`)
- **Purpose**: Access DRM framebuffers and export as DMA-BUF
- **Key Functions**:
  - `drm_fb_open()`: Open a DRM framebuffer by ID
  - `drm_fb_export_dma_buf()`: Export framebuffer as DMA-BUF file descriptor
  - `drm_fb_map()`: Map framebuffer to memory (fallback)
- **Device Discovery**: Automatically finds the correct DRM device

#### 3.3 Protocol (`protocol.c`)
- **Purpose**: Network message serialization/deserialization
- **Message Types**:
  - `MSG_HELLO`: Client handshake with display capabilities
  - `MSG_FRAME`: Framebuffer frame data
  - `MSG_CONFIG`: Output configuration changes (resolution, refresh rate, on/off)
  - `MSG_INPUT`: Input events from client (planned)
  - `MSG_AUDIO`: Audio data (planned)
  - `MSG_PING/PONG`: Keepalive

#### 3.4 Server Core (`server.c`)
- **Purpose**: Main server loop managing clients and frame capture
- **Key Features**:
  - TCP server accepting client connections
  - Per-client threads for message handling
  - Periodic frame capture and broadcasting
  - RandR event processing and change detection
  - CONFIG message forwarding when output properties change

### 4. Client Components

#### 4.1 Android Client

**MainActivity.java**:
- UI for server connection (address, port)
- SurfaceView for displaying frames
- Connection management

**FrameReceiver.java**:
- Network message reception loop
- Frame rendering to SurfaceView
- CONFIG message handling:
  - Resolution changes
  - Connection status (on/off → "NO SIGNAL" screen)
- State management (connected/disconnected)

**Protocol.java**:
- Message serialization/deserialization
- HELLO message construction with display capabilities
- CONFIG message parsing

## Data Flow

### Connection Establishment

1. **Client connects** to server via TCP
2. **Client sends HELLO**:
   - Display name (e.g., "Samsung Electric Company 55"")
   - Supported display modes (resolution, refresh rate)
3. **Server creates virtual output**:
   - Uses display name directly (no "XR-" prefix)
   - Sets resolution and refresh rate from client's preferred mode
   - Creates RandR output with `FRAMEBUFFER_ID` property
4. **Server refreshes outputs** and finds the new virtual output
5. **Connection established** - ready for frame streaming

### Frame Capture and Streaming

1. **Server periodically captures frames** (currently ~1 FPS, can be optimized):
   - For each active virtual output:
     - Gets `FRAMEBUFFER_ID` from RandR property
     - Opens DRM framebuffer using `drmModeGetFB()`
     - Exports as DMA-BUF using `drmPrimeHandleToFD()` (or maps memory as fallback)
     - Sends `MSG_FRAME` header
     - Sends framebuffer data (or DMA-BUF FD via `SCM_RIGHTS`)
2. **Client receives frame**:
   - Parses `MSG_FRAME` header
   - Reads pixel data (or receives DMA-BUF FD)
   - Renders to SurfaceView (scaled to fit)

### Configuration Change Handling

1. **User changes output via xrandr**:
   - Resolution change: `xrandr --output <name> --mode <mode>`
   - Turn off: `xrandr --output <name> --off`
   - Position/orientation: `xrandr --output <name> --pos/--rotate`
2. **X server generates RandR events**:
   - `RROutputChangeNotify` or `RRCrtcChangeNotify`
3. **Server processes events**:
   - `x11_context_process_events()` detects change
   - `x11_context_refresh_outputs()` updates output state
   - `server_check_and_notify_output_changes()` compares old vs new state
4. **Server sends CONFIG message**:
   - Resolution/refresh rate changes → `MSG_CONFIG` with new values
   - Disconnect (off) → `MSG_CONFIG` with width=0, height=0
   - Reconnect (on) → `MSG_CONFIG` with restored resolution
5. **Client receives CONFIG**:
   - Updates internal state
   - Shows "NO SIGNAL" if disconnected
   - Shows toast notification for resolution changes
   - Resumes frame processing when reconnected

## Virtual Display Behavior

The virtual displays created by the system behave **exactly like physical displays**:

- **xrandr commands work identically**:
  - `xrandr --output <name> --mode <mode>` - Change resolution
  - `xrandr --output <name> --off` - Turn off (shows "NO SIGNAL" on client)
  - `xrandr --output <name> --pos <x> <y>` - Position (server-side only)
  - `xrandr --output <name> --rotate <orientation>` - Rotate (server-side only)
  - `xrandr --output <name> --scale <factor>` - Scale (server-side only)
  - Overscan settings - Handled by X server (no client notification needed)

- **Display appears in system settings**:
  - Shows with the exact name provided by the client
  - Appears as a connected display
  - Can be configured like any physical display

## Protocol Details

### HELLO Message Format

```
[Header: 9 bytes]
  - type: MSG_HELLO (0x01)
  - length: variable
  - sequence: 0

[Payload]
  - protocol_version: uint16 (currently 1)
  - num_modes: uint16
  - display_name_len: uint16
  - display_name: string (null-terminated, display_name_len bytes)
  - modes[]: array of display_mode_t (num_modes entries)
    - width: uint32
    - height: uint32
    - refresh_rate: uint32 (Hz * 100)
```

### CONFIG Message Format

```
[Header: 9 bytes]
  - type: MSG_CONFIG (0x05)
  - length: 16
  - sequence: 0

[Payload]
  - output_id: uint32
  - width: uint32 (0 = disconnected)
  - height: uint32 (0 = disconnected)
  - refresh_rate: uint32 (Hz)
```

### FRAME Message Format

```
[Header: 9 bytes]
  - type: MSG_FRAME (0x02)
  - length: 24 + frame_data_size
  - sequence: 0

[Payload]
  - output_id: uint32
  - width: uint32
  - height: uint32
  - format: uint32 (DRM format, e.g., DRM_FORMAT_ARGB8888)
  - pitch: uint32
  - size: uint32 (frame data size in bytes)
  - frame_data: raw pixel data (or DMA-BUF FD via SCM_RIGHTS)
```

## Security Considerations

- **Network**: Currently no encryption/authentication (planned for future)
- **File Descriptors**: DMA-BUF FDs are sent via `SCM_RIGHTS` (Unix domain socket feature)
- **Access Control**: Server listens on all interfaces (0.0.0.0) - consider firewall rules

## Performance Characteristics

- **Frame Rate**: Currently ~1 FPS (limited by polling interval, can be optimized)
- **Latency**: Network-dependent, typically <100ms on LAN
- **CPU Usage**: Low (DMA-BUF zero-copy reduces CPU overhead)
- **Memory**: Minimal (frames streamed directly, not buffered)

## Future Enhancements

- **Input Forwarding**: Android touch/keyboard events → X11 input events
- **Audio Streaming**: Capture and stream audio alongside video
- **Compression**: Video/audio codecs for bandwidth optimization
- **Dirty Rectangle Updates**: Only send changed regions
- **Multiple Outputs**: Support streaming multiple outputs simultaneously
- **Encryption**: TLS/DTLS for secure connections
- **Authentication**: Client authentication and authorization

## Dependencies

### Server
- X11 libraries (libX11, libXrandr)
- DRM libraries (libdrm)
- Glamor (for DMA-BUF export from pixmaps)
- CMake, GCC

### Android Client
- Android SDK (API level 21+)
- Java/Kotlin
- Gradle

