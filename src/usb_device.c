#define _DEFAULT_SOURCE
#include "usb_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/time.h>

#define S_RATE 122880000
#define NUM_TRANSFERS 2

struct usb_device {
    libusb_context *ctx;
    libusb_device_handle *handle;

    // Device info
    char serial[33];
    int hw_version_major;
    int hw_version_minor;
    int sample_rate_correction;

    // Streaming state
    struct libusb_transfer *transfers[NUM_TRANSFERS];
    uint8_t *transfer_buffers[NUM_TRANSFERS];
    usb_sample_callback_t callback;
    void *callback_user_data;
    atomic_int streaming;

    // Disconnection detection
    atomic_int disconnected;
    atomic_int transfers_pending;  // Count of transfers still in flight
};

static void transfer_callback(struct libusb_transfer *transfer);

usb_device_t *usb_device_new(void) {
    usb_device_t *dev = calloc(1, sizeof(usb_device_t));
    if (!dev) return NULL;

    int res = libusb_init(&dev->ctx);
    if (res < 0) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_strerror(res));
        free(dev);
        return NULL;
    }

    return dev;
}

void usb_device_free(usb_device_t *dev) {
    if (!dev) return;

    usb_device_close(dev);

    if (dev->ctx) {
        libusb_exit(dev->ctx);
    }

    free(dev);
}

int usb_device_open(usb_device_t *dev) {
    if (!dev || !dev->ctx) return -1;
    if (dev->handle) return 0;  // Already open

    // Reset disconnection flag
    atomic_store(&dev->disconnected, 0);

    unsigned char buffer[64];
    int res;

    // Open device
    dev->handle = libusb_open_device_with_vid_pid(dev->ctx, ELAD_VENDOR_ID, ELAD_PRODUCT_ID);
    if (!dev->handle) {
        fprintf(stderr, "Cannot open FDM-DUO device (VID=0x%04X, PID=0x%04X)\n",
                ELAD_VENDOR_ID, ELAD_PRODUCT_ID);
        return -1;
    }
    fprintf(stderr, "FDM-DUO device opened\n");

    // Detach kernel driver if active
    if (libusb_kernel_driver_active(dev->handle, 0) == 1) {
        fprintf(stderr, "Kernel driver active, detaching...\n");
        if (libusb_detach_kernel_driver(dev->handle, 0) == 0) {
            fprintf(stderr, "Kernel driver detached\n");
        }
    }

    // Claim interface
    res = libusb_claim_interface(dev->handle, 0);
    if (res < 0) {
        fprintf(stderr, "Cannot claim interface: %s\n", libusb_strerror(res));
        libusb_close(dev->handle);
        dev->handle = NULL;
        return -1;
    }
    fprintf(stderr, "Interface claimed\n");

    // Read USB driver version
    memset(buffer, 0, sizeof(buffer));
    res = libusb_control_transfer(dev->handle, 0xc0, 0xFF, 0x0000, 0x0000, buffer, 2, 1000);
    if (res == 2) {
        fprintf(stderr, "USB driver version: %d.%d\n", buffer[0], buffer[1]);
    }

    // Read HW version
    memset(buffer, 0, sizeof(buffer));
    res = libusb_control_transfer(dev->handle, 0xc0, 0xA2, 0x404C, 0x0151, buffer, 2, 1000);
    if (res == 2) {
        dev->hw_version_major = buffer[0];
        dev->hw_version_minor = buffer[1];
        fprintf(stderr, "HW version: %d.%d\n", dev->hw_version_major, dev->hw_version_minor);
    }

    // Read serial number
    memset(buffer, 0, sizeof(buffer));
    res = libusb_control_transfer(dev->handle, 0xc0, 0xA2, 0x4000, 0x0151, buffer, 32, 1000);
    if (res == 32) {
        memcpy(dev->serial, buffer, 32);
        dev->serial[32] = '\0';
        fprintf(stderr, "Serial: %s\n", dev->serial);
    }

    // Stop FIFO
    res = libusb_control_transfer(dev->handle, 0xc0, 0xE1, 0x0000, 0xE9 << 8, buffer, 1, 1000);
    if (res != 1) {
        fprintf(stderr, "Warning: Stop FIFO failed: res=%d\n", res);
    } else {
        fprintf(stderr, "FIFO stopped\n");
    }

    // Initialize FIFO (set EP6 FIFO to slave mode)
    res = libusb_control_transfer(dev->handle, 0xc0, 0xE1, 0x0000, 0xE8 << 8, buffer, 1, 1000);
    if (res != 1) {
        fprintf(stderr, "Warning: Init FIFO failed: res=%d\n", res);
    } else {
        fprintf(stderr, "FIFO initialized\n");
    }

    // Read sample rate correction
    memset(buffer, 0, sizeof(buffer));
    res = libusb_control_transfer(dev->handle, 0xc0, 0xA2, 0x4024, 0x0151, buffer, 4, 1000);
    if (res == 4) {
        memcpy(&dev->sample_rate_correction, buffer, 4);
        fprintf(stderr, "Sample rate correction: %d\n", dev->sample_rate_correction);
    }

    // Allocate transfer buffers
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        dev->transfer_buffers[i] = malloc(USB_BUFFER_SIZE);
        if (!dev->transfer_buffers[i]) {
            fprintf(stderr, "Failed to allocate transfer buffer\n");
            usb_device_close(dev);
            return -1;
        }
    }

    fprintf(stderr, "FDM-DUO initialized successfully\n");
    return 0;
}

