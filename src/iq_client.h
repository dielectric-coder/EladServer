#ifndef IQ_CLIENT_H
#define IQ_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct iq_client iq_client_t;

// Callback for received IQ data (same signature as usb_sample_callback_t)
typedef void (*iq_client_callback_t)(const uint8_t *data, int length, void *user_data);

// Create IQ client
iq_client_t *iq_client_new(void);

// Free IQ client
void iq_client_free(iq_client_t *client);

// Start connecting to remote IQ server (spawns receive thread)
// Returns 0 on success, -1 on error
int iq_client_start(iq_client_t *client, const char *host, int port,
                    iq_client_callback_t callback, void *user_data);

// Stop client and disconnect
void iq_client_stop(iq_client_t *client);

// Check if connected to server
bool iq_client_is_connected(iq_client_t *client);

// Get sample rate from server header (0 if not yet connected)
uint32_t iq_client_get_sample_rate(iq_client_t *client);

#endif // IQ_CLIENT_H
