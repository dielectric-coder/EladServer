#ifndef CAT_SERVER_H
#define CAT_SERVER_H

#include <stdbool.h>
#include <pthread.h>
#include "cat_control.h"
#include "cat_client.h"

#define CAT_SERVER_DEFAULT_PORT 4532
#define CAT_SERVER_MAX_CLIENTS 8

typedef struct cat_server cat_server_t;

// Demod mode codes for DM command (maps to elad_mode_t-compatible values)
#define DEMOD_MODE_AM   0   // Symmetric (AM, SAM)
#define DEMOD_MODE_USB  1   // Upper sideband (USB, RTTY+, PSK31, MFSK16)
#define DEMOD_MODE_LSB  2   // Lower sideband (LSB, RTTY-)
#define DEMOD_MODE_CW   3   // CW (symmetric)

// Callback for demodulator status updates (DM command from SWLDemodTool)
// bandwidth_hz: demodulation bandwidth (0 = demod disconnected/inactive)
// mode: demod mode (DEMOD_MODE_AM/USB/LSB/CW)
typedef void (*cat_server_demod_callback_t)(int bandwidth_hz, int mode, void *user_data);

// Create CAT TCP server
cat_server_t *cat_server_new(void);

// Free CAT server
void cat_server_free(cat_server_t *server);

// Set CAT control (serial) and mutex for command forwarding
void cat_server_set_cat(cat_server_t *server, cat_control_t *cat, pthread_mutex_t *cat_mutex);

// Set CAT client (network) and mutex for command forwarding (alternative to set_cat)
void cat_server_set_cat_client(cat_server_t *server, cat_client_t *client, pthread_mutex_t *cat_mutex);

// Set callback for demodulator status updates (DM command)
void cat_server_set_demod_callback(cat_server_t *server,
                                    cat_server_demod_callback_t callback, void *user_data);

// Start listening on given port (spawns accept thread)
// listen_addr: NULL or "localhost" for loopback only, "any" for all interfaces
// Returns 0 on success, -1 on error
int cat_server_start(cat_server_t *server, int port, const char *listen_addr);

// Stop server and disconnect all clients
void cat_server_stop(cat_server_t *server);

// Check if server is running
bool cat_server_is_running(cat_server_t *server);

#endif // CAT_SERVER_H