void usb_device_close(usb_device_t *dev) {
    if (!dev) return;

    usb_device_stop_streaming(dev);

    // Free transfer buffers
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        free(dev->transfer_buffers[i]);
        dev->transfer_buffers[i] = NULL;
    }

    // Check if we need to reinit context (after disconnection)
    int was_disconnected = atomic_load(&dev->disconnected);

    if (dev->handle) {
        // Only release interface if device is still connected
        if (!was_disconnected) {
            libusb_release_interface(dev->handle, 0);
        }
        libusb_close(dev->handle);
        dev->handle = NULL;
    }

    // If device was disconnected, reinitialize libusb context
    // This ensures clean state for reconnection
    if (was_disconnected && dev->ctx) {
        fprintf(stderr, "Reinitializing libusb context...\n");
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
        int res = libusb_init(&dev->ctx);
        if (res < 0) {
            fprintf(stderr, "libusb_init failed: %s\n", libusb_strerror(res));
        } else {
            fprintf(stderr, "libusb context reinitialized\n");
        }
    }

    // Reset disconnection flag and pending count
    atomic_store(&dev->disconnected, 0);
    atomic_store(&dev->transfers_pending, 0);
}

bool usb_device_check_disconnected(usb_device_t *dev) {
    if (!dev) return true;
    return atomic_load(&dev->disconnected) != 0;
}

bool usb_device_is_open(usb_device_t *dev) {
    return dev && dev->handle != NULL;
}

int usb_device_set_frequency(usb_device_t *dev, long freq_hz) {
    if (!dev || !dev->handle) return -1;

    unsigned char buffer[16];
    int res;
    double tuning_freq;
    unsigned int tuning_word, tw_ls, tw_ms;
    long effective_rate = S_RATE + dev->sample_rate_correction;
    if (effective_rate <= 0) effective_rate = S_RATE;  // Guard against bad correction

    // Calculate tuning word for FPGA
    tuning_freq = freq_hz - (floor((double)freq_hz / effective_rate) * effective_rate);
    tuning_word = (unsigned int)((4294967296.0 * tuning_freq) / effective_rate);

    tw_ls = tuning_word & 0x0000FFFF;
    tuning_word >>= 16;
    tw_ms = (0xF2 << 8) | (tuning_word & 0x000000FF);
    tuning_word >>= 8;
    buffer[0] = tuning_word & 0x000000FF;
    buffer[1] = 0;

    // Set FPGA frequency
    res = libusb_control_transfer(dev->handle, 0x40, 0xE1, tw_ls, tw_ms, buffer, 2, 1000);
    if (res != 2) {
        fprintf(stderr, "Failed to set FPGA frequency\n");
        return -1;
    }

    // Wait for CAT buffer to be ready
    for (int j = 0; j < 200; j++) {
        res = libusb_control_transfer(dev->handle, 0xc0, 0xE1, 0x00, 0x0FC << 8, buffer, 3, 1000);
        if (res != 3 || (buffer[2] & 0x04) != 0x04) {
            break;
        }
        usleep(10000);
    }

    // Set CAT frequency
    memset(buffer, 0, sizeof(buffer));
    snprintf((char *)buffer, sizeof(buffer), "CF%11ld;", freq_hz);
    res = libusb_control_transfer(dev->handle, 0x40, 0xE1, 16, 0xF1 << 8, buffer, 16, 1000);

    fprintf(stderr, "Frequency set to %ld Hz\n", freq_hz);
    return 0;
}

