#include "waterfall_widget.h"
#include "usb_device.h"  // For elad_mode_t
#include <string.h>
#include <time.h>

// Must match spectrum_widget.c margins
#define MARGIN_LEFT 55

struct _WaterfallWidget {
    GtkDrawingArea parent_instance;

    GMutex data_mutex;
    int spectrum_size;
    int num_lines;
    int current_line;  // Index of next line to write

    // Cairo surface for direct rendering
    cairo_surface_t *surface;
    int surface_width;
    int surface_height;

    // Display parameters
    float min_db;
    float max_db;
    int zoom_level;  // 1, 2, 4, 8 = horizontal zoom factor
    int pan_offset;  // Bin offset from center (only effective when zoom > 1)

    // Bandwidth display
    int bandwidth_hz;       // Filter bandwidth in Hz
    int current_mode;       // elad_mode_t value
    int sample_rate;        // Sample rate for Hz to bin conversion
    int center_offset_hz;   // Offset from tuned freq (e.g., +1500 for data modes)
    int is_resonator;       // CW resonator mode (100&1, etc.) - draws orange

    // Demodulator bandwidth display (from SWLDemodTool via DM command)
    int demod_bandwidth_hz; // 0 = inactive/hidden
};

G_DEFINE_TYPE(WaterfallWidget, waterfall_widget, GTK_TYPE_DRAWING_AREA)

// Color map: converts dB value (0.0 to 1.0 normalized) to RGB
static void db_to_color(float normalized, uint8_t *r, uint8_t *g, uint8_t *b) {
    // Color gradient: black -> blue -> cyan -> green -> yellow -> red -> white
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    if (normalized < 0.2f) {
        // Black to blue
        float t = normalized / 0.2f;
        *r = 0;
        *g = 0;
        *b = (uint8_t)(t * 255);
    } else if (normalized < 0.4f) {
        // Blue to cyan
        float t = (normalized - 0.2f) / 0.2f;
        *r = 0;
        *g = (uint8_t)(t * 255);
        *b = 255;
    } else if (normalized < 0.6f) {
        // Cyan to green
        float t = (normalized - 0.4f) / 0.2f;
        *r = 0;
        *g = 255;
        *b = (uint8_t)((1.0f - t) * 255);
    } else if (normalized < 0.8f) {
        // Green to yellow
        float t = (normalized - 0.6f) / 0.2f;
        *r = (uint8_t)(t * 255);
        *g = 255;
        *b = 0;
    } else {
        // Yellow to red
        float t = (normalized - 0.8f) / 0.2f;
        *r = 255;
        *g = (uint8_t)((1.0f - t) * 255);
        *b = 0;
    }
}

// Lines per second: 192000 / 4096 / 3 = 15.625
#define LINES_PER_SECOND 15.625f

