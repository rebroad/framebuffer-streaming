#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

int protocol_send_message(int fd, message_type_t type, const void *data, size_t data_len)
{
    message_header_t header = {
        .type = type,
        .length = data_len,
        .sequence = 0  // TODO: implement sequence numbers
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

