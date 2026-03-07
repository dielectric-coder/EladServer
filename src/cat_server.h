#ifndef CAT_SERVER_H
#define CAT_SERVER_H

#include <stdbool.h>
#include <pthread.h>
#include "cat_control.h"

#define CAT_SERVER_DEFAULT_PORT 4532
#define CAT_SERVER_MAX_CLIENTS 8

typedef struct cat_server cat_server_t;

// Create CAT TCP server
cat_server_t *cat_server_new(void);

// Free CAT server
void cat_server_free(cat_server_t *server);

// Set CAT control and mutex for serial port access
void cat_server_set_cat(cat_server_t *server, cat_control_t *cat, pthread_mutex_t *cat_mutex);

// Start listening on given port (spawns accept thread)
// listen_addr: NULL or "localhost" for loopback only, "any" for all interfaces
// Returns 0 on success, -1 on error
int cat_server_start(cat_server_t *server, int port, const char *listen_addr);

// Stop server and disconnect all clients
void cat_server_stop(cat_server_t *server);

// Check if server is running
bool cat_server_is_running(cat_server_t *server);

#endif // CAT_SERVER_H
