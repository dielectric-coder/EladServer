#ifndef CAT_CLIENT_H
#define CAT_CLIENT_H

#include <stdbool.h>
#include "usb_device.h"  // For elad_mode_t

typedef struct cat_client cat_client_t;

// Create CAT TCP client
cat_client_t *cat_client_new(void);

// Free CAT client
void cat_client_free(cat_client_t *client);

// Connect to remote CAT server
// Returns 0 on success, -1 on error
int cat_client_connect(cat_client_t *client, const char *host, int port);

// Disconnect from server
void cat_client_disconnect(cat_client_t *client);

// Check if connected
bool cat_client_is_connected(cat_client_t *client);

// Send raw CAT command and read response (same semantics as cat_control_raw_command)
// Returns number of response bytes, or -1 on error
int cat_client_raw_command(cat_client_t *client, const char *cmd, int cmd_len,
                           char *response, int response_size);

// Read frequency, mode and VFO via IF command
// Returns 0 on success, -1 on error
int cat_client_get_freq_mode(cat_client_t *client, long *freq_hz, elad_mode_t *mode, int *vfo);

// Read filter bandwidth via RF command
// Returns 0 on success, -1 on error
int cat_client_get_filter_bw(cat_client_t *client, elad_mode_t mode,
                              char *filter_str, int filter_str_size);

#endif // CAT_CLIENT_H
