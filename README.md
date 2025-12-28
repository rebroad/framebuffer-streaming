# Framebuffer Streaming

Stream X11 framebuffer content from all outputs (including standard Xorg outputs and virtual XR outputs) to remote clients (Android, Raspberry Pi, etc.) for remote display.

## Project Structure

- `server/` - Linux X11 framebuffer server (C/C++)
- `client/` - Android client application (Java/Kotlin)
- `PLAN.md` - Detailed implementation plan

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

- Stream all X11 framebuffers (standard and virtual XR outputs)
- Zero-copy DMA-BUF support (when available)
- Multi-client support
- Audio streaming (planned)
- Input forwarding from Android clients (planned)

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