static void transfer_callback(struct libusb_transfer *transfer) {
    usb_device_t *dev = (usb_device_t *)transfer->user_data;
    int should_resubmit = 0;

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (dev->callback && atomic_load(&dev->streaming)) {
            dev->callback(transfer->buffer, transfer->actual_length, dev->callback_user_data);
        }
        should_resubmit = atomic_load(&dev->streaming);
    } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE ||
               transfer->status == LIBUSB_TRANSFER_STALL ||
               transfer->status == LIBUSB_TRANSFER_ERROR) {
        // Device disconnected or fatal error
        fprintf(stderr, "USB device disconnected (transfer status: %d)\n", transfer->status);
        atomic_store(&dev->disconnected, 1);
        should_resubmit = 0;
    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        // Transfer was cancelled, don't resubmit
        should_resubmit = 0;
    } else if (transfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
        // Timeout - resubmit if still streaming
        should_resubmit = atomic_load(&dev->streaming);
    } else {
        fprintf(stderr, "Transfer error: %d\n", transfer->status);
        should_resubmit = 0;
    }

    // Resubmit or decrement pending count
    if (should_resubmit && !atomic_load(&dev->disconnected)) {
        int res = libusb_submit_transfer(transfer);
        if (res != 0) {
            fprintf(stderr, "Failed to resubmit transfer: %s\n", libusb_strerror(res));
            atomic_fetch_sub(&dev->transfers_pending, 1);
            if (res == LIBUSB_ERROR_NO_DEVICE || res == LIBUSB_ERROR_IO) {
                atomic_store(&dev->disconnected, 1);
            }
        }
        // If resubmit succeeded, transfer is still pending (count unchanged)
    } else {
        // Transfer is done, decrement pending count
        atomic_fetch_sub(&dev->transfers_pending, 1);
    }
}

int usb_device_start_streaming(usb_device_t *dev, usb_sample_callback_t callback, void *user_data) {
    if (!dev || !dev->handle) return -1;
    if (atomic_load(&dev->streaming)) return 0;

    unsigned char buffer[4];
    int res;

    dev->callback = callback;
    dev->callback_user_data = user_data;

    // Re-initialize FIFO (in case device was power cycled)
    // Stop FIFO first
    res = libusb_control_transfer(dev->handle, 0xc0, 0xE1, 0x0000, 0xE9 << 8, buffer, 1, 1000);
    if (res != 1) {
        fprintf(stderr, "Warning: Stop FIFO failed in start_streaming\n");
    }

    // Initialize FIFO (set EP6 FIFO to slave mode)
    res = libusb_control_transfer(dev->handle, 0xc0, 0xE1, 0x0000, 0xE8 << 8, buffer, 1, 1000);
    if (res != 1) {
        fprintf(stderr, "Warning: Init FIFO failed in start_streaming\n");
    }

    // Clear any stalled state on the bulk endpoint
    res = libusb_clear_halt(dev->handle, ELAD_RF_ENDPOINT);
    if (res < 0 && res != LIBUSB_ERROR_NOT_FOUND) {
        fprintf(stderr, "Warning: Clear halt failed: %s\n", libusb_strerror(res));
    } else {
        fprintf(stderr, "Endpoint cleared\n");
    }

    // Start FIFO
    res = libusb_control_transfer(dev->handle, 0xc0, 0xE1, 0x0001, 0x0E9 << 8, buffer, 1, 1000);
    if (res != 1 || buffer[0] != 0xE9) {
        fprintf(stderr, "Failed to enable streaming: res=%d, buffer[0]=0x%02X\n", res, buffer[0]);
        return -1;
    }
    fprintf(stderr, "Streaming enabled\n");

    atomic_store(&dev->streaming, 1);
    atomic_store(&dev->transfers_pending, 0);

    // Allocate and submit transfers
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        dev->transfers[i] = libusb_alloc_transfer(0);
        if (!dev->transfers[i]) {
            fprintf(stderr, "Failed to allocate transfer\n");
            usb_device_stop_streaming(dev);
            return -1;
        }

        libusb_fill_bulk_transfer(
            dev->transfers[i],
            dev->handle,
            ELAD_RF_ENDPOINT,
            dev->transfer_buffers[i],
            USB_BUFFER_SIZE,
            transfer_callback,
            dev,
            2000
        );

        res = libusb_submit_transfer(dev->transfers[i]);
        if (res != 0) {
            fprintf(stderr, "Failed to submit transfer: %s\n", libusb_strerror(res));
            usb_device_stop_streaming(dev);
            return -1;
        }
        atomic_fetch_add(&dev->transfers_pending, 1);
    }

    fprintf(stderr, "USB transfers submitted\n");
    return 0;
}

