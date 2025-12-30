#include "noise_encryption.h"
#include <noise/protocol.h>
#include <noise/protocol/buffer.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>  // For htons/ntohs
#include <stdio.h>

#define MAX_MESSAGE_LEN 65535
#define NOISE_PATTERN "Noise_NK_25519_ChaChaPoly_SHA256"  // Receiver has static key, streamer uses ephemeral

// Helper function to format Noise error messages
static void noise_log_error(const char *context, int err)
{
    char err_buf[256];
    noise_strerror(err, err_buf, sizeof(err_buf));
    fprintf(stderr, "%s: %s\n", context, err_buf);
}

struct noise_encryption_context {
    bool is_initiator;
    bool handshake_complete;
    NoiseHandshakeState *handshake;
    NoiseCipherState *send_cipher;
    NoiseCipherState *recv_cipher;
    uint8_t message_buffer[MAX_MESSAGE_LEN + 2];
};

noise_encryption_context_t *noise_encryption_init(bool is_initiator)
{
    noise_encryption_context_t *ctx = calloc(1, sizeof(noise_encryption_context_t));
    if (!ctx)
        return NULL;

    ctx->is_initiator = is_initiator;
    ctx->handshake_complete = false;
    ctx->handshake = NULL;
    ctx->send_cipher = NULL;
    ctx->recv_cipher = NULL;

    // Create handshake state
    int role = is_initiator ? NOISE_ROLE_INITIATOR : NOISE_ROLE_RESPONDER;
    int err = noise_handshakestate_new_by_name(&ctx->handshake, NOISE_PATTERN, role);
    if (err != NOISE_ERROR_NONE) {
        noise_log_error("Failed to create Noise handshake state", err);
        free(ctx);
        return NULL;
    }

    // For Noise_NK pattern:
    // - Initiator (streamer): needs remote public key (receiver's static key)
    // - Responder (receiver): needs local keypair (static key)
    // For now, we'll handle key setup in handshake function
    // In production, receiver should generate and store a static key pair

    return ctx;
}

void noise_encryption_cleanup(noise_encryption_context_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->send_cipher) {
        noise_cipherstate_free(ctx->send_cipher);
        ctx->send_cipher = NULL;
    }

    if (ctx->recv_cipher) {
        noise_cipherstate_free(ctx->recv_cipher);
        ctx->recv_cipher = NULL;
    }

    if (ctx->handshake) {
        noise_handshakestate_free(ctx->handshake);
        ctx->handshake = NULL;
    }

    free(ctx);
}

static int read_exact(int fd, void *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, (char *)buf + total, len - total, MSG_WAITALL);
        if (n <= 0) {
            if (n == 0) return 0;  // Connection closed
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return (int)total;
}

static int write_exact(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, (const char *)buf + total, len - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return (int)total;
}

int noise_encryption_handshake(noise_encryption_context_t *ctx, int fd)
{
    if (!ctx || fd < 0 || !ctx->handshake)
        return -1;

    int err;
    NoiseBuffer message_buf;
    NoiseBuffer payload_buf;

    // Start the handshake
    err = noise_handshakestate_start(ctx->handshake);
    if (err != NOISE_ERROR_NONE) {
        noise_log_error("Failed to start Noise handshake", err);
        return -1;
    }

    // Perform handshake according to Noise_NK pattern
    while (1) {
        int action = noise_handshakestate_get_action(ctx->handshake);

        if (action == NOISE_ACTION_NONE || action == NOISE_ACTION_COMPLETE) {
            // Handshake complete
            break;
        }

        if (action == NOISE_ACTION_FAILED) {
            fprintf(stderr, "Noise handshake failed\n");
            return -1;
        }

        if (action == NOISE_ACTION_WRITE_MESSAGE) {
            // We need to write a handshake message
            noise_buffer_set_output(message_buf, ctx->message_buffer, sizeof(ctx->message_buffer));
            noise_buffer_init(payload_buf);

            err = noise_handshakestate_write_message(ctx->handshake, &message_buf, &payload_buf);
            if (err != NOISE_ERROR_NONE) {
                noise_log_error("Failed to write Noise handshake message", err);
                return -1;
            }

            // Send message length (2 bytes, network byte order)
            uint16_t msg_len = htons((uint16_t)message_buf.size);
            if (write_exact(fd, &msg_len, 2) != 2) {
                fprintf(stderr, "Failed to send handshake message length\n");
                return -1;
            }

            // Send message
            if (write_exact(fd, message_buf.data, message_buf.size) != (int)message_buf.size) {
                fprintf(stderr, "Failed to send handshake message\n");
                return -1;
            }

        } else if (action == NOISE_ACTION_READ_MESSAGE) {
            // We need to read a handshake message
            uint16_t msg_len;
            if (read_exact(fd, &msg_len, 2) != 2) {
                fprintf(stderr, "Failed to receive handshake message length\n");
                return -1;
            }
            msg_len = ntohs(msg_len);

            if (msg_len >= MAX_MESSAGE_LEN) {
                fprintf(stderr, "Handshake message too large: %u\n", msg_len);
                return -1;
            }

            if (read_exact(fd, ctx->message_buffer, msg_len) != (int)msg_len) {
                fprintf(stderr, "Failed to receive handshake message\n");
                return -1;
            }

            noise_buffer_set_input(message_buf, ctx->message_buffer, msg_len);
            noise_buffer_init(payload_buf);

            err = noise_handshakestate_read_message(ctx->handshake, &message_buf, &payload_buf);
            if (err != NOISE_ERROR_NONE) {
                noise_log_error("Failed to read Noise handshake message", err);
                return -1;
            }

        } else {
            fprintf(stderr, "Unexpected handshake action: %d\n", action);
            return -1;
        }
    }

    // Split handshake into send/recv cipher states
    err = noise_handshakestate_split(ctx->handshake, &ctx->send_cipher, &ctx->recv_cipher);
    if (err != NOISE_ERROR_NONE) {
        noise_log_error("Failed to split Noise handshake", err);
        return -1;
    }

    ctx->handshake_complete = true;
    return 0;
}