static void waterfall_widget_draw(GtkDrawingArea *area, cairo_t *cr,
                                   int width, int height, gpointer user_data G_GNUC_UNUSED) {
    WaterfallWidget *self = WATERFALL_WIDGET(area);

    // Black background
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    // Calculate plot area (matching spectrum widget)
    int plot_width = width - MARGIN_LEFT;
    if (plot_width <= 0 || height <= 0) return;

    g_mutex_lock(&self->data_mutex);

    // Check if we need to recreate surface (size changed)
    if (self->surface && (self->surface_width != plot_width || self->surface_height != height)) {
        cairo_surface_destroy(self->surface);
        self->surface = NULL;
    }

    // Create surface if needed (only for plot area)
    if (!self->surface && plot_width > 0 && height > 0) {
        self->surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, plot_width, height);
        self->surface_width = plot_width;
        self->surface_height = height;
        // Clear to black
        cairo_t *surface_cr = cairo_create(self->surface);
        cairo_set_source_rgb(surface_cr, 0, 0, 0);
        cairo_paint(surface_cr);
        cairo_destroy(surface_cr);
    }

    // Draw the surface at margin offset
    if (self->surface) {
        cairo_set_source_surface(cr, self->surface, MARGIN_LEFT, 0);
        cairo_paint(cr);
    }

    // Calculate visible bin range (same as add_line) for bandwidth lines
    int visible_bins = self->spectrum_size / self->zoom_level;
    int max_pan = (self->spectrum_size - visible_bins) / 2;
    int clamped_pan = self->pan_offset;
    if (clamped_pan < -max_pan) clamped_pan = -max_pan;
    if (clamped_pan > max_pan) clamped_pan = max_pan;
    int start_bin = (self->spectrum_size - visible_bins) / 2 + clamped_pan;

    g_mutex_unlock(&self->data_mutex);

    // Draw bandwidth lines (red dashed) if bandwidth is set
    if (self->bandwidth_hz > 0 && self->sample_rate > 0 && self->spectrum_size > 0) {
        int center_bin = self->spectrum_size / 2;
        // Apply center offset (e.g., +1500 Hz for data modes)
        int offset_bins = (self->center_offset_hz * self->spectrum_size) / self->sample_rate;
        center_bin += offset_bins;
        int bw_bins = (self->bandwidth_hz * self->spectrum_size) / self->sample_rate;

        // Set up dashed line style (orange for resonator, red otherwise)
        if (self->is_resonator) {
            cairo_set_source_rgb(cr, 1.0, 0.5, 0.0);  // Orange
        } else {
            cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);  // Red
        }
        cairo_set_line_width(cr, 1.5);
        double dashes[] = {6.0, 4.0};
        cairo_set_dash(cr, dashes, 2, 0);

        // Calculate line positions based on mode
        int line_bins[2] = {-1, -1};  // Up to 2 lines
        int num_lines = 0;

        // Data modes (with offset) are always symmetric around the offset center
        if (self->center_offset_hz != 0) {
            // Symmetric around offset center (e.g., D300, D600, D1k)
            line_bins[0] = center_bin - bw_bins / 2;
            line_bins[1] = center_bin + bw_bins / 2;
            num_lines = 2;
        } else {
            switch (self->current_mode) {
                case ELAD_MODE_USB:
                    // USB: signal is above carrier - one line at upper edge
                    line_bins[0] = center_bin + bw_bins;
                    num_lines = 1;
                    break;
                case ELAD_MODE_LSB:
                    // LSB: signal is below carrier - one line at lower edge
                    line_bins[0] = center_bin - bw_bins;
                    num_lines = 1;
                    break;
                case ELAD_MODE_CW:
                case ELAD_MODE_CWR:
                case ELAD_MODE_AM:
                case ELAD_MODE_FM:
                default:
                    // Symmetric modes: two lines around center
                    line_bins[0] = center_bin - bw_bins / 2;
                    line_bins[1] = center_bin + bw_bins / 2;
                    num_lines = 2;
                    break;
            }
        }

        // Draw each bandwidth line
        for (int i = 0; i < num_lines; i++) {
            int bin = line_bins[i];
            if (bin < 0) continue;

            // Check if bin is visible
            if (bin >= start_bin && bin < start_bin + visible_bins) {
                // Calculate x position in plot area
                double x = MARGIN_LEFT + (double)(bin - start_bin) / visible_bins * plot_width;
                cairo_move_to(cr, x, 0);
                cairo_line_to(cr, x, height);
                cairo_stroke(cr);
            }
        }

        // Reset dash pattern
        cairo_set_dash(cr, NULL, 0, 0);
    }

    // Draw demodulator bandwidth lines (yellow dashed) if active
    if (self->demod_bandwidth_hz > 0 && self->sample_rate > 0 && self->spectrum_size > 0) {
        int center_bin = self->spectrum_size / 2;
        int bw_bins = (self->demod_bandwidth_hz * self->spectrum_size) / self->sample_rate;

        cairo_set_source_rgb(cr, 1.0, 1.0, 0.0);  // Yellow
        cairo_set_line_width(cr, 1.5);
        double dashes[] = {6.0, 4.0};
        cairo_set_dash(cr, dashes, 2, 0);

        // Always 2 symmetric lines around center
        int line_bins[2] = { center_bin - bw_bins / 2, center_bin + bw_bins / 2 };
        for (int i = 0; i < 2; i++) {
            int bin = line_bins[i];
            if (bin >= start_bin && bin < start_bin + visible_bins) {
                double x = MARGIN_LEFT + (double)(bin - start_bin) / visible_bins * plot_width;
                cairo_move_to(cr, x, 0);
                cairo_line_to(cr, x, height);
                cairo_stroke(cr);
            }
        }

        cairo_set_dash(cr, NULL, 0, 0);
    }

    // Draw time labels in left margin
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 1.0);

    // Calculate total time span visible
    float total_seconds = height / LINES_PER_SECOND;

    // Draw time labels at regular intervals (right-justified)
    int num_labels = 5;
    for (int i = 0; i <= num_labels; i++) {
        double y = (double)i / num_labels * height;
        float seconds = (float)i / num_labels * total_seconds;

        char label[16];
        if (seconds < 60) {
            snprintf(label, sizeof(label), "%.0fs", seconds);
        } else {
            int mins = (int)(seconds / 60);
            int secs = (int)seconds % 60;
            snprintf(label, sizeof(label), "%d:%02d", mins, secs);
        }
        cairo_text_extents_t extents;
        cairo_text_extents(cr, label, &extents);
        cairo_move_to(cr, MARGIN_LEFT - extents.width - 5, y + 4);
        cairo_show_text(cr, label);
    }

    // Draw local and UTC time at top of waterfall
    time_t now = time(NULL);
    struct tm local_tm, utc_tm;
    char local_time[16], utc_time[16];

    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
    strftime(local_time, sizeof(local_time), "LOCAL %H:%M:%S", &local_tm);
    strftime(utc_time, sizeof(utc_time), "UTC %H:%M:%S", &utc_tm);

    cairo_set_source_rgba(cr, 0.0, 1.0, 1.0, 1.0);  // Cyan
    cairo_set_font_size(cr, 16);

    // Local time - top left
    cairo_move_to(cr, MARGIN_LEFT + 5, 18);
    cairo_show_text(cr, local_time);

    // UTC time - top right (right-aligned)
    cairo_text_extents_t time_extents;
    cairo_text_extents(cr, utc_time, &time_extents);
    cairo_move_to(cr, width - time_extents.width - 5, 18);
    cairo_show_text(cr, utc_time);
}

