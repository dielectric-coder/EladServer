#include "spectrum_widget.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

struct _SpectrumWidget {
    GtkDrawingArea parent_instance;

    // Spectrum data (protected by mutex)
    GMutex data_mutex;
    float *spectrum_db;
    int spectrum_size;

    // Display parameters
    float min_db;
    float max_db;
    int center_freq_hz;
    int sample_rate;
    int zoom_level;  // 1, 2, 4, 8 = horizontal zoom factor
    int pan_offset;  // Bin offset from center (only effective when zoom > 1)

    // Overlay text
    char overlay_freq[32];
    char overlay_mode[64];

    // Hide center frequency marker when demod app is connected
    gboolean hide_center_marker;

    // Band overlay
    const bandplan_t *bandplan;
};

G_DEFINE_TYPE(SpectrumWidget, spectrum_widget, GTK_TYPE_DRAWING_AREA)

// Margins for axis labels
#define MARGIN_LEFT 55
#define MARGIN_BOTTOM 20

static void spectrum_widget_draw(GtkDrawingArea *area, cairo_t *cr,
                                  int width, int height, gpointer user_data G_GNUC_UNUSED) {
    SpectrumWidget *self = SPECTRUM_WIDGET(area);

    // Black background
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    // Calculate plot area (inside margins)
    int plot_x = MARGIN_LEFT;
    int plot_y = 0;
    int plot_width = width - MARGIN_LEFT;
    int plot_height = height - MARGIN_BOTTOM;

    if (plot_width <= 0 || plot_height <= 0) return;

    // Lock data for reading
    g_mutex_lock(&self->data_mutex);

    // Draw band overlays on x-axis (frequency axis at bottom)
    // Note: Use FFT_SIZE constant instead of spectrum_size so bands draw before data arrives
    if (self->bandplan && self->bandplan->count > 0 && self->sample_rate > 0) {
        // Calculate visible frequency range
        double freq_span = (double)self->sample_rate / self->zoom_level;
        double hz_per_bin = (double)self->sample_rate / FFT_SIZE;
        double pan_hz = self->pan_offset * hz_per_bin;
        int64_t freq_start = (int64_t)(self->center_freq_hz - freq_span / 2 + pan_hz);
        int64_t freq_end = (int64_t)(self->center_freq_hz + freq_span / 2 + pan_hz);

        // Find visible bands
        int visible_indices[32];
        int num_visible = bandplan_find_visible(self->bandplan, freq_start, freq_end,
                                                 visible_indices, 32);

        // Draw each visible band as colored rectangle on x-axis
        for (int i = 0; i < num_visible; i++) {
            const band_entry_t *band = &self->bandplan->bands[visible_indices[i]];

            // Set color based on band type
            switch (band->tag) {
                case BAND_TAG_HAMRADIO:
                    cairo_set_source_rgba(cr, 0.0, 0.8, 0.0, 0.6);  // Green
                    break;
                case BAND_TAG_BROADCAST:
                    cairo_set_source_rgba(cr, 1.0, 0.5, 0.0, 0.6);  // Orange
                    break;
                default:
                    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.4);  // Gray for other bands
                    break;
            }

            // Convert band frequencies to pixel coordinates
            double band_start_x = plot_x + ((double)(band->lower_bound - freq_start) / freq_span) * plot_width;
            double band_end_x = plot_x + ((double)(band->upper_bound - freq_start) / freq_span) * plot_width;

            // Clamp to plot area boundaries
            if (band_start_x < plot_x) band_start_x = plot_x;
            if (band_end_x > plot_x + plot_width) band_end_x = plot_x + plot_width;

            // Draw rectangle on x-axis area (bottom margin)
            if (band_end_x > band_start_x) {
                cairo_rectangle(cr, band_start_x, plot_y + plot_height, band_end_x - band_start_x, MARGIN_BOTTOM);
                cairo_fill(cr);
            }
        }
    }

    float range = self->max_db - self->min_db;
    if (range < 1.0f) range = 1.0f;

    // Set up font for labels
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);

    // Draw grid lines in plot area
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 1.0);
    cairo_set_line_width(cr, 1.0);

    int num_h_lines = 10;
    int num_v_lines = 10;

    // Horizontal grid lines (dB levels)
    for (int i = 0; i <= num_h_lines; i++) {
        double y = plot_y + (double)i / num_h_lines * plot_height;
        cairo_move_to(cr, plot_x, y);
        cairo_line_to(cr, plot_x + plot_width, y);
    }

    // Vertical grid lines
    for (int i = 0; i <= num_v_lines; i++) {
        double x = plot_x + (double)i / num_v_lines * plot_width;
        cairo_move_to(cr, x, plot_y);
        cairo_line_to(cr, x, plot_y + plot_height);
    }
    cairo_stroke(cr);

    // Draw dB labels in left margin (right-justified)
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 1.0);
    for (int i = 0; i <= num_h_lines; i++) {
        double y = plot_y + (double)i / num_h_lines * plot_height;
        float db = self->max_db - (float)i / num_h_lines * range;
        char label[16];
        snprintf(label, sizeof(label), "%+.0f", db);
        cairo_text_extents_t extents;
        cairo_text_extents(cr, label, &extents);
        cairo_move_to(cr, MARGIN_LEFT - extents.width - 5, y + 4);
        cairo_show_text(cr, label);
    }

    // Draw frequency labels in bottom margin (adjusted for zoom and pan)
    if (self->sample_rate > 0) {
        double freq_span = (double)self->sample_rate / self->zoom_level;
        // Calculate pan offset in Hz
        double hz_per_bin = (double)self->sample_rate / FFT_SIZE;
        double pan_hz = self->pan_offset * hz_per_bin;
        double freq_start = self->center_freq_hz - freq_span / 2 + pan_hz;

        for (int i = 0; i <= num_v_lines; i++) {
            double x = plot_x + (double)i / num_v_lines * plot_width;
            double freq = freq_start + (double)i / num_v_lines * freq_span;

            char label[32];
            if (fabs(freq) >= 1e6) {
                snprintf(label, sizeof(label), "%.3f", freq / 1e6);
            } else if (fabs(freq) >= 1e3) {
                snprintf(label, sizeof(label), "%.1fk", freq / 1e3);
            } else {
                snprintf(label, sizeof(label), "%.0f", freq);
            }

            // Center label under tick
            cairo_text_extents_t extents;
            cairo_text_extents(cr, label, &extents);
            cairo_move_to(cr, x - extents.width / 2, height - 5);
            cairo_show_text(cr, label);
        }
    }

    // Draw spectrum in plot area (with zoom and pan support)
    if (self->spectrum_db && self->spectrum_size > 0) {
        int visible_bins, start_bin;
        calc_visible_range(self->spectrum_size, self->zoom_level, self->pan_offset,
                           &start_bin, &visible_bins);
        int end_bin = start_bin + visible_bins;

        // Build spectrum path once, then fill and stroke
        double inv_range = 1.0 / range;
        double x_scale = (double)plot_width / (visible_bins - 1);

        cairo_new_path(cr);
        cairo_move_to(cr, plot_x, plot_y + plot_height);

        for (int i = start_bin; i < end_bin; i++) {
            double x = plot_x + (double)(i - start_bin) * x_scale;
            float db = self->spectrum_db[i];
            if (db < self->min_db) db = self->min_db;
            if (db > self->max_db) db = self->max_db;
            double y = plot_y + (1.0 - (db - self->min_db) * inv_range) * plot_height;
            cairo_line_to(cr, x, y);
        }

        // Close fill path along bottom edge
        cairo_line_to(cr, plot_x + plot_width, plot_y + plot_height);
        cairo_close_path(cr);

        // Fill with transparent cyan, preserving path
        cairo_set_source_rgba(cr, 0.0, 1.0, 1.0, 0.2);
        cairo_fill_preserve(cr);

        // Stroke the spectrum trace in solid cyan
        cairo_set_source_rgb(cr, 0.0, 1.0, 1.0);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Draw red center frequency marker line or arrow (hidden when demod connected)
        if (!self->hide_center_marker) {
        int center_bin = self->spectrum_size / 2;
        cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);

        if (center_bin >= start_bin && center_bin < end_bin) {
            // Center is visible - draw vertical line
            double marker_x = plot_x + (double)(center_bin - start_bin) / (visible_bins - 1) * plot_width;
            cairo_set_line_width(cr, 2.0);
            cairo_move_to(cr, marker_x, plot_y);
            cairo_line_to(cr, marker_x, plot_y + plot_height);
            cairo_stroke(cr);
        } else {
            // Center is off-screen - draw arrow pointing toward it
            double arrow_size = 12.0;
            double arrow_y = plot_y + plot_height / 2;

            if (center_bin < start_bin) {
                // Tuned frequency is to the left - draw left arrow
                double arrow_x = plot_x + 5;
                cairo_move_to(cr, arrow_x + arrow_size, arrow_y - arrow_size / 2);
                cairo_line_to(cr, arrow_x, arrow_y);
                cairo_line_to(cr, arrow_x + arrow_size, arrow_y + arrow_size / 2);
                cairo_close_path(cr);
                cairo_fill(cr);
            } else {
                // Tuned frequency is to the right - draw right arrow
                double arrow_x = plot_x + plot_width - 5;
                cairo_move_to(cr, arrow_x - arrow_size, arrow_y - arrow_size / 2);
                cairo_line_to(cr, arrow_x, arrow_y);
                cairo_line_to(cr, arrow_x - arrow_size, arrow_y + arrow_size / 2);
                cairo_close_path(cr);
                cairo_fill(cr);
            }
        }
        } // !hide_center_marker
    }

    // Draw overlay (frequency and mode) with transparent background - centered in plot area
    if (self->overlay_freq[0] != '\0' || self->overlay_mode[0] != '\0') {
        char overlay_text[100];
        snprintf(overlay_text, sizeof(overlay_text), "%s  %s",
                 self->overlay_freq[0] ? self->overlay_freq : "",
                 self->overlay_mode[0] ? self->overlay_mode : "");

        // Set up font
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 18);

        // Measure text
        cairo_text_extents_t extents;
        cairo_text_extents(cr, overlay_text, &extents);

        // Position centered in plot area, at top
        double text_x = plot_x + (plot_width - extents.width) / 2;
        double padding = 4;
        double text_y = plot_y + extents.height + padding * 2;

        // Draw semi-transparent background
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
        cairo_rectangle(cr,
                        text_x - padding,
                        text_y - extents.height - padding,
                        extents.width + padding * 2,
                        extents.height + padding * 2);
        cairo_fill(cr);

        // Draw text in bright cyan
        cairo_set_source_rgba(cr, 0.0, 1.0, 1.0, 1.0);
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, overlay_text);
    }

    g_mutex_unlock(&self->data_mutex);
}

