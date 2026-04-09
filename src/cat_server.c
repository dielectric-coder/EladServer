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
    cat_client_t *cat_client;
    pthread_mutex_t *cat_mutex;

    // Track client sockets and threads for shutdown
    int client_fds[CAT_SERVER_MAX_CLIENTS];
    pthread_t client_threads[CAT_SERVER_MAX_CLIENTS];
    int client_thread_active[CAT_SERVER_MAX_CLIENTS];
    pthread_mutex_t clients_mutex;
    int client_count;

    // Demodulator status callback (DM command)
    cat_server_demod_callback_t demod_callback;
    void *demod_user_data;
};

typedef struct {
    cat_server_t *server;
    int client_fd;
    int slot_index;
} client_ctx_t;

cat_server_t *cat_server_new(void) {
    cat_server_t *server = calloc(1, sizeof(cat_server_t));
    if (!server) return NULL;
    server->listen_fd = -1;
    pthread_mutex_init(&server->clients_mutex, NULL);
    for (int i = 0; i < CAT_SERVER_MAX_CLIENTS; i++) {
        server->client_fds[i] = -1;
        server->client_thread_active[i] = 0;
    }
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

void cat_server_set_cat_client(cat_server_t *server, cat_client_t *client, pthread_mutex_t *cat_mutex) {
    if (!server) return;
    server->cat_client = client;
    server->cat_mutex = cat_mutex;
}

void cat_server_set_demod_callback(cat_server_t *server,
                                    cat_server_demod_callback_t callback, void *user_data) {
    if (!server) return;
    server->demod_callback = callback;
    server->demod_user_data = user_data;
}

// Add client fd atomically with count check; returns slot index on success, -1 if full
static int add_client_fd(cat_server_t *server, int fd) {
    pthread_mutex_lock(&server->clients_mutex);
    if (server->client_count >= CAT_SERVER_MAX_CLIENTS) {
        pthread_mutex_unlock(&server->clients_mutex);
        return -1;
    }
    for (int i = 0; i < CAT_SERVER_MAX_CLIENTS; i++) {
        if (server->client_fds[i] == -1) {
            server->client_fds[i] = fd;
            server->client_count++;
            pthread_mutex_unlock(&server->clients_mutex);
            return i;
        }
    }
    // Should not reach here if client_count is accurate
    pthread_mutex_unlock(&server->clients_mutex);
    return -1;
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

// Client handler thread (joinable)
static void *client_handler(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    cat_server_t *server = ctx->server;
    int fd = ctx->client_fd;
    int slot = ctx->slot_index;
    free(ctx);

    // Set receive timeout so thread checks running flag periodically
    struct timeval rcv_tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

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

                // Intercept DM (demodulator) command - don't forward to radio
                if (cmd_len >= 3 && buf[start] == 'D' && buf[start + 1] == 'M') {
                    if (cmd_len == 3) {
                        // "DM;" = demod inactive/disconnect
                        if (server->demod_callback)
                            server->demod_callback(0, DEMOD_MODE_AM, server->demod_user_data);
                    } else if (cmd_len >= 8) {
                        // "DMxx#####;" = mode(2) + bandwidth(5)
                        // Parse mode code (2 chars at offset 2)
                        int mode = DEMOD_MODE_AM;
                        if (buf[start + 2] == 'S' && buf[start + 3] == 'U')
                            mode = DEMOD_MODE_USB;
                        else if (buf[start + 2] == 'S' && buf[start + 3] == 'L')
                            mode = DEMOD_MODE_LSB;
                        else if (buf[start + 2] == 'C' && buf[start + 3] == 'W')
                            mode = DEMOD_MODE_CW;
                        // Parse bandwidth (digits at offset 4)
                        char bw_str[6];
                        int bw_digits = cmd_len - 5;  // subtract "DM" + mode(2) + ";"
                        if (bw_digits > 0 && bw_digits <= 5) {
                            strncpy(bw_str, buf + start + 4, bw_digits);
                            bw_str[bw_digits] = '\0';
                            int bw = atoi(bw_str);
                            if (bw > 0 && server->demod_callback)
                                server->demod_callback(bw, mode, server->demod_user_data);
                        }
                    }
                    // Acknowledge
                    if (send(fd, "DM;", 3, MSG_NOSIGNAL) < 0 && errno != EINTR) break;
                    start = i + 1;
                    continue;
                }

                pthread_mutex_lock(server->cat_mutex);
                if (server->cat && cat_control_is_open(server->cat)) {
                    resp_len = cat_control_raw_command(server->cat,
                                                       buf + start, cmd_len,
                                                       response, sizeof(response));
                } else if (server->cat_client && cat_client_is_connected(server->cat_client)) {
                    resp_len = cat_client_raw_command(server->cat_client,
                                                      buf + start, cmd_len,
                                                      response, sizeof(response));
                }
                pthread_mutex_unlock(server->cat_mutex);

                if (resp_len > 0) {
                    if (send(fd, response, resp_len, MSG_NOSIGNAL) < 0 && errno != EINTR) break;
                } else {
                    if (send(fd, "?;", 2, MSG_NOSIGNAL) < 0 && errno != EINTR) break;
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

    // Remove from tracking and close fd (only if server hasn't already closed it)
    pthread_mutex_lock(&server->clients_mutex);
    int found = 0;
    if (server->client_fds[slot] == fd) {
        server->client_fds[slot] = -1;
        server->client_count--;
        found = 1;
    }
    server->client_thread_active[slot] = 0;
    pthread_mutex_unlock(&server->clients_mutex);
    if (found)
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

        // Add to client list atomically (checks count inside lock)
        int slot = add_client_fd(server, client_fd);
        if (slot < 0) {
            fprintf(stderr, "CAT server: max clients reached, rejecting connection\n");
            close(client_fd);
            continue;
        }

        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        fprintf(stderr, "CAT server: client connected from %s:%d\n",
                addr_str, ntohs(client_addr.sin_port));

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) {
            remove_client_fd(server, client_fd);
            close(client_fd);
            continue;
        }
        ctx->server = server;
        ctx->client_fd = client_fd;
        ctx->slot_index = slot;

        // Join any previously finished thread in this slot
        pthread_mutex_lock(&server->clients_mutex);
        if (server->client_thread_active[slot]) {
            pthread_mutex_unlock(&server->clients_mutex);
            pthread_join(server->client_threads[slot], NULL);
            pthread_mutex_lock(&server->clients_mutex);
        }
        server->client_thread_active[slot] = 1;
        pthread_mutex_unlock(&server->clients_mutex);

        if (pthread_create(&server->client_threads[slot], NULL, client_handler, ctx) != 0) {
            fprintf(stderr, "CAT server: failed to create client thread\n");
            pthread_mutex_lock(&server->clients_mutex);
            server->client_thread_active[slot] = 0;
            pthread_mutex_unlock(&server->clients_mutex);
            remove_client_fd(server, client_fd);
            close(client_fd);
            free(ctx);
        }
    }

    fprintf(stderr, "CAT server: accept thread stopped\n");
    return NULL;
}

int cat_server_start(cat_server_t *server, int port, const char *listen_addr) {
    if (!server || (!server->cat && !server->cat_client) || !server->cat_mutex) return -1;
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

    // Shutdown all client sockets to unblock read() in handler threads
    // Don't close here — handler threads will close on exit to avoid double-close
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < CAT_SERVER_MAX_CLIENTS; i++) {
        if (server->client_fds[i] >= 0) {
            shutdown(server->client_fds[i], SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);

    pthread_join(server->accept_thread, NULL);

    // Join all client handler threads (now safe — accept thread is done)
    for (int i = 0; i < CAT_SERVER_MAX_CLIENTS; i++) {
        if (server->client_thread_active[i]) {
            pthread_join(server->client_threads[i], NULL);
            server->client_thread_active[i] = 0;
        }
    }

    fprintf(stderr, "CAT server: stopped\n");
}

bool cat_server_is_running(cat_server_t *server) {
    return server && atomic_load(&server->running);
}
