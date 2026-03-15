#ifndef CAT_PARSE_H
#define CAT_PARSE_H

#include "usb_device.h"  // For elad_mode_t

// Convert Kenwood mode digit (from IF response) to elad_mode_t
elad_mode_t cat_parse_kenwood_mode(int kenwood_mode);

// Convert elad_mode_t to Kenwood RF command mode character
// Returns mode char ('1'-'7') or 0 on invalid mode
char cat_parse_mode_char(elad_mode_t mode);

// Parse IF command response to extract frequency, mode, and VFO
// response: full IF response string, len: response length
// Returns 0 on success, -1 on error
int cat_parse_if_response(const char *response, int len,
                           long *freq_hz, elad_mode_t *mode, int *vfo);

// Parse RF command response to extract filter bandwidth string
// response: full RF response string, len: response length
// mode: operating mode (determines which filter table to use)
// filter_str/filter_str_size: output buffer
// Returns 0 on success, -1 on error
int cat_parse_rf_response(const char *response, int len, elad_mode_t mode,
                           char *filter_str, int filter_str_size);

#endif // CAT_PARSE_H
