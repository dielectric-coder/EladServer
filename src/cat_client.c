#define _DEFAULT_SOURCE
#include "cat_client.h"
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

// Filter bandwidth lookup tables (same as cat_control.c)
static const char *filter_lsb_usb[] = {
    "1.6k", "1.7k", "1.8k", "1.9k", "2.0k", "2.1k", "2.2k", "2.3k",
    "2.4k", "2.5k", "2.6k", "2.7k", "2.8k", "2.9k", "3.0k", "3.1k",
    "4.0k", "5.0k", "6.0k", "D300", "D600", "D1k"
};
#define FILTER_LSB_USB_COUNT 22

static const char *filter_cw[] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "100&4", "100&3", "100&2", "100&1", "100", "300", "500",
    "1.0k", "1.5k", "2.6k"
};
#define FILTER_CW_COUNT 17

static const char *filter_am[] = {
    "2.5k", "3.0k", "3.5k", "4.0k", "4.5k", "5.0k", "5.5k", "6.0k"
};
#define FILTER_AM_COUNT 8

static const char *filter_fm[] = {
    "Narrow", "Wide", "Data"
};
#define FILTER_FM_COUNT 3

int cat_client_get_freq_mode(cat_client_t *client, long *freq_hz, elad_mode_t *mode, int *vfo) {
    if (!client || client->fd < 0) return -1;

    char response[64];
    int len = cat_command(client, "IF;", response, sizeof(response));
    if (len < 32 || strncmp(response, "IF", 2) != 0)
        return -1;

    if (freq_hz) {
        char freq_str[12];
        strncpy(freq_str, response + 2, 11);
        freq_str[11] = '\0';
        *freq_hz = atol(freq_str);
    }

    if (mode) {
        int kenwood_mode = response[29] - '0';
        switch (kenwood_mode) {
            case 1: *mode = ELAD_MODE_LSB; break;
            case 2: *mode = ELAD_MODE_USB; break;
            case 3: *mode = ELAD_MODE_CW; break;
            case 4: *mode = ELAD_MODE_FM; break;
            case 5: *mode = ELAD_MODE_AM; break;
            case 7: *mode = ELAD_MODE_CWR; break;
            default: *mode = ELAD_MODE_UNKNOWN; break;
        }
    }

    if (vfo) {
        *vfo = response[30] - '0';
    }

    return 0;
}

int cat_client_get_filter_bw(cat_client_t *client, elad_mode_t mode,
                              char *filter_str, int filter_str_size) {
    if (!client || client->fd < 0 || !filter_str || filter_str_size < 1) return -1;

    char mode_char;
    switch (mode) {
        case ELAD_MODE_LSB: mode_char = '1'; break;
        case ELAD_MODE_USB: mode_char = '2'; break;
        case ELAD_MODE_CW:  mode_char = '3'; break;
        case ELAD_MODE_FM:  mode_char = '4'; break;
        case ELAD_MODE_AM:  mode_char = '5'; break;
        case ELAD_MODE_CWR: mode_char = '7'; break;
        default:
            filter_str[0] = '\0';
            return -1;
    }

    char cmd[8];
    snprintf(cmd, sizeof(cmd), "RF%c;", mode_char);

    char response[32];
    int len = cat_command(client, cmd, response, sizeof(response));
    if (len < 6 || strncmp(response, "RF", 2) != 0) {
        filter_str[0] = '\0';
        return -1;
    }

    char p2_str[3];
    p2_str[0] = response[3];
    p2_str[1] = response[4];
    p2_str[2] = '\0';
    int p2 = atoi(p2_str);

    const char *filter = NULL;
    switch (mode) {
        case ELAD_MODE_LSB:
        case ELAD_MODE_USB:
            if (p2 >= 0 && p2 < FILTER_LSB_USB_COUNT)
                filter = filter_lsb_usb[p2];
            break;
        case ELAD_MODE_CW:
        case ELAD_MODE_CWR:
            if (p2 >= 0 && p2 < FILTER_CW_COUNT)
                filter = filter_cw[p2];
            break;
        case ELAD_MODE_AM:
            if (p2 >= 0 && p2 < FILTER_AM_COUNT)
                filter = filter_am[p2];
            break;
        case ELAD_MODE_FM:
            if (p2 >= 0 && p2 < FILTER_FM_COUNT)
                filter = filter_fm[p2];
            break;
        default:
            break;
    }

    if (filter)
        snprintf(filter_str, filter_str_size, "%s", filter);
    else
        snprintf(filter_str, filter_str_size, "?%d", p2);

    return 0;
}
