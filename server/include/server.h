#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include <stdbool.h>

#define SERVER_DEFAULT_PORT 8888
#define MAX_CLIENTS 16

typedef struct server server_t;

server_t *server_create(int port);
void server_destroy(server_t *server);
int server_run(server_t *server);
void server_stop(server_t *server);

#endif // SERVER_H

