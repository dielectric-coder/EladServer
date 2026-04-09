#define _DEFAULT_SOURCE
#include "iq_server.h"
#include "app_state.h"  // USB_BUFFER_SIZE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>

// Spillover buffer: 4 chunks ≈ 32 ms at 192 kHz.  Absorbs transient
// network stalls without blocking the USB thread or dropping data.
#define IQ_SPILL_MAX (USB_BUFFER_SIZE * 4)

struct iq_server {
    int listen_fd;
    int port;
    int bind_any;
    uint32_t sample_rate;
    atomic_int running;
    pthread_t accept_thread;

    // Client sockets protected by mutex
    int client_fds[IQ_SERVER_MAX_CLIENTS];
    uint8_t client_spill[IQ_SERVER_MAX_CLIENTS][IQ_SPILL_MAX];
    int client_spill_len[IQ_SERVER_MAX_CLIENTS];
    pthread_mutex_t clients_mutex;
    int client_count;
};

iq_server_t *iq_server_new(void) {
    iq_server_t *server = calloc(1, sizeof(iq_server_t));
    if (!server) return NULL;
    server->listen_fd = -1;
    pthread_mutex_init(&server->clients_mutex, NULL);
    for (int i = 0; i < IQ_SERVER_MAX_CLIENTS; i++) {
        server->client_fds[i] = -1;
        server->client_spill_len[i] = 0;
    }
    return server;
}

void iq_server_free(iq_server_t *server) {
    if (!server) return;
    iq_server_stop(server);
    pthread_mutex_destroy(&server->clients_mutex);
    free(server);
}

