#define _DEFAULT_SOURCE
#include "iq_client.h"
#include "iq_server.h"  // For iq_server_header_t, IQ_SERVER_MAGIC
#include "app_state.h"  // For USB_BUFFER_SIZE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>

struct iq_client {
    char host[256];
    int port;
    int sock_fd;
    atomic_int running;
    atomic_int connected;
    pthread_t recv_thread;
    iq_client_callback_t callback;
    void *user_data;
    uint32_t sample_rate;
};

iq_client_t *iq_client_new(void) {
    iq_client_t *client = calloc(1, sizeof(iq_client_t));
    if (!client) return NULL;
    client->sock_fd = -1;
    return client;
}

void iq_client_free(iq_client_t *client) {
    if (!client) return;
    iq_client_stop(client);
    free(client);
}

// Read exactly n bytes from socket
static int recv_exact(int fd, uint8_t *buf, int n) {
    int total = 0;
    while (total < n) {
        int r = recv(fd, buf + total, n - total, 0);
        if (r <= 0) return -1;
        total += r;
    }
    return total;
}

static int connect_to_server(const char *host, int port) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    // Set receive timeout so recv unblocks for shutdown checks
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

static void *recv_thread_func(void *arg) {
    iq_client_t *client = (iq_client_t *)arg;
    uint8_t buf[USB_BUFFER_SIZE];

    fprintf(stderr, "IQ client: connecting to %s:%d\n", client->host, client->port);

    while (atomic_load(&client->running)) {
        // Connect
        int fd = connect_to_server(client->host, client->port);
        if (fd < 0) {
            if (atomic_load(&client->running))
                usleep(1000000);  // Retry in 1 second
            continue;
        }

        // Read and validate header
        iq_server_header_t header;
        if (recv_exact(fd, (uint8_t *)&header, sizeof(header)) < 0) {
            close(fd);
            continue;
        }

        if (memcmp(header.magic, IQ_SERVER_MAGIC, 4) != 0) {
            fprintf(stderr, "IQ client: invalid header magic\n");
            close(fd);
            continue;
        }

        client->sample_rate = header.sample_rate;
        client->sock_fd = fd;
        atomic_store(&client->connected, 1);
        fprintf(stderr, "IQ client: connected (rate=%u Hz, format=%u-bit)\n",
                header.sample_rate, header.format);

        // Stream data
        while (atomic_load(&client->running)) {
            int n = recv_exact(fd, buf, USB_BUFFER_SIZE);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;  // Timeout, check running flag
                break;  // Disconnected
            }
            client->callback(buf, USB_BUFFER_SIZE, client->user_data);
        }

        // Disconnected
        atomic_store(&client->connected, 0);
        close(fd);
        client->sock_fd = -1;
        fprintf(stderr, "IQ client: disconnected\n");

        if (atomic_load(&client->running))
            usleep(1000000);  // Wait before reconnect
    }

    fprintf(stderr, "IQ client: thread stopped\n");
    return NULL;
}

int iq_client_start(iq_client_t *client, const char *host, int port,
                    iq_client_callback_t callback, void *user_data) {
    if (!client || !callback) return -1;
    if (atomic_load(&client->running)) return 0;

    snprintf(client->host, sizeof(client->host), "%s", host);
    client->port = port;
    client->callback = callback;
    client->user_data = user_data;

    atomic_store(&client->running, 1);

    if (pthread_create(&client->recv_thread, NULL, recv_thread_func, client) != 0) {
        fprintf(stderr, "IQ client: failed to create thread\n");
        atomic_store(&client->running, 0);
        return -1;
    }

    return 0;
}

void iq_client_stop(iq_client_t *client) {
    if (!client || !atomic_load(&client->running)) return;

    atomic_store(&client->running, 0);

    // Close socket to unblock recv
    if (client->sock_fd >= 0) {
        shutdown(client->sock_fd, SHUT_RDWR);
        close(client->sock_fd);
        client->sock_fd = -1;
    }

    pthread_join(client->recv_thread, NULL);
    atomic_store(&client->connected, 0);

    fprintf(stderr, "IQ client: stopped\n");
}

bool iq_client_is_connected(iq_client_t *client) {
    return client && atomic_load(&client->connected);
}

uint32_t iq_client_get_sample_rate(iq_client_t *client) {
    return client ? client->sample_rate : 0;
}
