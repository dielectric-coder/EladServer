#define _DEFAULT_SOURCE
#include "iq_server.h"
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

// Disconnect after this many consecutive dropped chunks
#define IQ_DROP_LIMIT 50

struct iq_server {
    int listen_fd;
    int port;
    int bind_any;
    uint32_t sample_rate;
    atomic_int running;
    pthread_t accept_thread;

    // Client sockets protected by mutex
    int client_fds[IQ_SERVER_MAX_CLIENTS];
    int client_drops[IQ_SERVER_MAX_CLIENTS];  // Consecutive EAGAIN count
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
        server->client_drops[i] = 0;
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
        int n = write(fd, (uint8_t *)&header + total, sizeof(header) - total);
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
            server->client_drops[i] = 0;
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

void iq_server_broadcast(iq_server_t *server, const uint8_t *data, int length) {
    if (!server || !atomic_load(&server->running) || length <= 0) return;

    pthread_mutex_lock(&server->clients_mutex);

    for (int i = 0; i < IQ_SERVER_MAX_CLIENTS; i++) {
        int fd = server->client_fds[i];
        if (fd < 0) continue;

        int total = 0;
        int failed = 0;
        while (total < length) {
            int n = send(fd, data + total, length - total, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Send buffer full - drop this chunk, track consecutive drops
                    server->client_drops[i]++;
                    if (server->client_drops[i] >= IQ_DROP_LIMIT) {
                        fprintf(stderr, "IQ server: client too slow, disconnecting\n");
                        failed = 1;
                    }
                    break;
                }
                // Real error (connection reset, broken pipe, etc.)
                failed = 1;
                break;
            }
            if (n == 0) {
                failed = 1;
                break;
            }
            total += n;
        }

        if (total > 0)
            server->client_drops[i] = 0;  // Successful send resets drop counter

        if (failed) {
            fprintf(stderr, "IQ server: client disconnected\n");
            close(fd);
            server->client_fds[i] = -1;
            server->client_drops[i] = 0;
            server->client_count--;
        }
    }

    pthread_mutex_unlock(&server->clients_mutex);
}
