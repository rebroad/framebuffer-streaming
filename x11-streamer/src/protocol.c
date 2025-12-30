#include "protocol.h"
#include "noise_encryption.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>  // For htonl/ntohl, htons/ntohs
#include <stdatomic.h>

// Per-connection sequence counter (thread-safe, per connection)
// Note: Using TCP, so sequence numbers are mainly for debugging/monitoring
static _Thread_local uint32_t sequence_counter = 0;

int protocol_send_message(int fd, message_type_t type, const void *data, size_t data_len)
{
    message_header_t header = {
        .type = type,
        .length = htonl((uint32_t)data_len),  // Convert to network byte order
        .sequence = htonl(sequence_counter++)  // Convert to network byte order
    };

    // Send header
    ssize_t sent = send(fd, &header, sizeof(header), MSG_NOSIGNAL);
    if (sent != sizeof(header))
        return -1;

    // Send payload if any
    if (data && data_len > 0) {
        sent = send(fd, data, data_len, MSG_NOSIGNAL);
        if (sent != (ssize_t)data_len)
            return -1;
    }

    return 0;
}

int protocol_receive_message(int fd, message_header_t *header, void **payload)
{
    if (!header)
        return -1;

    // Receive header
    ssize_t received = recv(fd, header, sizeof(message_header_t), MSG_WAITALL);
    if (received != sizeof(message_header_t)) {
        if (received == 0)
            return 0;  // Connection closed
        return -1;
    }

    // Convert from network byte order
    header->length = ntohl(header->length);
    header->sequence = ntohl(header->sequence);

    // Allocate and receive payload
    if (header->length > 0) {
        *payload = malloc(header->length);
        if (!*payload)
            return -1;

        received = recv(fd, *payload, header->length, MSG_WAITALL);
        if (received != (ssize_t)header->length) {
            free(*payload);
            *payload = NULL;
            if (received == 0)
                return 0;  // Connection closed
            return -1;
        }
    } else {
        *payload = NULL;
    }

    return 1;
}

// Encrypted version of protocol_send_message
int protocol_send_message_encrypted(void *noise_ctx, int fd, message_type_t type, const void *data, size_t data_len)
{
    noise_encryption_context_t *ctx = (noise_encryption_context_t *)noise_ctx;

    if (!ctx || !noise_encryption_is_ready(ctx)) {
        // Fallback to unencrypted if encryption not ready
        return protocol_send_message(fd, type, data, data_len);
    }

    message_header_t header = {
        .type = type,
        .length = htonl((uint32_t)data_len),  // Convert to network byte order
        .sequence = htonl(sequence_counter++)  // Convert to network byte order
    };

    // Encrypt and send header
    if (noise_encryption_send(ctx, fd, &header, sizeof(header)) < 0)
        return -1;

    // Encrypt and send payload if any
    if (data && data_len > 0) {
        if (noise_encryption_send(ctx, fd, data, data_len) < 0)
            return -1;
    }

    return 0;
}

// Encrypted version of protocol_receive_message
int protocol_receive_message_encrypted(void *noise_ctx, int fd, message_header_t *header, void **payload)
{
    noise_encryption_context_t *ctx = (noise_encryption_context_t *)noise_ctx;

    if (!header)
        return -1;

    if (!ctx || !noise_encryption_is_ready(ctx)) {
        // Fallback to unencrypted if encryption not ready
        return protocol_receive_message(fd, header, payload);
    }

    // Receive and decrypt header
    ssize_t received = noise_encryption_recv(ctx, fd, header, sizeof(message_header_t));
    if (received != sizeof(message_header_t)) {
        if (received == 0)
            return 0;  // Connection closed
        return -1;
    }

    // Convert from network byte order
    header->length = ntohl(header->length);
    header->sequence = ntohl(header->sequence);

    // Allocate and receive payload
    if (header->length > 0) {
        *payload = malloc(header->length);
        if (!*payload)
            return -1;

        received = noise_encryption_recv(ctx, fd, *payload, header->length);
        if (received != (ssize_t)header->length) {
            free(*payload);
            *payload = NULL;
            if (received == 0)
                return 0;  // Connection closed
            return -1;
        }
    } else {
        *payload = NULL;
    }

    return 1;
}

