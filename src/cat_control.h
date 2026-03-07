#ifndef CAT_CONTROL_H
#define CAT_CONTROL_H

#include <stdbool.h>
#include "usb_device.h"  // For elad_mode_t

typedef struct cat_control cat_control_t;

// Create CAT control handler
cat_control_t *cat_control_new(void);

// Free CAT control handler
void cat_control_free(cat_control_t *cat);

// Open serial port (e.g., "/dev/ttyUSB0")
int cat_control_open(cat_control_t *cat, const char *device);

// Close serial port
void cat_control_close(cat_control_t *cat);

// Check if port is open
bool cat_control_is_open(cat_control_t *cat);

// Read frequency, mode and VFO via CAT commands (single poll)
// vfo: 0=VFO A, 1=VFO B
// Returns 0 on success, -1 on error
int cat_control_get_freq_mode(cat_control_t *cat, long *freq_hz, elad_mode_t *mode, int *vfo);

// Send raw CAT command and read response (for external passthrough)
// cmd/cmd_len: command bytes to send (e.g., "IF;" with len=3)
// response/response_size: buffer for response
// Returns number of response bytes, or -1 on error
int cat_control_raw_command(cat_control_t *cat, const char *cmd, int cmd_len,
                            char *response, int response_size);

// Read filter bandwidth via RF CAT command
// mode: current operating mode (needed to send correct RF query)
// filter_str: output buffer for human-readable filter string (e.g., "2.4k", "500")
// filter_str_size: size of output buffer
// Returns 0 on success, -1 on error
int cat_control_get_filter_bw(cat_control_t *cat, elad_mode_t mode, char *filter_str, int filter_str_size);

#endif // CAT_CONTROL_H
