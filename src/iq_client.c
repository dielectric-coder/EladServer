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
    pthread_mutex_t fd_mutex;  // Protects sock_fd
    atomic_int running;
    atomic_int connected;
    pthread_t recv_thread;
    iq_client_callback_t callback;
    void *user_data;
    atomic_uint_least32_t sample_rate;
};

iq_client_t *iq_client_new(void) {
    iq_client_t *client = calloc(1, sizeof(iq_client_t));
    if (!client) return NULL;
    client->sock_fd = -1;
    pthread_mutex_init(&client->fd_mutex, NULL);
    return client;
}

void iq_client_free(iq_client_t *client) {
    if (!client) return;
    iq_client_stop(client);
    pthread_mutex_destroy(&client->fd_mutex);
    free(client);
}

// Read exactly n bytes from socket, handling EINTR and timeouts.
// Returns 0 on success, -1 on error/disconnect.
// On timeout (EAGAIN), returns -1 with errno == EAGAIN only if no bytes were read yet.
// If partial data was received before timeout, keeps trying until complete or hard error.
static int recv_exact(int fd, uint8_t *buf, int n) {
    int total = 0;
    while (total < n) {
        int r = recv(fd, buf + total, n - total, 0);
        if (r > 0) {
            total += r;
        } else if (r == 0) {
            return -1;  // Connection closed
        } else {
            if (errno == EINTR)
                continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && total > 0)
                continue;  // Partial read in progress, keep trying
            return -1;
        }
    }
    return 0;
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

        // Validate sample_rate to prevent downstream division-by-zero
        if (header.sample_rate == 0) {
            fprintf(stderr, "IQ client: invalid sample rate 0\n");
            close(fd);
            continue;
        }

        atomic_store(&client->sample_rate, header.sample_rate);

        // Publish fd under lock
        pthread_mutex_lock(&client->fd_mutex);
        client->sock_fd = fd;
        pthread_mutex_unlock(&client->fd_mutex);

        atomic_store(&client->connected, 1);
        fprintf(stderr, "IQ client: connected (rate=%u Hz, format=%u-bit)\n",
                header.sample_rate, header.format);

        // Stream data
        int chunks = 0;
        while (atomic_load(&client->running)) {
            int rc = recv_exact(fd, buf, USB_BUFFER_SIZE);
            if (rc < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;  // Timeout, check running flag
                fprintf(stderr, "IQ client: recv failed after %d chunks (errno=%d: %s)\n",
                        chunks, errno, strerror(errno));
                break;  // Disconnected
            }
            chunks++;
            client->callback(buf, USB_BUFFER_SIZE, client->user_data);
        }

        // Disconnected - clear fd under lock
        atomic_store(&client->connected, 0);
        pthread_mutex_lock(&client->fd_mutex);
        client->sock_fd = -1;
        pthread_mutex_unlock(&client->fd_mutex);
        close(fd);
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

    // Shutdown socket to unblock recv (under lock to avoid racing with recv thread)
    pthread_mutex_lock(&client->fd_mutex);
    if (client->sock_fd >= 0) {
        shutdown(client->sock_fd, SHUT_RDWR);
        // Don't close here — recv thread owns the fd and will close it
    }
    pthread_mutex_unlock(&client->fd_mutex);

    pthread_join(client->recv_thread, NULL);
    atomic_store(&client->connected, 0);

    fprintf(stderr, "IQ client: stopped\n");
}

bool iq_client_is_connected(iq_client_t *client) {
    return client && atomic_load(&client->connected);
}

uint32_t iq_client_get_sample_rate(iq_client_t *client) {
    return client ? atomic_load(&client->sample_rate) : 0;
}
