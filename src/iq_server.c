#define _DEFAULT_SOURCE
#include "iq_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zmq.h>
#include <stdatomic.h>

// PUB high-water mark: drop messages for slow subscribers beyond this.
// 64 messages × 12 KB ≈ 768 KB ≈ 500 ms at 192 kHz.
#define IQ_PUB_HWM 64

struct iq_server {
    void *zmq_ctx;
    void *zmq_pub;
    uint32_t sample_rate;
    atomic_int running;
    iq_server_header_t header;  // pre-built, sent with every message
};

iq_server_t *iq_server_new(void) {
    iq_server_t *server = calloc(1, sizeof(iq_server_t));
    if (!server) return NULL;
    server->zmq_ctx = zmq_ctx_new();
    if (!server->zmq_ctx) {
        free(server);
        return NULL;
    }
    return server;
}

void iq_server_free(iq_server_t *server) {
    if (!server) return;
    iq_server_stop(server);
    if (server->zmq_ctx) {
        zmq_ctx_destroy(server->zmq_ctx);
        server->zmq_ctx = NULL;
    }
    free(server);
}

int iq_server_start(iq_server_t *server, int port, const char *listen_addr,
                    uint32_t sample_rate) {
    if (!server) return -1;
    if (atomic_load(&server->running)) return 0;

    server->sample_rate = sample_rate;

    // Pre-build header
    memcpy(server->header.magic, IQ_SERVER_MAGIC, 4);
    server->header.sample_rate = sample_rate;
    server->header.format = 32;
    server->header.reserved = 0;

    server->zmq_pub = zmq_socket(server->zmq_ctx, ZMQ_PUB);
    if (!server->zmq_pub) {
        fprintf(stderr, "IQ server: zmq_socket failed: %s\n", zmq_strerror(errno));
        return -1;
    }

    int hwm = IQ_PUB_HWM;
    zmq_setsockopt(server->zmq_pub, ZMQ_SNDHWM, &hwm, sizeof(hwm));

    int linger = 0;
    zmq_setsockopt(server->zmq_pub, ZMQ_LINGER, &linger, sizeof(linger));

    int bind_any = (listen_addr && strcmp(listen_addr, "any") == 0);
    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "tcp://%s:%d",
             bind_any ? "0.0.0.0" : "127.0.0.1", port);

    if (zmq_bind(server->zmq_pub, endpoint) != 0) {
        fprintf(stderr, "IQ server: zmq_bind(%s) failed: %s\n",
                endpoint, zmq_strerror(errno));
        zmq_close(server->zmq_pub);
        server->zmq_pub = NULL;
        return -1;
    }

    atomic_store(&server->running, 1);
    fprintf(stderr, "IQ server: PUB bound to %s (rate=%u Hz, hwm=%d)\n",
            endpoint, sample_rate, hwm);
    return 0;
}

void iq_server_stop(iq_server_t *server) {
    if (!server || !atomic_load(&server->running)) return;
    atomic_store(&server->running, 0);

    if (server->zmq_pub) {
        zmq_close(server->zmq_pub);
        server->zmq_pub = NULL;
    }
    fprintf(stderr, "IQ server: stopped\n");
}

bool iq_server_is_running(iq_server_t *server) {
    return server && atomic_load(&server->running);
}

void iq_server_broadcast(iq_server_t *server, const uint8_t *data, int length) {
    if (!server || !server->zmq_pub || length <= 0) return;

    // Two-part message: [header (16 bytes)] [IQ data (12288 bytes)]
    int rc = zmq_send(server->zmq_pub, &server->header, sizeof(server->header),
                      ZMQ_DONTWAIT | ZMQ_SNDMORE);
    if (rc < 0) return;  // no subscribers or HWM reached — silently drop

    zmq_send(server->zmq_pub, data, length, ZMQ_DONTWAIT);
}