static void spectrum_widget_finalize(GObject *object) {
    SpectrumWidget *self = SPECTRUM_WIDGET(object);

    g_mutex_clear(&self->data_mutex);
    g_free(self->spectrum_db);

    G_OBJECT_CLASS(spectrum_widget_parent_class)->finalize(object);
}

static void spectrum_widget_class_init(SpectrumWidgetClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = spectrum_widget_finalize;
}

static void spectrum_widget_init(SpectrumWidget *self) {
    g_mutex_init(&self->data_mutex);
    self->spectrum_db = NULL;
    self->spectrum_size = 0;
    self->min_db = -120.0f;
    self->max_db = 0.0f;
    self->center_freq_hz = 14200000;
    self->sample_rate = DEFAULT_SAMPLE_RATE;
    self->zoom_level = 1;
    self->pan_offset = 0;
    self->overlay_freq[0] = '\0';
    self->overlay_mode[0] = '\0';
    self->bandplan = NULL;

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self), spectrum_widget_draw, NULL, NULL);
}

GtkWidget *spectrum_widget_new(void) {
    return g_object_new(SPECTRUM_TYPE_WIDGET, NULL);
}

void spectrum_widget_update(SpectrumWidget *widget, const float *spectrum_db, int size) {
    if (!widget || !spectrum_db || size <= 0) return;

    g_mutex_lock(&widget->data_mutex);

    // Reallocate if size changed
    if (widget->spectrum_size != size) {
        g_free(widget->spectrum_db);
        widget->spectrum_db = g_malloc(sizeof(float) * size);
        widget->spectrum_size = size;
    }

    memcpy(widget->spectrum_db, spectrum_db, sizeof(float) * size);

    g_mutex_unlock(&widget->data_mutex);

    // Request redraw
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

void spectrum_widget_set_range(SpectrumWidget *widget, float min_db, float max_db) {
    if (!widget) return;
    widget->min_db = min_db;
    widget->max_db = max_db;
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

void spectrum_widget_set_center_freq(SpectrumWidget *widget, int freq_hz) {
    if (!widget) return;
    widget->center_freq_hz = freq_hz;
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

void spectrum_widget_set_sample_rate(SpectrumWidget *widget, int sample_rate) {
    if (!widget) return;
    widget->sample_rate = sample_rate;
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

void spectrum_widget_set_overlay(SpectrumWidget *widget, const char *freq_str, const char *mode_str) {
    if (!widget) return;

    g_mutex_lock(&widget->data_mutex);

    if (freq_str) {
        snprintf(widget->overlay_freq, sizeof(widget->overlay_freq), "%s", freq_str);
    } else {
        widget->overlay_freq[0] = '\0';
    }

    if (mode_str) {
        snprintf(widget->overlay_mode, sizeof(widget->overlay_mode), "%s", mode_str);
    } else {
        widget->overlay_mode[0] = '\0';
    }

    g_mutex_unlock(&widget->data_mutex);

    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

void spectrum_widget_set_zoom(SpectrumWidget *widget, int zoom_level) {
    if (!widget) return;
    // Clamp to valid zoom levels
    if (zoom_level < 1) zoom_level = 1;
    if (zoom_level > 16) zoom_level = 16;
    widget->zoom_level = zoom_level;
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

int spectrum_widget_get_zoom(SpectrumWidget *widget) {
    if (!widget) return 1;
    return widget->zoom_level;
}

void spectrum_widget_set_pan(SpectrumWidget *widget, int pan_offset) {
    if (!widget) return;
    widget->pan_offset = pan_offset;
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

int spectrum_widget_get_pan(SpectrumWidget *widget) {
    if (!widget) return 0;
    return widget->pan_offset;
}

void spectrum_widget_set_hide_center_marker(SpectrumWidget *widget, gboolean hide) {
    if (!widget) return;
    widget->hide_center_marker = hide;
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

void spectrum_widget_set_bandplan(SpectrumWidget *widget, const bandplan_t *plan) {
    if (!widget) return;
    widget->bandplan = plan;
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}
