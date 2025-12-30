# Noise Protocol Framework Implementation Status

## Overview

The Noise Protocol Framework integration structure has been created for both the C streamer and Java Android receiver. The framework is in place, but the actual Noise Protocol library integration needs to be completed.

## Completed

1. **C Side (x11-streamer)**:
   - Created `noise_encryption.h` and `noise_encryption.c` with encryption wrapper functions
   - Added `noise_encryption_context_t` to `x11_streamer` struct
   - Integrated Noise handshake after PIN verification
   - Created encrypted versions of `protocol_send_message` and `protocol_receive_message`
   - Added conditional encryption in all protocol message send/receive locations
   - Added cleanup in `x11_streamer_destroy`

2. **Java Side (tv-receiver)**:
   - Created `NoiseEncryption.java` class with encryption wrapper
   - Integrated Noise handshake after PIN verification in `MainActivity`
   - Structure ready for integration with FrameReceiver

3. **Documentation**:
   - Created `NOISE_PROTOCOL_INTEGRATION.md` with detailed integration plan
   - Documented library options and implementation strategy

## Pending

1. **Library Integration**:
   - **C Side**: Need to integrate a Noise Protocol library:
	 - Option 1: `snow` (https://github.com/mcginty/snow) - Modern, recommended
	 - Option 2: `noise-c` (https://github.com/rweather/noise-c) - Official reference
	 - Option 3: libsodium with Noise support (if available)
   - **Java Side**: Need to integrate a Noise Protocol library:
	 - Option 1: `noise-java` (https://github.com/rweather/noise-java) - Official Java implementation
	 - Option 2: JNI wrapper around C library

2. **Complete Implementation**:
   - Replace placeholder implementations in `noise_encryption.c` with actual Noise Protocol calls
   - Replace placeholder implementations in `NoiseEncryption.java` with actual Noise Protocol calls
   - Implement proper handshake pattern (recommended: `Noise_NK_25519_ChaChaPoly_SHA256`)
   - Handle key generation and storage (receiver static key, streamer ephemeral key)

3. **FrameReceiver Integration**:
   - Pass `NoiseEncryption` object from `MainActivity` to `FrameReceiver`
   - Modify `Protocol.java` to support encrypted send/receive
   - Update `FrameReceiver` to use encryption for all protocol messages

4. **Testing**:
   - Test encrypted communication end-to-end
   - Verify that traffic cannot be intercepted
   - Test backward compatibility (if implemented)

## Next Steps

1. **Choose and integrate Noise Protocol library for C**:
   ```bash
   cd /home/rebroad/src/framebuffer-streaming/x11-streamer
   git submodule add https://github.com/mcginty/snow.git third_party/snow
   # Or download and include source files
   ```

2. **Update CMakeLists.txt** to build and link the Noise library

3. **Complete `noise_encryption.c`** implementation using the chosen library

4. **Choose and integrate Noise Protocol library for Java**:
   - Add to `build.gradle` dependencies, or
   - Include as AAR/library module, or
   - Use JNI wrapper

5. **Complete `NoiseEncryption.java`** implementation

6. **Integrate encryption into FrameReceiver**:
   - Store `NoiseEncryption` in `MainActivity` after handshake
   - Pass to `FrameReceiver` constructor
   - Modify `Protocol.java` methods to use encryption
   - Update `FrameReceiver` to use encrypted protocol methods

7. **Test and verify** encrypted communication

## Current State

The code currently has placeholder implementations that:
- Mark handshake as complete immediately (for testing structure)
- Send/receive unencrypted data (will be replaced with encrypted versions)

**WARNING**: The current implementation does NOT provide encryption. It only has the structure in place. To enable actual encryption, the Noise Protocol libraries must be integrated and the placeholder code replaced.

## Files Modified

- `x11-streamer/include/noise_encryption.h` - New header
- `x11-streamer/src/noise_encryption.c` - New implementation (placeholders)
- `x11-streamer/include/protocol.h` - Added encrypted function declarations
- `x11-streamer/src/protocol.c` - Added encrypted function implementations
- `x11-streamer/src/x11_streamer.c` - Integrated Noise handshake and encryption
- `x11-streamer/CMakeLists.txt` - Added noise_encryption.c to sources
- `tv-receiver/app/src/main/java/com/framebuffer/client/NoiseEncryption.java` - New class (placeholders)
- `tv-receiver/app/src/main/java/com/framebuffer/client/MainActivity.java` - Integrated Noise handshake

## References

- Noise Protocol Framework: https://noiseprotocol.org/
- snow library: https://github.com/mcginty/snow
- noise-java: https://github.com/rweather/noise-java
- noise-c: https://github.com/rweather/noise-c