void usb_device_stop_streaming(usb_device_t *dev) {
    if (!dev) return;

    atomic_store(&dev->streaming, 0);

    // Cancel transfers if device is still connected
    if (!atomic_load(&dev->disconnected)) {
        for (int i = 0; i < NUM_TRANSFERS; i++) {
            if (dev->transfers[i]) {
                libusb_cancel_transfer(dev->transfers[i]);
            }
        }
    }

    // Wait for all pending transfers to complete their callbacks
    // This is essential - we can't free transfers until callbacks are done
    if (dev->ctx) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };  // 50ms
        int wait_count = 0;
        while (atomic_load(&dev->transfers_pending) > 0 && wait_count < 40) {  // Max 2 sec
            libusb_handle_events_timeout(dev->ctx, &tv);
            wait_count++;
        }
        if (atomic_load(&dev->transfers_pending) > 0) {
            fprintf(stderr, "Warning: %d transfers still pending after timeout, "
                    "skipping free to avoid use-after-free\n",
                    atomic_load(&dev->transfers_pending));
            return;  // Leak transfers rather than risk use-after-free
        }
    }

    // Now safe to free transfers (all callbacks have completed)
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        if (dev->transfers[i]) {
            libusb_free_transfer(dev->transfers[i]);
            dev->transfers[i] = NULL;
        }
    }

    // Stop FIFO (only if device still connected)
    if (dev->handle && !atomic_load(&dev->disconnected)) {
        unsigned char buffer[4];
        libusb_control_transfer(dev->handle, 0xc0, 0xE1, 0x0000, 0xE9 << 8, buffer, 1, 1000);
    }
}

int usb_device_handle_events(usb_device_t *dev) {
    if (!dev || !dev->ctx) return -1;
    // Use timeout to allow periodic disconnect checks
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };  // 100ms timeout
    return libusb_handle_events_timeout(dev->ctx, &tv);
}

const char *usb_device_get_serial(usb_device_t *dev) {
    return dev ? dev->serial : NULL;
}

int usb_device_get_hw_version_major(usb_device_t *dev) {
    return dev ? dev->hw_version_major : 0;
}

int usb_device_get_hw_version_minor(usb_device_t *dev) {
    return dev ? dev->hw_version_minor : 0;
}

long usb_device_get_frequency(usb_device_t *dev) {
    if (!dev || !dev->handle) return -1;

    unsigned char buffer[16];
    int res;

    // Read tuning frequency from FPGA
    memset(buffer, 0, sizeof(buffer));
    res = libusb_control_transfer(dev->handle, 0xc0, 0xE1, 0x00, 0x0F5 << 8, buffer, 11, 100);
    if (res != 11) {
        return -1;
    }

    // Frequency is in bytes 1-4, big-endian
    long freq = (long)buffer[1];
    freq <<= 8;
    freq |= (long)buffer[2];
    freq <<= 8;
    freq |= (long)buffer[3];
    freq <<= 8;
    freq |= (long)buffer[4];

    return freq;
}

elad_mode_t usb_device_get_mode(usb_device_t *dev) {
    long freq;
    elad_mode_t mode;
    if (usb_device_get_freq_mode(dev, &freq, &mode) == 0) {
        return mode;
    }
    return ELAD_MODE_UNKNOWN;
}

int usb_device_get_freq_mode(usb_device_t *dev, long *freq_hz, elad_mode_t *mode) {
    if (!dev || !dev->handle) return -1;

    unsigned char buffer[16];
    int res;

    // Read from FPGA register 0xF5 - single transfer for both freq and mode
    // Buffer layout: [0]=0xF5, [1-4]=freq (big-endian), [5-7]=?, [8]=mode info, [9-10]=?
    memset(buffer, 0, sizeof(buffer));
    res = libusb_control_transfer(dev->handle, 0xc0, 0xE1, 0x00, 0x0F5 << 8, buffer, 11, 100);
    if (res != 11) {
        return -1;
    }

    // Extract frequency from bytes 1-4 (big-endian)
    if (freq_hz) {
        long freq = (long)buffer[1];
        freq <<= 8;
        freq |= (long)buffer[2];
        freq <<= 8;
        freq |= (long)buffer[3];
        freq <<= 8;
        freq |= (long)buffer[4];
        *freq_hz = freq;
    }

    // Extract mode from byte 8, bits 0-3
    if (mode) {
        int m = buffer[8] & 0x0F;
        if (m >= 1 && m <= 6) {
            *mode = (elad_mode_t)m;
        } else {
            *mode = ELAD_MODE_UNKNOWN;
        }
    }

    return 0;
}

const char *usb_device_mode_name(elad_mode_t mode) {
    switch (mode) {
        case ELAD_MODE_AM:  return "AM";
        case ELAD_MODE_LSB: return "LSB";
        case ELAD_MODE_USB: return "USB";
        case ELAD_MODE_CW:  return "CW";
        case ELAD_MODE_FM:  return "FM";
        case ELAD_MODE_CWR: return "CW-R";
        default:            return "---";
    }
}