static void waterfall_widget_finalize(GObject *object) {
    WaterfallWidget *self = WATERFALL_WIDGET(object);

    g_mutex_clear(&self->data_mutex);
    if (self->surface) {
        cairo_surface_destroy(self->surface);
    }

    G_OBJECT_CLASS(waterfall_widget_parent_class)->finalize(object);
}

static void waterfall_widget_class_init(WaterfallWidgetClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = waterfall_widget_finalize;
}

static void waterfall_widget_init(WaterfallWidget *self) {
    g_mutex_init(&self->data_mutex);
    self->spectrum_size = 0;
    self->num_lines = WATERFALL_LINES;
    self->current_line = 0;
    self->surface = NULL;
    self->surface_width = 0;
    self->surface_height = 0;
    self->min_db = -120.0f;
    self->max_db = 0.0f;
    self->zoom_level = 1;
    self->pan_offset = 0;
    self->bandwidth_hz = 0;
    self->current_mode = ELAD_MODE_UNKNOWN;
    self->sample_rate = DEFAULT_SAMPLE_RATE;
    self->center_offset_hz = 0;
    self->is_resonator = 0;
    self->demod_bandwidth_hz = 0;

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self), waterfall_widget_draw, NULL, NULL);
}

GtkWidget *waterfall_widget_new(void) {
    return g_object_new(WATERFALL_TYPE_WIDGET, NULL);
}

