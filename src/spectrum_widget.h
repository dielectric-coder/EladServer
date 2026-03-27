#ifndef SPECTRUM_WIDGET_H
#define SPECTRUM_WIDGET_H

#include <gtk/gtk.h>
#include "app_state.h"
#include "bandplan.h"

G_BEGIN_DECLS

#define SPECTRUM_TYPE_WIDGET (spectrum_widget_get_type())
G_DECLARE_FINAL_TYPE(SpectrumWidget, spectrum_widget, SPECTRUM, WIDGET, GtkDrawingArea)

// Create a new spectrum widget
GtkWidget *spectrum_widget_new(void);

// Update spectrum data (thread-safe, copies data)
void spectrum_widget_update(SpectrumWidget *widget, const float *spectrum_db, int size);

// Set display range
void spectrum_widget_set_range(SpectrumWidget *widget, float min_db, float max_db);

// Set center frequency for display
void spectrum_widget_set_center_freq(SpectrumWidget *widget, int freq_hz);

// Set sample rate for frequency axis
void spectrum_widget_set_sample_rate(SpectrumWidget *widget, int sample_rate);

// Set overlay text (frequency and mode) displayed on top of spectrum
void spectrum_widget_set_overlay(SpectrumWidget *widget, const char *freq_str, const char *mode_str);

// Set horizontal zoom level (1, 2, 4, 8 = divisor of displayed frequency span)
void spectrum_widget_set_zoom(SpectrumWidget *widget, int zoom_level);

// Get current zoom level
int spectrum_widget_get_zoom(SpectrumWidget *widget);

// Set horizontal pan offset (bin offset from center, only effective when zoom > 1)
void spectrum_widget_set_pan(SpectrumWidget *widget, int pan_offset);

// Get current pan offset
int spectrum_widget_get_pan(SpectrumWidget *widget);

// Hide/show center frequency marker (hidden when demod app is connected)
void spectrum_widget_set_hide_center_marker(SpectrumWidget *widget, gboolean hide);

// Set bandplan for band overlay display
void spectrum_widget_set_bandplan(SpectrumWidget *widget, const bandplan_t *plan);

G_END_DECLS

#endif // SPECTRUM_WIDGET_H
