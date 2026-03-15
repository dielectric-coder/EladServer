#define _DEFAULT_SOURCE
#include "cat_control.h"
#include "cat_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

struct cat_control {
    int fd;
    char device[256];
};

cat_control_t *cat_control_new(void) {
    cat_control_t *cat = calloc(1, sizeof(cat_control_t));
    if (!cat) return NULL;
    cat->fd = -1;
    return cat;
}

void cat_control_free(cat_control_t *cat) {
    if (!cat) return;
    cat_control_close(cat);
    free(cat);
}

int cat_control_open(cat_control_t *cat, const char *device) {
    if (!cat || !device) return -1;
    if (cat->fd >= 0) return 0;  // Already open

    // Open serial port
    cat->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (cat->fd < 0) {
        fprintf(stderr, "CAT: Cannot open %s: %s\n", device, strerror(errno));
        return -1;
    }

    // Configure serial port: 38400 8N1
    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    if (tcgetattr(cat->fd, &tty) != 0) {
        fprintf(stderr, "CAT: tcgetattr failed: %s\n", strerror(errno));
        close(cat->fd);
        cat->fd = -1;
        return -1;
    }

    // Set baud rate
    cfsetispeed(&tty, B38400);
    cfsetospeed(&tty, B38400);

    // 8N1, no flow control
    tty.c_cflag &= ~PARENB;        // No parity
    tty.c_cflag &= ~CSTOPB;        // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;            // 8 data bits
    tty.c_cflag &= ~CRTSCTS;       // No hardware flow control
    tty.c_cflag |= CREAD | CLOCAL; // Enable receiver, ignore modem control

    // Raw input
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);  // No software flow control
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    // Raw output
    tty.c_oflag &= ~OPOST;

    // Read timeout: 100ms
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;  // 100ms timeout

    if (tcsetattr(cat->fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "CAT: tcsetattr failed: %s\n", strerror(errno));
        close(cat->fd);
        cat->fd = -1;
        return -1;
    }

    // Flush any pending data
    tcflush(cat->fd, TCIOFLUSH);

    strncpy(cat->device, device, sizeof(cat->device) - 1);
    fprintf(stderr, "CAT: Opened %s at 38400 baud\n", device);

    return 0;
}

void cat_control_close(cat_control_t *cat) {
    if (!cat) return;
    if (cat->fd >= 0) {
        close(cat->fd);
        cat->fd = -1;
        fprintf(stderr, "CAT: Closed\n");
    }
}

bool cat_control_is_open(cat_control_t *cat) {
    return cat && cat->fd >= 0;
}

// Send raw command and read response (public API for passthrough)
int cat_control_raw_command(cat_control_t *cat, const char *cmd, int cmd_len,
                            char *response, int response_size) {
    if (!cat || cat->fd < 0) return -1;

    // Flush input buffer
    tcflush(cat->fd, TCIFLUSH);

    // Send command (retry on EINTR)
    int written = 0;
    while (written < cmd_len) {
        int n = write(cat->fd, cmd + written, cmd_len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += n;
    }

    // Small delay for radio to process
    usleep(50000);  // 50ms

    // Read response
    int total = 0;
    int retries = 10;
    while (total < response_size - 1 && retries > 0) {
        int n = read(cat->fd, response + total, response_size - 1 - total);
        if (n > 0) {
            total += n;
            // Scan for ';' terminator anywhere in received data
            for (int j = 0; j < total; j++) {
                if (response[j] == ';') {
                    total = j + 1;  // Truncate to end of first complete response
                    goto response_done;
                }
            }
        } else if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EINTR))) {
            if (errno != EINTR) retries--;
            usleep(10000);  // 10ms
        } else {
            return -1;
        }
    }
response_done:

    response[total] = '\0';
    return total;
}

// Send command and read response (internal convenience wrapper)
static int cat_command(cat_control_t *cat, const char *cmd, char *response, int response_size) {
    return cat_control_raw_command(cat, cmd, strlen(cmd), response, response_size);
}

int cat_control_get_freq_mode(cat_control_t *cat, long *freq_hz, elad_mode_t *mode, int *vfo) {
    if (!cat || cat->fd < 0) return -1;

    char response[64];
    int len = cat_command(cat, "IF;", response, sizeof(response));
    return cat_parse_if_response(response, len, freq_hz, mode, vfo);
}

int cat_control_get_filter_bw(cat_control_t *cat, elad_mode_t mode, char *filter_str, int filter_str_size) {
    if (!cat || cat->fd < 0 || !filter_str || filter_str_size < 1) return -1;

    char mode_char = cat_parse_mode_char(mode);
    if (!mode_char) {
        filter_str[0] = '\0';
        return -1;
    }

    char cmd[8];
    snprintf(cmd, sizeof(cmd), "RF%c;", mode_char);

    char response[32];
    int len = cat_command(cat, cmd, response, sizeof(response));
    return cat_parse_rf_response(response, len, mode, filter_str, filter_str_size);
}
