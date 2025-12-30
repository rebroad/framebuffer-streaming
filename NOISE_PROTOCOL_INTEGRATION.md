# Noise Protocol Framework Integration

This document describes the integration of Noise Protocol Framework for encrypted communication between the X11 streamer and Android TV receiver.

## Overview

Noise Protocol Framework provides authenticated encryption for the framebuffer streaming protocol, ensuring that:
- All traffic is encrypted and cannot be viewed by third parties
- Communication is authenticated (prevents man-in-the-middle attacks)
- Forward secrecy is maintained

## Implementation Strategy

### Phase 1: Library Integration

#### C Side (x11-streamer)

We need to integrate a Noise Protocol implementation for C. Recommended options:

1. **snow** (https://github.com/mcginty/snow) - Modern, well-maintained
2. **noise-c** (https://github.com/rweather/noise-c) - Official reference implementation
3. **libsodium with Noise** - If available

**Steps:**
1. Add library as git submodule or include source files
2. Update CMakeLists.txt to build and link the library
3. Create encryption wrapper (`noise_encryption.c`)

#### Java Side (tv-receiver)

We need a Java Noise Protocol implementation:

1. **noise-java** (https://github.com/rweather/noise-java) - Official Java implementation
2. **JNI wrapper** - Wrap C library using JNI

**Steps:**
1. Add dependency to `build.gradle` (if Maven/Gradle available)
2. Or include as AAR/library module
3. Create encryption wrapper class

### Phase 2: Protocol Integration

The encryption layer wraps the existing protocol:

1. **Handshake**: After TCP connection and PIN verification, perform Noise handshake
   - Streamer is initiator
   - Receiver is responder
   - Use Noise pattern: `Noise_NK_25519_ChaChaPoly_SHA256` (no pre-shared key, receiver has static key)

2. **Message Encryption**: All messages after handshake are encrypted
   - Wrap `protocol_send_message()` to encrypt before sending
   - Wrap `protocol_receive_message()` to decrypt after receiving

3. **Backward Compatibility**:
   - Add protocol version negotiation
   - Support unencrypted mode for older clients (optional)

### Phase 3: Key Management

For the initial implementation:
- Receiver generates a static key pair on first run
- Streamer uses ephemeral key (generated per connection)
- Future enhancement: Support for pre-shared keys or certificate-based authentication

## Implementation Details

### C Side Structure

```c
// noise_encryption.h
typedef struct noise_encryption_context noise_encryption_context_t;

noise_encryption_context_t *noise_encryption_init(bool is_initiator);
void noise_encryption_cleanup(noise_encryption_context_t *ctx);
int noise_encryption_handshake(noise_encryption_context_t *ctx, int fd);
int noise_encryption_send(noise_encryption_context_t *ctx, int fd,
                          const void *data, size_t data_len);
ssize_t noise_encryption_recv(noise_encryption_context_t *ctx, int fd,
                               void *buf, size_t buf_len);
bool noise_encryption_is_ready(noise_encryption_context_t *ctx);
```

### Java Side Structure

```java
// NoiseEncryption.java
public class NoiseEncryption {
    private long nativeContext; // Pointer to native context

    public NoiseEncryption(boolean isInitiator);
    public void cleanup();
    public boolean handshake(Socket socket) throws IOException;
    public void send(Socket socket, byte[] data) throws IOException;
    public byte[] recv(Socket socket, int length) throws IOException;
    public boolean isReady();
}
```

### Integration Points

1. **After PIN verification** (x11_streamer.c: ~1085):
   ```c
   // Perform Noise handshake
   noise_encryption_context_t *noise_ctx = noise_encryption_init(true);
   if (noise_encryption_handshake(noise_ctx, streamer->tv_fd) < 0) {
       // Handle error
   }
   streamer->noise_ctx = noise_ctx;
   ```

2. **In protocol_send_message** (protocol.c):
   ```c
   if (streamer->noise_ctx && noise_encryption_is_ready(streamer->noise_ctx)) {
       // Encrypt and send
       return noise_encryption_send(streamer->noise_ctx, fd, &header, sizeof(header));
       // ... then payload
   } else {
       // Unencrypted (backward compatibility)
   }
   ```

3. **In protocol_receive_message** (protocol.c):
   ```c
   if (streamer->noise_ctx && noise_encryption_is_ready(streamer->noise_ctx)) {
       // Receive and decrypt
       ssize_t received = noise_encryption_recv(streamer->noise_ctx, fd, header, sizeof(*header));
   }
   ```

## Next Steps

1. Choose and integrate Noise Protocol library for C
2. Choose and integrate Noise Protocol library for Java/Android
3. Implement encryption wrapper functions
4. Integrate into protocol layer
5. Test encrypted communication
6. Remove backward compatibility mode (optional)

## References

- Noise Protocol Framework: https://noiseprotocol.org/
- Noise Protocol Specification: https://noiseprotocol.org/noise.html
- snow library: https://github.com/mcginty/snow
- noise-java: https://github.com/rweather/noise-java

