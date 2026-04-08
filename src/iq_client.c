#define _DEFAULT_SOURCE
#include "iq_client.h"
#include "iq_server.h"  // For iq_server_header_t, IQ_SERVER_MAGIC
#include "app_state.h"  // For USB_BUFFER_SIZE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zmq.h>
#include <pthread.h>
#include <stdatomic.h>

// SUB high-water mark — matches server PUB HWM
#define IQ_SUB_HWM 64

// Poll timeout for shutdown checks (ms)
#define IQ_POLL_TIMEOUT_MS 2000

// Consecutive poll timeouts before declaring "disconnected"
#define IQ_DISCONNECT_TIMEOUTS 5

struct iq_client {
    void *zmq_ctx;
    void *zmq_sub;
    atomic_int running;
    atomic_int connected;
    atomic_uint_least32_t sample_rate;
    pthread_t recv_thread;
    iq_client_callback_t callback;
    void *user_data;
    char endpoint[300];
};

iq_client_t *iq_client_new(void) {
    iq_client_t *client = calloc(1, sizeof(iq_client_t));
    if (!client) return NULL;
    client->zmq_ctx = zmq_ctx_new();
    if (!client->zmq_ctx) {
        free(client);
        return NULL;
    }
    return client;
}

void iq_client_free(iq_client_t *client) {
    if (!client) return;
    iq_client_stop(client);
    if (client->zmq_ctx) {
        zmq_ctx_destroy(client->zmq_ctx);
        client->zmq_ctx = NULL;
    }
    free(client);
}

static void *recv_thread_func(void *arg) {
    iq_client_t *client = (iq_client_t *)arg;
    uint8_t buf[USB_BUFFER_SIZE];
    iq_server_header_t header;
    int timeouts = 0;

    fprintf(stderr, "IQ client: subscribing to %s\n", client->endpoint);

    while (atomic_load(&client->running)) {
        zmq_pollitem_t item = { client->zmq_sub, 0, ZMQ_POLLIN, 0 };
        int rc = zmq_poll(&item, 1, IQ_POLL_TIMEOUT_MS);

        if (rc < 0) {
            if (errno == EINTR) continue;
            break;  // fatal error
        }
        if (rc == 0) {
            // Timeout — no data
            if (++timeouts >= IQ_DISCONNECT_TIMEOUTS) {
                if (atomic_load(&client->connected)) {
                    atomic_store(&client->connected, 0);
                    fprintf(stderr, "IQ client: no data, disconnected\n");
                }
            }
            continue;
        }
        timeouts = 0;

        // Frame 0: header (16 bytes)
        int n = zmq_recv(client->zmq_sub, &header, sizeof(header), 0);
        if (n != (int)sizeof(header)) {
            // Drain remaining frames of malformed message
            int more = 0;
            size_t more_sz = sizeof(more);
            zmq_getsockopt(client->zmq_sub, ZMQ_RCVMORE, &more, &more_sz);
            while (more) {
                zmq_recv(client->zmq_sub, buf, sizeof(buf), 0);
                zmq_getsockopt(client->zmq_sub, ZMQ_RCVMORE, &more, &more_sz);
            }
            continue;
        }

        // Validate magic
        if (memcmp(header.magic, IQ_SERVER_MAGIC, 4) != 0) continue;

        // Check for frame 1
        int more = 0;
        size_t more_sz = sizeof(more);
        zmq_getsockopt(client->zmq_sub, ZMQ_RCVMORE, &more, &more_sz);
        if (!more) continue;

        // Frame 1: IQ data
        n = zmq_recv(client->zmq_sub, buf, USB_BUFFER_SIZE, 0);
        if (n != USB_BUFFER_SIZE) continue;

        // Drain any unexpected extra frames
        zmq_getsockopt(client->zmq_sub, ZMQ_RCVMORE, &more, &more_sz);
        while (more) {
            uint8_t discard[64];
            zmq_recv(client->zmq_sub, discard, sizeof(discard), 0);
            zmq_getsockopt(client->zmq_sub, ZMQ_RCVMORE, &more, &more_sz);
        }

        // Update state
        atomic_store(&client->sample_rate, header.sample_rate);
        if (!atomic_load(&client->connected)) {
            atomic_store(&client->connected, 1);
            fprintf(stderr, "IQ client: receiving (rate=%u Hz, format=%u-bit)\n",
                    header.sample_rate, header.format);
        }

        client->callback(buf, USB_BUFFER_SIZE, client->user_data);
    }

    fprintf(stderr, "IQ client: thread stopped\n");
    return NULL;
}

int iq_client_start(iq_client_t *client, const char *host, int port,
                    iq_client_callback_t callback, void *user_data) {
    if (!client || !callback) return -1;
    if (atomic_load(&client->running)) return 0;

    snprintf(client->endpoint, sizeof(client->endpoint), "tcp://%s:%d", host, port);
    client->callback = callback;
    client->user_data = user_data;

    client->zmq_sub = zmq_socket(client->zmq_ctx, ZMQ_SUB);
    if (!client->zmq_sub) {
        fprintf(stderr, "IQ client: zmq_socket failed: %s\n", zmq_strerror(errno));
        return -1;
    }

    // Subscribe to all messages (no topic filter)
    zmq_setsockopt(client->zmq_sub, ZMQ_SUBSCRIBE, "", 0);

    int hwm = IQ_SUB_HWM;
    zmq_setsockopt(client->zmq_sub, ZMQ_RCVHWM, &hwm, sizeof(hwm));

    int linger = 0;
    zmq_setsockopt(client->zmq_sub, ZMQ_LINGER, &linger, sizeof(linger));

    // Auto-reconnect with 1-second interval
    int reconn = 1000;
    zmq_setsockopt(client->zmq_sub, ZMQ_RECONNECT_IVL, &reconn, sizeof(reconn));

    if (zmq_connect(client->zmq_sub, client->endpoint) != 0) {
        fprintf(stderr, "IQ client: zmq_connect(%s) failed: %s\n",
                client->endpoint, zmq_strerror(errno));
        zmq_close(client->zmq_sub);
        client->zmq_sub = NULL;
        return -1;
    }

    atomic_store(&client->running, 1);

    if (pthread_create(&client->recv_thread, NULL, recv_thread_func, client) != 0) {
        fprintf(stderr, "IQ client: failed to create thread\n");
        atomic_store(&client->running, 0);
        zmq_close(client->zmq_sub);
        client->zmq_sub = NULL;
        return -1;
    }

    return 0;
}

void iq_client_stop(iq_client_t *client) {
    if (!client || !atomic_load(&client->running)) return;

    atomic_store(&client->running, 0);
    pthread_join(client->recv_thread, NULL);

    if (client->zmq_sub) {
        zmq_close(client->zmq_sub);
        client->zmq_sub = NULL;
    }

    atomic_store(&client->connected, 0);
    fprintf(stderr, "IQ client: stopped\n");
}

bool iq_client_is_connected(iq_client_t *client) {
    return client && atomic_load(&client->connected);
}

uint32_t iq_client_get_sample_rate(iq_client_t *client) {
    return client ? atomic_load(&client->sample_rate) : 0;
}
