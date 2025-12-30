#ifndef NOISE_ENCRYPTION_H
#define NOISE_ENCRYPTION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Noise Protocol Framework encryption context
typedef struct noise_encryption_context noise_encryption_context_t;

// Initialize Noise Protocol encryption
// Returns NULL on error
noise_encryption_context_t *noise_encryption_init(bool is_initiator);

// Cleanup encryption context
void noise_encryption_cleanup(noise_encryption_context_t *ctx);

// Perform Noise handshake
// Returns 0 on success, -1 on error
int noise_encryption_handshake(noise_encryption_context_t *ctx, int fd);

// Encrypt and send data
// Returns 0 on success, -1 on error
int noise_encryption_send(noise_encryption_context_t *ctx, int fd,
						  const void *data, size_t data_len);

// Receive and decrypt data
// Returns number of bytes received (>0), 0 on connection close, -1 on error
ssize_t noise_encryption_recv(noise_encryption_context_t *ctx, int fd,
							   void *buf, size_t buf_len);

// Check if handshake is complete
bool noise_encryption_is_ready(noise_encryption_context_t *ctx);

#endif // NOISE_ENCRYPTION_H