void waterfall_widget_add_line(WaterfallWidget *widget, const float *spectrum_db, int size) {
    if (!widget || !spectrum_db || size <= 0) return;

    g_mutex_lock(&widget->data_mutex);

    widget->spectrum_size = size;

    // If we have a surface, scroll it down and draw new line at top using direct pixel access
    if (widget->surface) {
        int width = widget->surface_width;
        int height = widget->surface_height;

        // Flush any pending Cairo operations
        cairo_surface_flush(widget->surface);

        // Get direct access to pixel data
        unsigned char *data = cairo_image_surface_get_data(widget->surface);
        int stride = cairo_image_surface_get_stride(widget->surface);

        // Scroll down: move all rows down by 1 (from bottom to top to avoid overlap)
        // memmove handles overlapping regions correctly
        if (height > 1) {
            memmove(data + stride, data, stride * (height - 1));
        }

        // Draw new line at top (row 0) - RGB24 format is 0xXXRRGGBB (little-endian: BB GG RR XX)
        float range = widget->max_db - widget->min_db;
        if (range < 1.0f) range = 1.0f;

        // Calculate visible bin range based on zoom level and pan offset
        int visible_bins = size / widget->zoom_level;
        int max_pan = (size - visible_bins) / 2;
        int clamped_pan = widget->pan_offset;
        if (clamped_pan < -max_pan) clamped_pan = -max_pan;
        if (clamped_pan > max_pan) clamped_pan = max_pan;
        int start_bin = (size - visible_bins) / 2 + clamped_pan;

        uint32_t *row = (uint32_t *)data;
        for (int x = 0; x < width; x++) {
            // Map x to spectrum bin (within zoomed range)
            int bin = start_bin + x * visible_bins / width;
            if (bin >= start_bin + visible_bins) bin = start_bin + visible_bins - 1;

            float db = spectrum_db[bin];
            if (db < widget->min_db) db = widget->min_db;
            if (db > widget->max_db) db = widget->max_db;

            float normalized = (db - widget->min_db) / range;

            uint8_t r, g, b;
            db_to_color(normalized, &r, &g, &b);

            // RGB24 format: 0x00RRGGBB
            row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }

        // Mark surface as modified
        cairo_surface_mark_dirty(widget->surface);
    }

    g_mutex_unlock(&widget->data_mutex);

    // Request redraw
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

void waterfall_widget_set_range(WaterfallWidget *widget, float min_db, float max_db) {
    if (!widget) return;
    widget->min_db = min_db;
    widget->max_db = max_db;
}

void waterfall_widget_clear(WaterfallWidget *widget) {
    if (!widget) return;

    g_mutex_lock(&widget->data_mutex);
    if (widget->surface) {
        cairo_t *cr = cairo_create(widget->surface);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
    }
    widget->current_line = 0;
    g_mutex_unlock(&widget->data_mutex);

    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

void waterfall_widget_set_zoom(WaterfallWidget *widget, int zoom_level) {
    if (!widget) return;
    // Clamp to valid zoom levels
    if (zoom_level < 1) zoom_level = 1;
    if (zoom_level > 16) zoom_level = 16;

    if (widget->zoom_level != zoom_level) {
        widget->zoom_level = zoom_level;
        // Clear waterfall when zoom changes for clean display
        waterfall_widget_clear(widget);
    }
}

int waterfall_widget_get_zoom(WaterfallWidget *widget) {
    if (!widget) return 1;
    return widget->zoom_level;
}

void waterfall_widget_set_pan(WaterfallWidget *widget, int pan_offset) {
    if (!widget) return;
    if (widget->pan_offset != pan_offset) {
        widget->pan_offset = pan_offset;
        // Clear waterfall when pan changes for clean display
        waterfall_widget_clear(widget);
    }
}

int waterfall_widget_get_pan(WaterfallWidget *widget) {
    if (!widget) return 0;
    return widget->pan_offset;
}

void waterfall_widget_set_bandwidth(WaterfallWidget *widget, int bandwidth_hz, int mode, int center_offset_hz, int is_resonator) {
    if (!widget) return;
    widget->bandwidth_hz = bandwidth_hz;
    widget->current_mode = mode;
    widget->center_offset_hz = center_offset_hz;
    widget->is_resonator = is_resonator;
}

void waterfall_widget_set_demod_bandwidth(WaterfallWidget *widget, int bandwidth_hz) {
    if (!widget) return;
    widget->demod_bandwidth_hz = bandwidth_hz;
}

void waterfall_widget_set_sample_rate(WaterfallWidget *widget, int sample_rate) {
    if (!widget) return;
    widget->sample_rate = sample_rate;
}
