#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>
#include <stdatomic.h>

#define FFT_SIZE 4096
#define WATERFALL_LINES 256
#define USB_BUFFER_SIZE (512 * 24)
#define DEFAULT_SAMPLE_RATE 192000

// Calculate visible bin range for zoom/pan display
// Sets start_bin and visible_bins; clamped_pan is the effective pan offset
static inline void calc_visible_range(int spectrum_size, int zoom_level, int pan_offset,
                                       int *start_bin, int *visible_bins) {
    int vb = spectrum_size / zoom_level;
    int max_pan = (spectrum_size - vb) / 2;
    int cp = pan_offset;
    if (cp < -max_pan) cp = -max_pan;
    if (cp > max_pan) cp = max_pan;
    *visible_bins = vb;
    *start_bin = (spectrum_size - vb) / 2 + cp;
}

#endif // APP_STATE_H
