/*
 * Stub implementations for noise-c algorithms we don't use.
 * We only use Noise_NK_25519_ChaChaPoly_SHA256, so these are never called.
 */

#include <noise/protocol.h>
#include "internal.h"

// Stub for curve448
NoiseDHState *noise_curve448_new(void)
{
    return NULL;  // Not implemented - we don't use curve448
}

// Stub for newhope
NoiseDHState *noise_newhope_new(void)
{
    return NULL;  // Not implemented - we don't use newhope
}

// Stub for blake2b
NoiseHashState *noise_blake2b_new(void)
{
    return NULL;  // Not implemented - we don't use blake2b
}

// Stub for ed25519 signing (we use Noise_NK which doesn't require signing)
NoiseSignState *noise_ed25519_new(void)
{
    return NULL;  // Not implemented - we don't use signing
}

