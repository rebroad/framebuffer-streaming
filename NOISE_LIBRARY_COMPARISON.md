# Noise Protocol Library Comparison: snow vs noise-c

## Overview

Both libraries implement the Noise Protocol Framework for C applications. Here's a comparison to help decide which to use.

## snow (C Implementation)

**Repository**: https://github.com/mcginty/snow

### Pros:
- **Modern API**: Clean, simple API designed for ease of use
- **Active Development**: More recent updates and active maintenance
- **Well Documented**: Good documentation and examples
- **Smaller Codebase**: More focused implementation
- **Better Error Handling**: More user-friendly error reporting

### Cons:
- **Less Mature**: Newer project, may have fewer real-world deployments
- **Community**: Smaller community compared to noise-c (reference implementation)
- **No Formal Audit**: Hasn't undergone formal security audit (though based on Noise spec)

### Code Example:
```c
snow_t *snow = snow_create();
snow_init(snow, SNOW_INITIATOR);
snow_handshake(snow, fd);
snow_encrypt(snow, plaintext, ciphertext);
```

## noise-c (Official Reference Implementation)

**Repository**: https://github.com/rweather/noise-c

### Pros:
- **Reference Implementation**: Official reference implementation by Noise Protocol author
- **Mature**: Older, more battle-tested codebase
- **Widely Used**: Used as reference by other implementations
- **Complete**: Full implementation of all Noise patterns and primitives
- **Well Tested**: Extensive test suite

### Cons:
- **More Complex API**: Lower-level API, more verbose
- **Larger Codebase**: More code to integrate
- **Less Modern**: Older C style, may be less ergonomic

### Code Example:
```c
NoiseHandshakeState *handshake;
noise_handshakestate_new_by_name(&handshake, "Noise_NK_25519_ChaChaPoly_SHA256", NOISE_ROLE_INITIATOR);
noise_handshakestate_start(handshake);
// ... handshake messages ...
NoiseCipherState *send_cipher, *recv_cipher;
noise_handshakestate_get_cipher_states(handshake, &send_cipher, &recv_cipher);
```

## Recommendation

**For this project, I recommend `snow`** for the following reasons:

1. **Easier Integration**: Simpler API will make integration faster and less error-prone
2. **Modern Design**: Better suited for a modern C codebase
3. **Active Maintenance**: More likely to receive updates and fixes
4. **Sufficient Maturity**: While newer, it's based on the well-specified Noise Protocol

**However**, if you prefer:
- **Maximum stability and reference compliance**: Choose `noise-c`
- **Official reference implementation**: Choose `noise-c`
- **Easier integration and modern API**: Choose `snow`

## Integration Notes

Both libraries can be integrated as:
1. Git submodule
2. Copied source files
3. System library (if packaged)

For this project, I recommend using a git submodule for easier updates.

## Pattern Selection

For this use case (streamer = initiator, receiver = responder, no pre-shared key), recommended patterns:
- **Noise_NK_25519_ChaChaPoly_SHA256**: Receiver has static key, streamer uses ephemeral (good for this use case)
- **Noise_XX_25519_ChaChaPoly_SHA256**: Both sides exchange keys (more secure, but more complex)

I recommend **Noise_NK_25519_ChaChaPoly_SHA256** for simplicity - the receiver can generate a static key pair on first run and store it.

