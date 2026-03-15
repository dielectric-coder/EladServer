#ifndef WATERFALL_WIDGET_H
#define WATERFALL_WIDGET_H

#include <gtk/gtk.h>
#include "app_state.h"

G_BEGIN_DECLS

#define WATERFALL_TYPE_WIDGET (waterfall_widget_get_type())
G_DECLARE_FINAL_TYPE(WaterfallWidget, waterfall_widget, WATERFALL, WIDGET, GtkDrawingArea)

// Create a new waterfall widget
GtkWidget *waterfall_widget_new(void);

// Add a new spectrum line (thread-safe, copies data)
void waterfall_widget_add_line(WaterfallWidget *widget, const float *spectrum_db, int size);

// Set display range
void waterfall_widget_set_range(WaterfallWidget *widget, float min_db, float max_db);

// Clear waterfall history
void waterfall_widget_clear(WaterfallWidget *widget);

// Set horizontal zoom level (1, 2, 4, 8 = divisor of displayed frequency span)
void waterfall_widget_set_zoom(WaterfallWidget *widget, int zoom_level);

// Get current zoom level
int waterfall_widget_get_zoom(WaterfallWidget *widget);

// Set horizontal pan offset (bin offset from center, only effective when zoom > 1)
void waterfall_widget_set_pan(WaterfallWidget *widget, int pan_offset);

// Get current pan offset
int waterfall_widget_get_pan(WaterfallWidget *widget);

// Set filter bandwidth for drawing bandwidth lines
// bandwidth_hz: filter bandwidth in Hz
// mode: current operating mode (for asymmetric SSB display)
// center_offset_hz: offset from tuned frequency (e.g., +1500 for data modes)
// is_resonator: true for CW resonator modes (draws orange instead of red)
void waterfall_widget_set_bandwidth(WaterfallWidget *widget, int bandwidth_hz, int mode, int center_offset_hz, int is_resonator);

// Set demodulator bandwidth for drawing yellow indicator lines
// bandwidth_hz: demodulation bandwidth in Hz (0 to disable)
// mode: DEMOD_MODE_AM/USB/LSB/CW from cat_server.h
void waterfall_widget_set_demod_bandwidth(WaterfallWidget *widget, int bandwidth_hz, int mode);

// Set sample rate (needed for Hz to bin conversion)
void waterfall_widget_set_sample_rate(WaterfallWidget *widget, int sample_rate);

G_END_DECLS

#endif // WATERFALL_WIDGET_H
