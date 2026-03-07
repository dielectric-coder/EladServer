#ifndef IQ_SERVER_H
#define IQ_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#define IQ_SERVER_MAX_CLIENTS 8
#define IQ_SERVER_MAGIC "ELAD"

// Header sent to each client on connection (16 bytes)
typedef struct __attribute__((packed)) {
    char magic[4];          // "ELAD"
    uint32_t sample_rate;   // e.g., 192000
    uint32_t format;        // 32 = 32-bit signed int IQ pairs
    uint32_t reserved;
} iq_server_header_t;

typedef struct iq_server iq_server_t;

// Create IQ streaming server
iq_server_t *iq_server_new(void);

// Free IQ server
void iq_server_free(iq_server_t *server);

// Start listening on given port (spawns accept thread)
// listen_addr: NULL or "localhost" for loopback only, "any" for all interfaces
// Returns 0 on success, -1 on error
int iq_server_start(iq_server_t *server, int port, const char *listen_addr,
                    uint32_t sample_rate);

// Stop server and disconnect all clients
void iq_server_stop(iq_server_t *server);

// Check if server is running
bool iq_server_is_running(iq_server_t *server);

// Broadcast IQ data to all connected clients
// Called from USB data callback thread
void iq_server_broadcast(iq_server_t *server, const uint8_t *data, int length);

#endif // IQ_SERVER_H