int noise_encryption_send(noise_encryption_context_t *ctx, int fd,
                          const void *data, size_t data_len)
{
    if (!ctx || !data || fd < 0 || data_len == 0)
        return -1;

    if (!ctx->handshake_complete || !ctx->send_cipher) {
        errno = EINVAL;
        return -1;
    }

    // Encrypt the data
    // Need to leave room for MAC (16 bytes for ChaChaPoly)
    size_t max_plaintext = sizeof(ctx->message_buffer) - 16;
    if (data_len > max_plaintext) {
        errno = EMSGSIZE;
        return -1;
    }

    // Copy plaintext to buffer
    memcpy(ctx->message_buffer, data, data_len);

    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, ctx->message_buffer, data_len, sizeof(ctx->message_buffer));

    int err = noise_cipherstate_encrypt(ctx->send_cipher, &buffer);
    if (err != NOISE_ERROR_NONE) {
        noise_log_error("Failed to encrypt data", err);
        return -1;
    }

    // Send encrypted message length (2 bytes, network byte order)
    uint16_t msg_len = htons((uint16_t)buffer.size);
    if (write_exact(fd, &msg_len, 2) != 2) {
        return -1;
    }

    // Send encrypted data
    if (write_exact(fd, buffer.data, buffer.size) != (int)buffer.size) {
        return -1;
    }

    return 0;
}

ssize_t noise_encryption_recv(noise_encryption_context_t *ctx, int fd,
                               void *buf, size_t buf_len)
{
    if (!ctx || !buf || fd < 0 || buf_len == 0)
        return -1;

    if (!ctx->handshake_complete || !ctx->recv_cipher) {
        errno = EINVAL;
        return -1;
    }

    // Read encrypted message length
    uint16_t msg_len;
    if (read_exact(fd, &msg_len, 2) != 2) {
        if (errno == 0) return 0;  // Connection closed
        return -1;
    }
    msg_len = ntohs(msg_len);

    if (msg_len >= MAX_MESSAGE_LEN) {
        fprintf(stderr, "Encrypted message too large: %u\n", msg_len);
        errno = EMSGSIZE;
        return -1;
    }

    // Read encrypted data
    if (read_exact(fd, ctx->message_buffer, msg_len) != (int)msg_len) {
        return -1;
    }

    // Decrypt the data
    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, ctx->message_buffer, msg_len, sizeof(ctx->message_buffer));

    int err = noise_cipherstate_decrypt(ctx->recv_cipher, &buffer);
    if (err != NOISE_ERROR_NONE) {
        noise_log_error("Failed to decrypt data", err);
        return -1;
    }

    // buffer.size now contains decrypted size (plaintext, MAC removed)
    // Copy decrypted data to output buffer
    if (buffer.size > buf_len) {
        errno = EMSGSIZE;
        return -1;
    }
    memcpy(buf, buffer.data, buffer.size);

    return (ssize_t)buffer.size;
}

bool noise_encryption_is_ready(noise_encryption_context_t *ctx)
{
    return ctx && ctx->handshake_complete && ctx->send_cipher && ctx->recv_cipher;
}
