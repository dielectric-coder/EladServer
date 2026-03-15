#define _DEFAULT_SOURCE
#include "cat_client.h"
#include "cat_parse.h"
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

struct cat_client {
    int fd;
    char host[256];
    int port;
};

cat_client_t *cat_client_new(void) {
    cat_client_t *client = calloc(1, sizeof(cat_client_t));
    if (!client) return NULL;
    client->fd = -1;
    return client;
}

void cat_client_free(cat_client_t *client) {
    if (!client) return;
    cat_client_disconnect(client);
    free(client);
}

int cat_client_connect(cat_client_t *client, const char *host, int port) {
    if (!client || !host) return -1;
    if (client->fd >= 0) return 0;  // Already connected

    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        fprintf(stderr, "CAT client: cannot resolve %s\n", host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    // Read/write timeout for CAT commands
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "CAT client: connect to %s:%d failed: %s\n",
                host, port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);

    client->fd = fd;
    snprintf(client->host, sizeof(client->host), "%s", host);
    client->port = port;

    fprintf(stderr, "CAT client: connected to %s:%d\n", host, port);
    return 0;
}

void cat_client_disconnect(cat_client_t *client) {
    if (!client || client->fd < 0) return;
    close(client->fd);
    client->fd = -1;
    fprintf(stderr, "CAT client: disconnected\n");
}

bool cat_client_is_connected(cat_client_t *client) {
    return client && client->fd >= 0;
}

int cat_client_raw_command(cat_client_t *client, const char *cmd, int cmd_len,
                           char *response, int response_size) {
    if (!client || client->fd < 0) return -1;

    // Send command (retry on EINTR)
    int written = 0;
    while (written < cmd_len) {
        int n = send(client->fd, cmd + written, cmd_len - written, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            cat_client_disconnect(client);
            return -1;
        }
        if (n == 0) {
            cat_client_disconnect(client);
            return -1;
        }
        written += n;
    }

    // Read response until ';'
    int total = 0;
    int retries = 20;  // 2 seconds with 100ms timeout chunks
    while (total < response_size - 1 && retries > 0) {
        int n = recv(client->fd, response + total, response_size - 1 - total, 0);
        if (n > 0) {
            total += n;
            // Scan for ';' terminator anywhere in received data
            for (int j = 0; j < total; j++) {
                if (response[j] == ';') {
                    total = j + 1;
                    goto response_done;
                }
            }
        } else if (n == 0) {
            // Server closed connection
            cat_client_disconnect(client);
            return -1;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                retries--;
                continue;
            }
            cat_client_disconnect(client);
            return -1;
        }
    }
response_done:

    response[total] = '\0';
    return total;
}

// Convenience wrapper
static int cat_command(cat_client_t *client, const char *cmd,
                       char *response, int response_size) {
    return cat_client_raw_command(client, cmd, strlen(cmd), response, response_size);
}

int cat_client_get_freq_mode(cat_client_t *client, long *freq_hz, elad_mode_t *mode, int *vfo) {
    if (!client || client->fd < 0) return -1;

    char response[64];
    int len = cat_command(client, "IF;", response, sizeof(response));
    return cat_parse_if_response(response, len, freq_hz, mode, vfo);
}

int cat_client_get_filter_bw(cat_client_t *client, elad_mode_t mode,
                              char *filter_str, int filter_str_size) {
    if (!client || client->fd < 0 || !filter_str || filter_str_size < 1) return -1;

    char mode_char = cat_parse_mode_char(mode);
    if (!mode_char) {
        filter_str[0] = '\0';
        return -1;
    }

    char cmd[8];
    snprintf(cmd, sizeof(cmd), "RF%c;", mode_char);

    char response[32];
    int len = cat_command(client, cmd, response, sizeof(response));
    return cat_parse_rf_response(response, len, mode, filter_str, filter_str_size);
}
