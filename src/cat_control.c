#define _DEFAULT_SOURCE
#include "cat_control.h"
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

    // Send command
    int written = write(cat->fd, cmd, cmd_len);
    if (written != cmd_len) {
        return -1;
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
            // Check if we got a complete response (ends with ;)
            if (total > 0 && response[total - 1] == ';') {
                break;
            }
        } else if (n == 0 || (n < 0 && errno == EAGAIN)) {
            retries--;
            usleep(10000);  // 10ms
        } else {
            return -1;
        }
    }

    response[total] = '\0';
    return total;
}

// Send command and read response (internal convenience wrapper)
static int cat_command(cat_control_t *cat, const char *cmd, char *response, int response_size) {
    return cat_control_raw_command(cat, cmd, strlen(cmd), response, response_size);
}

// Filter bandwidth lookup tables from RF CAT command (per ELAD FDM-DUO manual)
// LSB/USB filters (P1=1,2): index 0-21
static const char *filter_lsb_usb[] = {
    "1.6k", "1.7k", "1.8k", "1.9k", "2.0k", "2.1k", "2.2k", "2.3k",  // 00-07
    "2.4k", "2.5k", "2.6k", "2.7k", "2.8k", "2.9k", "3.0k", "3.1k",  // 08-15
    "4.0k", "5.0k", "6.0k", "D300", "D600", "D1k"                     // 16-21
};
#define FILTER_LSB_USB_COUNT 22

// CW/CWR filters (P1=3,7): valid indices 07-16
static const char *filter_cw[] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,                         // 00-06 invalid
    "100&4", "100&3", "100&2", "100&1", "100", "300", "500",         // 07-13
    "1.0k", "1.5k", "2.6k"                                            // 14-16
};
#define FILTER_CW_COUNT 17

// AM filters (P1=5): index 0-7
static const char *filter_am[] = {
    "2.5k", "3.0k", "3.5k", "4.0k", "4.5k", "5.0k", "5.5k", "6.0k"
};
#define FILTER_AM_COUNT 8

// FM filters (P1=4): index 0-2
static const char *filter_fm[] = {
    "Narrow", "Wide", "Data"
};
#define FILTER_FM_COUNT 3

int cat_control_get_freq_mode(cat_control_t *cat, long *freq_hz, elad_mode_t *mode, int *vfo) {
    if (!cat || cat->fd < 0) return -1;

    char response[64];

    // Use IF command to get frequency, mode and VFO
    // Response format: IF[freq 11][step 4][rit 5][rit+/-][xit+/-][0][0][mem 2][tx][mode][vfo][scan][split]...;
    // Positions:       2-12       13-16   17-21  22      23      24 25 26-27   28  29    30   31    32
    // Example: IF00014200000000000000000000000200;
    int len = cat_command(cat, "IF;", response, sizeof(response));
    if (len < 32 || strncmp(response, "IF", 2) != 0) {
        return -1;
    }

    // Extract frequency (characters 2-12, 11 digits)
    if (freq_hz) {
        char freq_str[12];
        strncpy(freq_str, response + 2, 11);
        freq_str[11] = '\0';
        *freq_hz = atol(freq_str);
    }

    // Extract mode (character 29)
    // Kenwood modes: 1=LSB, 2=USB, 3=CW, 4=FM, 5=AM, 7=CW-R
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

    // Extract VFO (character 30): 0=VFO A, 1=VFO B
    if (vfo) {
        *vfo = response[30] - '0';
    }

    return 0;
}

int cat_control_get_filter_bw(cat_control_t *cat, elad_mode_t mode, char *filter_str, int filter_str_size) {
    if (!cat || cat->fd < 0 || !filter_str || filter_str_size < 1) return -1;

    // Map elad_mode_t to Kenwood/ELAD RF command mode parameter
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

    // Send RF command: RF P1 ;
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "RF%c;", mode_char);

    char response[32];
    int len = cat_command(cat, cmd, response, sizeof(response));

    // Response format: RF P1 P2 P2 ; (e.g., "RF10808;")
    // P1 = 1 char mode, P2 P2 = 2 chars filter code
    if (len < 6 || strncmp(response, "RF", 2) != 0) {
        filter_str[0] = '\0';
        return -1;
    }

    // Extract P2 (filter code) - 2 digits starting at position 3
    char p2_str[3];
    p2_str[0] = response[3];
    p2_str[1] = response[4];
    p2_str[2] = '\0';
    int p2 = atoi(p2_str);

    // Look up filter string based on mode
    const char *filter = NULL;
    switch (mode) {
        case ELAD_MODE_LSB:
        case ELAD_MODE_USB:
            if (p2 >= 0 && p2 < FILTER_LSB_USB_COUNT) {
                filter = filter_lsb_usb[p2];
            }
            break;
        case ELAD_MODE_CW:
        case ELAD_MODE_CWR:
            if (p2 >= 0 && p2 < FILTER_CW_COUNT) {
                filter = filter_cw[p2];
            }
            break;
        case ELAD_MODE_AM:
            if (p2 >= 0 && p2 < FILTER_AM_COUNT) {
                filter = filter_am[p2];
            }
            break;
        case ELAD_MODE_FM:
            if (p2 >= 0 && p2 < FILTER_FM_COUNT) {
                filter = filter_fm[p2];
            }
            break;
        default:
            break;
    }

    if (filter) {
        snprintf(filter_str, filter_str_size, "%s", filter);
    } else {
        snprintf(filter_str, filter_str_size, "?%d", p2);
    }

    return 0;
}
