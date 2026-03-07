#define _DEFAULT_SOURCE
#include "cat_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdatomic.h>

#define CMD_BUF_SIZE 256

struct cat_server {
    int listen_fd;
    int port;
    int bind_any;
    atomic_int running;
    pthread_t accept_thread;

    cat_control_t *cat;
    pthread_mutex_t *cat_mutex;

    // Track client sockets for shutdown
    int client_fds[CAT_SERVER_MAX_CLIENTS];
    pthread_mutex_t clients_mutex;
    int client_count;
};

typedef struct {
    cat_server_t *server;
    int client_fd;
} client_ctx_t;

cat_server_t *cat_server_new(void) {
    cat_server_t *server = calloc(1, sizeof(cat_server_t));
    if (!server) return NULL;
    server->listen_fd = -1;
    pthread_mutex_init(&server->clients_mutex, NULL);
    for (int i = 0; i < CAT_SERVER_MAX_CLIENTS; i++)
        server->client_fds[i] = -1;
    return server;
}

void cat_server_free(cat_server_t *server) {
    if (!server) return;
    cat_server_stop(server);
    pthread_mutex_destroy(&server->clients_mutex);
    free(server);
}

void cat_server_set_cat(cat_server_t *server, cat_control_t *cat, pthread_mutex_t *cat_mutex) {
    if (!server) return;
    server->cat = cat;
    server->cat_mutex = cat_mutex;
}

static void add_client_fd(cat_server_t *server, int fd) {
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < CAT_SERVER_MAX_CLIENTS; i++) {
        if (server->client_fds[i] == -1) {
            server->client_fds[i] = fd;
            server->client_count++;
            break;
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);
}

static void remove_client_fd(cat_server_t *server, int fd) {
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < CAT_SERVER_MAX_CLIENTS; i++) {
        if (server->client_fds[i] == fd) {
            server->client_fds[i] = -1;
            server->client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);
}

// Client handler thread (detached)
static void *client_handler(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    cat_server_t *server = ctx->server;
    int fd = ctx->client_fd;
    free(ctx);

    char buf[CMD_BUF_SIZE];
    int buf_len = 0;

    while (atomic_load(&server->running)) {
        int n = read(fd, buf + buf_len, sizeof(buf) - buf_len - 1);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            break;  // Client disconnected or error
        }
        buf_len += n;

        // Process all complete commands (terminated by ';')
        int start = 0;
        for (int i = 0; i < buf_len; i++) {
            if (buf[i] == ';') {
                int cmd_len = i - start + 1;  // Include the ';'
                char response[256];
                int resp_len = -1;

                pthread_mutex_lock(server->cat_mutex);
                if (cat_control_is_open(server->cat)) {
                    resp_len = cat_control_raw_command(server->cat,
                                                       buf + start, cmd_len,
                                                       response, sizeof(response));
                }
                pthread_mutex_unlock(server->cat_mutex);

                if (resp_len > 0) {
                    if (write(fd, response, resp_len) < 0) break;
                } else {
                    if (write(fd, "?;", 2) < 0) break;
                }

                start = i + 1;
            }
        }

        // Shift remaining partial command to front of buffer
        if (start > 0) {
            buf_len -= start;
            if (buf_len > 0)
                memmove(buf, buf + start, buf_len);
        }

        // Buffer overflow protection - discard if no ';' found in full buffer
        if (buf_len >= (int)sizeof(buf) - 1) {
            fprintf(stderr, "CAT server: client buffer overflow, closing connection\n");
            break;
        }
    }

    remove_client_fd(server, fd);
    close(fd);
    return NULL;
}

// Accept thread
static void *accept_thread_func(void *arg) {
    cat_server_t *server = (cat_server_t *)arg;

    fprintf(stderr, "CAT server: listening on %s:%d\n",
            server->bind_any ? "0.0.0.0" : "127.0.0.1", server->port);

    while (atomic_load(&server->running)) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server->listen_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            if (atomic_load(&server->running))
                fprintf(stderr, "CAT server: accept error: %s\n", strerror(errno));
            break;
        }

        // Check client count
        pthread_mutex_lock(&server->clients_mutex);
        int count = server->client_count;
        pthread_mutex_unlock(&server->clients_mutex);

        if (count >= CAT_SERVER_MAX_CLIENTS) {
            fprintf(stderr, "CAT server: max clients reached, rejecting connection\n");
            close(client_fd);
            continue;
        }

        fprintf(stderr, "CAT server: client connected from %s:%d\n",
                inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        add_client_fd(server, client_fd);

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) {
            remove_client_fd(server, client_fd);
            close(client_fd);
            continue;
        }
        ctx->server = server;
        ctx->client_fd = client_fd;

        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, client_handler, ctx) != 0) {
            fprintf(stderr, "CAT server: failed to create client thread\n");
            remove_client_fd(server, client_fd);
            close(client_fd);
            free(ctx);
        }
        pthread_attr_destroy(&attr);
    }

    fprintf(stderr, "CAT server: accept thread stopped\n");
    return NULL;
}

int cat_server_start(cat_server_t *server, int port, const char *listen_addr) {
    if (!server || !server->cat || !server->cat_mutex) return -1;
    if (atomic_load(&server->running)) return 0;  // Already running

    server->port = port;

    // Create listening socket
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        fprintf(stderr, "CAT server: socket error: %s\n", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Default to localhost; "any" binds to all interfaces
    server->bind_any = (listen_addr && strcmp(listen_addr, "any") == 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = server->bind_any ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "CAT server: bind error on port %d: %s\n", port, strerror(errno));
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    if (listen(server->listen_fd, 4) < 0) {
        fprintf(stderr, "CAT server: listen error: %s\n", strerror(errno));
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    atomic_store(&server->running, 1);

    if (pthread_create(&server->accept_thread, NULL, accept_thread_func, server) != 0) {
        fprintf(stderr, "CAT server: failed to create accept thread\n");
        atomic_store(&server->running, 0);
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    return 0;
}

void cat_server_stop(cat_server_t *server) {
    if (!server || !atomic_load(&server->running)) return;

    atomic_store(&server->running, 0);

    // Close listen socket to unblock accept()
    if (server->listen_fd >= 0) {
        shutdown(server->listen_fd, SHUT_RDWR);
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    // Close all client sockets to unblock read()
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < CAT_SERVER_MAX_CLIENTS; i++) {
        if (server->client_fds[i] >= 0) {
            shutdown(server->client_fds[i], SHUT_RDWR);
            close(server->client_fds[i]);
            server->client_fds[i] = -1;
        }
    }
    server->client_count = 0;
    pthread_mutex_unlock(&server->clients_mutex);

    pthread_join(server->accept_thread, NULL);

    fprintf(stderr, "CAT server: stopped\n");
}

bool cat_server_is_running(cat_server_t *server) {
    return server && atomic_load(&server->running);
}