// Send header to newly connected client
static int send_header(int fd, uint32_t sample_rate) {
    iq_server_header_t header;
    memcpy(header.magic, IQ_SERVER_MAGIC, 4);
    header.sample_rate = sample_rate;
    header.format = 32;
    header.reserved = 0;

    int total = 0;
    while (total < (int)sizeof(header)) {
        int n = send(fd, (uint8_t *)&header + total, sizeof(header) - total, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

// Add client fd atomically with count check; returns 0 on success, -1 if full
static int add_client_fd(iq_server_t *server, int fd) {
    pthread_mutex_lock(&server->clients_mutex);
    if (server->client_count >= IQ_SERVER_MAX_CLIENTS) {
        pthread_mutex_unlock(&server->clients_mutex);
        return -1;
    }
    for (int i = 0; i < IQ_SERVER_MAX_CLIENTS; i++) {
        if (server->client_fds[i] == -1) {
            server->client_fds[i] = fd;
            server->client_spill_len[i] = 0;
            server->client_count++;
            pthread_mutex_unlock(&server->clients_mutex);
            return 0;
        }
    }
    // Should not reach here if client_count is accurate
    pthread_mutex_unlock(&server->clients_mutex);
    return -1;
}

// Accept thread
static void *accept_thread_func(void *arg) {
    iq_server_t *server = (iq_server_t *)arg;

    fprintf(stderr, "IQ server: listening on %s:%d (rate=%u Hz)\n",
            server->bind_any ? "0.0.0.0" : "127.0.0.1",
            server->port, server->sample_rate);

    while (atomic_load(&server->running)) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server->listen_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            if (atomic_load(&server->running))
                fprintf(stderr, "IQ server: accept error: %s\n", strerror(errno));
            break;
        }

        // Disable Nagle for low-latency streaming
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        // Large send buffer to absorb bursts (1MB; 192kHz IQ = 1.5 MB/s)
        int sndbuf = 1024 * 1024;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        // Set send timeout to prevent blocking on slow clients during header send
        struct timeval snd_tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

        // Send protocol header (before adding to client list, so broadcast won't block on it)
        if (send_header(client_fd, server->sample_rate) != 0) {
            fprintf(stderr, "IQ server: failed to send header\n");
            close(client_fd);
            continue;
        }

        // Add to client list atomically (checks count inside lock)
        if (add_client_fd(server, client_fd) != 0) {
            fprintf(stderr, "IQ server: max clients reached, rejecting\n");
            close(client_fd);
            continue;
        }

        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        fprintf(stderr, "IQ server: client connected from %s:%d\n",
                addr_str, ntohs(client_addr.sin_port));
    }

    fprintf(stderr, "IQ server: accept thread stopped\n");
    return NULL;
}

int iq_server_start(iq_server_t *server, int port, const char *listen_addr,
                    uint32_t sample_rate) {
    if (!server) return -1;
    if (atomic_load(&server->running)) return 0;

    server->port = port;
    server->sample_rate = sample_rate;

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        fprintf(stderr, "IQ server: socket error: %s\n", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server->bind_any = (listen_addr && strcmp(listen_addr, "any") == 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = server->bind_any ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "IQ server: bind error on port %d: %s\n", port, strerror(errno));
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    if (listen(server->listen_fd, 4) < 0) {
        fprintf(stderr, "IQ server: listen error: %s\n", strerror(errno));
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    atomic_store(&server->running, 1);

    if (pthread_create(&server->accept_thread, NULL, accept_thread_func, server) != 0) {
        fprintf(stderr, "IQ server: failed to create accept thread\n");
        atomic_store(&server->running, 0);
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    return 0;
}

void iq_server_stop(iq_server_t *server) {
    if (!server || !atomic_load(&server->running)) return;

    atomic_store(&server->running, 0);

    // Close listen socket to unblock accept()
    if (server->listen_fd >= 0) {
        shutdown(server->listen_fd, SHUT_RDWR);
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    // Close all client sockets
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < IQ_SERVER_MAX_CLIENTS; i++) {
        if (server->client_fds[i] >= 0) {
            shutdown(server->client_fds[i], SHUT_RDWR);
            close(server->client_fds[i]);
            server->client_fds[i] = -1;
        }
    }
    server->client_count = 0;
    pthread_mutex_unlock(&server->clients_mutex);

    pthread_join(server->accept_thread, NULL);

    fprintf(stderr, "IQ server: stopped\n");
}

bool iq_server_is_running(iq_server_t *server) {
    return server && atomic_load(&server->running);
}

// Non-blocking send helper.  Returns bytes sent (>=0) or -1 on fatal error.
static int nb_send(int fd, const uint8_t *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(fd, buf + total, len - total, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            total += n;
        } else if (n == 0) {
            return -1;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return total;  // can't send more right now
            return -1;         // real error
        }
    }
    return total;
}

void iq_server_broadcast(iq_server_t *server, const uint8_t *data, int length) {
    if (!server || !atomic_load(&server->running) || length <= 0) return;

    pthread_mutex_lock(&server->clients_mutex);

    for (int i = 0; i < IQ_SERVER_MAX_CLIENTS; i++) {
        int fd = server->client_fds[i];
        if (fd < 0) continue;

        int failed = 0;

        // Step 1: drain spillover from previous partial sends
        if (server->client_spill_len[i] > 0) {
            int n = nb_send(fd, server->client_spill[i],
                            server->client_spill_len[i]);
            if (n < 0) {
                failed = 1;
            } else if (n > 0) {
                int rem = server->client_spill_len[i] - n;
                if (rem > 0)
                    memmove(server->client_spill[i],
                            server->client_spill[i] + n, rem);
                server->client_spill_len[i] = rem;
            }
            // n == 0: still blocked, skip to append below
        }

        if (failed) goto disconnect;

        // Step 2: try to send new chunk directly (only if spill is empty)
        if (server->client_spill_len[i] == 0) {
            int n = nb_send(fd, data, length);
            if (n < 0) {
                failed = 1;
            } else if (n < length) {
                // Partial — save unsent remainder into spill
                int rem = length - n;
                memcpy(server->client_spill[i], data + n, rem);
                server->client_spill_len[i] = rem;
            }
            // n == length: fully sent, nothing to do
        } else {
            // Step 3: spill not yet drained — append new chunk if it fits
            int avail = IQ_SPILL_MAX - server->client_spill_len[i];
            if (length <= avail) {
                memcpy(server->client_spill[i] + server->client_spill_len[i],
                       data, length);
                server->client_spill_len[i] += length;
            } else {
                // Spill overflow — client is too slow
                failed = 1;
            }
        }

disconnect:
        if (failed) {
            close(fd);
            server->client_fds[i] = -1;
            server->client_spill_len[i] = 0;
            server->client_count--;
            fprintf(stderr, "IQ server: client disconnected (slow or error)\n");
        }
    }

    pthread_mutex_unlock(&server->clients_mutex);
}
