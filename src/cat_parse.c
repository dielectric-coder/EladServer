#include "cat_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Filter bandwidth lookup tables (per ELAD FDM-DUO manual)

// LSB/USB filters (P1=1,2): index 0-21
static const char *filter_lsb_usb[] = {
    "1.6k", "1.7k", "1.8k", "1.9k", "2.0k", "2.1k", "2.2k", "2.3k",
    "2.4k", "2.5k", "2.6k", "2.7k", "2.8k", "2.9k", "3.0k", "3.1k",
    "4.0k", "5.0k", "6.0k", "D300", "D600", "D1k"
};
#define FILTER_LSB_USB_COUNT 22

// CW/CWR filters (P1=3,7): valid indices 07-16
static const char *filter_cw[] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "100&4", "100&3", "100&2", "100&1", "100", "300", "500",
    "1.0k", "1.5k", "2.6k"
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

elad_mode_t cat_parse_kenwood_mode(int kenwood_mode) {
    switch (kenwood_mode) {
        case 1: return ELAD_MODE_LSB;
        case 2: return ELAD_MODE_USB;
        case 3: return ELAD_MODE_CW;
        case 4: return ELAD_MODE_FM;
        case 5: return ELAD_MODE_AM;
        case 7: return ELAD_MODE_CWR;
        default: return ELAD_MODE_UNKNOWN;
    }
}

char cat_parse_mode_char(elad_mode_t mode) {
    switch (mode) {
        case ELAD_MODE_LSB: return '1';
        case ELAD_MODE_USB: return '2';
        case ELAD_MODE_CW:  return '3';
        case ELAD_MODE_FM:  return '4';
        case ELAD_MODE_AM:  return '5';
        case ELAD_MODE_CWR: return '7';
        default: return 0;
    }
}

int cat_parse_if_response(const char *response, int len,
                           long *freq_hz, elad_mode_t *mode, int *vfo) {
    // IF response format: IF[freq 11][step 4][rit 5][...][mode][vfo]...;
    // Minimum 32 chars, starts with "IF"
    if (len < 32 || strncmp(response, "IF", 2) != 0)
        return -1;

    if (freq_hz) {
        char freq_str[12];
        strncpy(freq_str, response + 2, 11);
        freq_str[11] = '\0';
        *freq_hz = atol(freq_str);
    }

    if (mode) {
        *mode = cat_parse_kenwood_mode(response[29] - '0');
    }

    if (vfo) {
        *vfo = response[30] - '0';
    }

    return 0;
}

int cat_parse_rf_response(const char *response, int len, elad_mode_t mode,
                           char *filter_str, int filter_str_size) {
    if (!filter_str || filter_str_size < 1) return -1;

    // RF response format: RF P1 P2 P2 ; (e.g., "RF10808;")
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
