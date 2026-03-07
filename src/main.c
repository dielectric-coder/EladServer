#define _DEFAULT_SOURCE
#include <gtk/gtk.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_state.h"
#include "usb_device.h"
#include "fft_processor.h"
#include "spectrum_widget.h"
#include "waterfall_widget.h"
#include "cat_control.h"
#include "cat_server.h"
#include "settings.h"
#include "bandplan.h"
#ifdef HAVE_GPIOD
#include "rotary_encoder.h"
#endif

// Active parameter for encoder 1
typedef enum {
    PARAM_SPECTRUM_REF = 0,
    PARAM_SPECTRUM_RANGE = 1,
    PARAM_WATERFALL_REF = 2,
    PARAM_WATERFALL_RANGE = 3,
    PARAM_COUNT = 4
} active_param_t;

// Encoder 2 mode (zoom vs pan)
typedef enum {
    ENCODER2_MODE_ZOOM = 0,
    ENCODER2_MODE_PAN = 1
} encoder2_mode_t;

// Application state
typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *spectrum;
    GtkWidget *spectrum_frame;
    GtkWidget *waterfall;
    GtkWidget *status_icon;  // Actually a label with colored circle
    GtkAdjustment *ref_adj;           // Spectrum reference level
    GtkAdjustment *range_adj;         // Spectrum dynamic range
    GtkAdjustment *waterfall_ref_adj;   // Waterfall reference level
    GtkAdjustment *waterfall_range_adj; // Waterfall dynamic range

    usb_device_t *usb;
    fft_processor_t *fft;
    cat_control_t *cat;
    cat_server_t *cat_server;
    pthread_mutex_t cat_mutex;
#ifdef HAVE_GPIOD
    rotary_encoder_t *encoder1;       // Parameter control encoder
    rotary_encoder_t *encoder2;       // Zoom/pan control encoder
    GtkWidget *param_label;           // Shows all parameter values
    GtkWidget *zoom_label;            // Shows zoom level and mode
    active_param_t active_param;      // Current parameter selection
    encoder2_mode_t encoder2_mode;    // Zoom or pan mode
    int zoom_level;                   // 1, 2, 4
    int pan_offset;                   // Shared pan for spectrum/waterfall
#endif

    pthread_t usb_thread;
    atomic_int running;
    atomic_int usb_connected;

    // Latest spectrum for display update
    GMutex spectrum_mutex;
    float spectrum_db[FFT_SIZE];
    atomic_int spectrum_ready;

    int center_freq_hz;
    elad_mode_t current_mode;
    int current_vfo;  // 0=VFO A, 1=VFO B
    char current_filter[16];  // Filter bandwidth string
    int freq_poll_counter;

    // Command-line options
    gboolean fullscreen;
    gboolean pi_mode;
    int window_width;
    int window_height;
    int cat_server_port;
    char cat_listen_addr[16];  // "any" or empty for localhost

    // Settings auto-save
    guint save_timeout_id;

    // Band overlay
    bandplan_t bandplan;
} app_data_t;

static app_data_t app;

// Parse bandwidth string to Hz and center offset
// Handles formats: "2.4k", "500", "Narrow", "Wide", "D300", "D600", "D1k", "100&1"
// offset_hz: output for center frequency offset (e.g., +1500 for data modes)
// is_resonator: output flag for CW resonator modes (100&1, 100&2, etc.)
static int parse_bandwidth_hz(const char *bw_str, int *offset_hz, int *is_resonator) {
    if (offset_hz) *offset_hz = 0;
    if (is_resonator) *is_resonator = 0;
    if (!bw_str || bw_str[0] == '\0') return 0;

    // Handle special FM bandwidth strings
    if (strcasecmp(bw_str, "Narrow") == 0) return 2500;
    if (strcasecmp(bw_str, "Wide") == 0) return 6000;

    // Handle CW resonator modes: "100&1", "100&2", "100&3", "100&4"
    if (strncmp(bw_str, "100&", 4) == 0) {
        if (is_resonator) *is_resonator = 1;
        return 100;
    }

    // Check for "D" prefix (data mode filters centered at +1500 Hz)
    const char *num_start = bw_str;
    if (bw_str[0] == 'D' || bw_str[0] == 'd') {
        num_start = bw_str + 1;
        if (offset_hz) *offset_hz = 1500;
    }

    // Parse numeric values with optional 'k' suffix
    char *endptr;
    double value = strtod(num_start, &endptr);
    if (endptr == num_start) return 0;  // No number found

    // Check for 'k' or 'K' suffix (kHz)
    if (*endptr == 'k' || *endptr == 'K') {
        return (int)(value * 1000);
    }

    // Assume Hz if no suffix (values like "500" for CW)
    return (int)value;
}

// USB data callback - called from USB thread
static void usb_data_callback(const uint8_t *data, int length, void *user_data) {
    app_data_t *app_data = (app_data_t *)user_data;
    // Process data through FFT
    if (fft_processor_process(app_data->fft, data, length)) {
        // New spectrum ready - copy to shared buffer
        g_mutex_lock(&app_data->spectrum_mutex);
        fft_processor_get_spectrum_db(app_data->fft, app_data->spectrum_db);
        atomic_store(&app_data->spectrum_ready, 1);
        g_mutex_unlock(&app_data->spectrum_mutex);
    }
}

// USB thread function
static void *usb_thread_func(void *user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    fprintf(stderr, "USB thread started\n");

    while (atomic_load(&app_data->running)) {
        // Check for disconnection (detected via transfer errors)
        if (usb_device_is_open(app_data->usb) && usb_device_check_disconnected(app_data->usb)) {
            fprintf(stderr, "USB device disconnected, closing...\n");
            usb_device_close(app_data->usb);
            atomic_store(&app_data->usb_connected, 0);
            // Also close CAT - serial port will be invalid
            pthread_mutex_lock(&app_data->cat_mutex);
            cat_control_close(app_data->cat);
            pthread_mutex_unlock(&app_data->cat_mutex);
            usleep(500000);  // Wait 500ms before attempting reconnect
            continue;
        }

        if (!usb_device_is_open(app_data->usb)) {
            // Try to open device
            if (usb_device_open(app_data->usb) == 0) {
                // Give device time to fully initialize after power-on
                // The FPGA and USB controller need time to stabilize
                fprintf(stderr, "Waiting for device to stabilize (3s)...\n");
                usleep(3000000);  // 3 seconds

                atomic_store(&app_data->usb_connected, 1);
                fprintf(stderr, "USB device connected\n");

                // Try to reopen CAT serial port (it may have been recreated)
                pthread_mutex_lock(&app_data->cat_mutex);
                if (!cat_control_is_open(app_data->cat)) {
                    cat_control_open(app_data->cat, "/dev/ttyUSB0");
                }
                pthread_mutex_unlock(&app_data->cat_mutex);

                // Read current frequency from radio (don't change it)
                long freq = usb_device_get_frequency(app_data->usb);
                if (freq > 0 && freq < 100000000) {  // Sanity check: < 100 MHz
                    app_data->center_freq_hz = (int)freq;
                    fprintf(stderr, "Radio frequency: %ld Hz\n", freq);
                } else {
                    fprintf(stderr, "Radio frequency invalid: %ld Hz (using previous)\n", freq);
                }

                // Start streaming
                if (usb_device_start_streaming(app_data->usb, usb_data_callback, app_data) != 0) {
                    fprintf(stderr, "Failed to start streaming\n");
                    usb_device_close(app_data->usb);
                    atomic_store(&app_data->usb_connected, 0);
                }
            } else {
                // Device not found, wait and retry
                usleep(1000000);  // 1 second
                continue;
            }
        }

        // Handle USB events (with timeout to allow disconnect check)
        int res = usb_device_handle_events(app_data->usb);
        if (res < 0) {
            fprintf(stderr, "USB error: %d\n", res);
            usb_device_close(app_data->usb);
            atomic_store(&app_data->usb_connected, 0);
        }
    }

    // Cleanup
    usb_device_stop_streaming(app_data->usb);
    usb_device_close(app_data->usb);

    fprintf(stderr, "USB thread stopped\n");
    return NULL;
}

// Display refresh timer callback - called from GTK main thread
static gboolean refresh_display(gpointer user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    if (!atomic_load(&app_data->running)) {
        return G_SOURCE_REMOVE;
    }

    // Update status
    if (atomic_load(&app_data->usb_connected)) {
        gtk_label_set_text(GTK_LABEL(app_data->status_icon), "●");
        gtk_widget_remove_css_class(GTK_WIDGET(app_data->status_icon), "disconnected");
        gtk_widget_add_css_class(GTK_WIDGET(app_data->status_icon), "connected");
    } else {
        gtk_label_set_text(GTK_LABEL(app_data->status_icon), "○");
        gtk_widget_remove_css_class(GTK_WIDGET(app_data->status_icon), "connected");
        gtk_widget_add_css_class(GTK_WIDGET(app_data->status_icon), "disconnected");
    }

    // Poll frequency and mode from radio every ~10 frames (~300ms)
    app_data->freq_poll_counter++;
    if (app_data->freq_poll_counter >= 10 && atomic_load(&app_data->usb_connected)) {
        app_data->freq_poll_counter = 0;

        // Try to lock CAT mutex (don't block GTK main thread if server is using it)
        if (pthread_mutex_trylock(&app_data->cat_mutex) == 0) {
            // Read frequency, mode and VFO via CAT serial port
            long freq;
            elad_mode_t mode;
            int vfo;
            if (cat_control_is_open(app_data->cat) &&
                cat_control_get_freq_mode(app_data->cat, &freq, &mode, &vfo) == 0) {

                gboolean freq_changed = (freq > 0 && freq != app_data->center_freq_hz);
                gboolean mode_changed = (mode != app_data->current_mode);
                gboolean vfo_changed = (vfo != app_data->current_vfo);

                if (freq_changed) {
                    app_data->center_freq_hz = (int)freq;

                    // Update spectrum display
                    spectrum_widget_set_center_freq(SPECTRUM_WIDGET(app_data->spectrum), app_data->center_freq_hz);
                }

                if (mode_changed) {
                    app_data->current_mode = mode;
                }

                if (vfo_changed) {
                    app_data->current_vfo = vfo;

                    // Update spectrum frame label
                    gtk_frame_set_label(GTK_FRAME(app_data->spectrum_frame),
                                        vfo == 0 ? "VFO A" : "VFO B");
                }

                // Read filter bandwidth (may have changed even if mode didn't)
                char filter_str[16] = "";
                gboolean filter_changed = FALSE;
                if (cat_control_get_filter_bw(app_data->cat, app_data->current_mode,
                                              filter_str, sizeof(filter_str)) == 0) {
                    if (strcmp(filter_str, app_data->current_filter) != 0) {
                        strncpy(app_data->current_filter, filter_str, sizeof(app_data->current_filter) - 1);
                        app_data->current_filter[sizeof(app_data->current_filter) - 1] = '\0';
                        filter_changed = TRUE;
                    }
                }

                // Update overlay with frequency, mode and filter
                if (freq_changed || mode_changed || filter_changed) {
                    char freq_str[32];
                    char mode_filter_str[32];
                    snprintf(freq_str, sizeof(freq_str), "%.6f MHz", app_data->center_freq_hz / 1e6);
                    snprintf(mode_filter_str, sizeof(mode_filter_str), "%s %s",
                             usb_device_mode_name(app_data->current_mode),
                             app_data->current_filter);
                    spectrum_widget_set_overlay(SPECTRUM_WIDGET(app_data->spectrum),
                                                freq_str, mode_filter_str);

                    // Update waterfall bandwidth lines
                    int offset_hz = 0;
                    int is_resonator = 0;
                    int bw_hz = parse_bandwidth_hz(app_data->current_filter, &offset_hz, &is_resonator);
                    waterfall_widget_set_bandwidth(WATERFALL_WIDGET(app_data->waterfall),
                                                   bw_hz, app_data->current_mode, offset_hz, is_resonator);
                }
            }
            pthread_mutex_unlock(&app_data->cat_mutex);
        }
    }

    // Check if new spectrum data is available
    if (atomic_exchange(&app_data->spectrum_ready, 0)) {
        float spectrum_copy[FFT_SIZE];

        g_mutex_lock(&app_data->spectrum_mutex);
        memcpy(spectrum_copy, app_data->spectrum_db, sizeof(spectrum_copy));
        g_mutex_unlock(&app_data->spectrum_mutex);

        // Update display widgets
        spectrum_widget_update(SPECTRUM_WIDGET(app_data->spectrum), spectrum_copy, FFT_SIZE);
        waterfall_widget_add_line(WATERFALL_WIDGET(app_data->waterfall), spectrum_copy, FFT_SIZE);
    }

    return G_SOURCE_CONTINUE;
}

// Auto-save settings after 3 seconds of no changes
#define SETTINGS_SAVE_DELAY_MS 3000

static gboolean save_settings_timeout(gpointer user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    app_settings_t settings;
    settings.spectrum_ref = gtk_adjustment_get_value(app_data->ref_adj);
    settings.spectrum_range = gtk_adjustment_get_value(app_data->range_adj);
    settings.waterfall_ref = gtk_adjustment_get_value(app_data->waterfall_ref_adj);
    settings.waterfall_range = gtk_adjustment_get_value(app_data->waterfall_range_adj);
#ifdef HAVE_GPIOD
    if (app_data->pi_mode) {
        settings.zoom_level = app_data->zoom_level;
        settings.pan_offset = app_data->pan_offset;
    } else {
        settings.zoom_level = 1;
        settings.pan_offset = 0;
    }
#else
    settings.zoom_level = 1;
    settings.pan_offset = 0;
#endif
    settings_save(&settings);

    app_data->save_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void schedule_settings_save(app_data_t *app_data) {
    // Cancel any pending save
    if (app_data->save_timeout_id > 0) {
        g_source_remove(app_data->save_timeout_id);
    }
    // Schedule new save after delay
    app_data->save_timeout_id = g_timeout_add(SETTINGS_SAVE_DELAY_MS, save_settings_timeout, app_data);
}

// Spectrum range changed callback
static void on_spectrum_range_changed(GtkAdjustment *adj G_GNUC_UNUSED, gpointer user_data) {
    app_data_t *app_data = (app_data_t *)user_data;
    if (!app_data->spectrum || !app_data->ref_adj || !app_data->range_adj) return;
    float ref_db = (float)gtk_adjustment_get_value(app_data->ref_adj);
    float range_db = (float)gtk_adjustment_get_value(app_data->range_adj);
    float min_db = ref_db - range_db;
    spectrum_widget_set_range(SPECTRUM_WIDGET(app_data->spectrum), min_db, ref_db);
    schedule_settings_save(app_data);
}

// Waterfall range changed callback
static void on_waterfall_range_changed(GtkAdjustment *adj G_GNUC_UNUSED, gpointer user_data) {
    app_data_t *app_data = (app_data_t *)user_data;
    if (!app_data->waterfall) return;  // Widget not created yet
    float ref_db = (float)gtk_adjustment_get_value(app_data->waterfall_ref_adj);
    float range_db = (float)gtk_adjustment_get_value(app_data->waterfall_range_adj);
    float min_db = ref_db - range_db;
    waterfall_widget_set_range(WATERFALL_WIDGET(app_data->waterfall), min_db, ref_db);
    schedule_settings_save(app_data);
}

#ifdef HAVE_GPIOD
// Get adjustment for current parameter
static GtkAdjustment *get_active_adjustment(app_data_t *app_data) {
    switch (app_data->active_param) {
        case PARAM_SPECTRUM_REF:    return app_data->ref_adj;
        case PARAM_SPECTRUM_RANGE:  return app_data->range_adj;
        case PARAM_WATERFALL_REF:   return app_data->waterfall_ref_adj;
        case PARAM_WATERFALL_RANGE: return app_data->waterfall_range_adj;
        default:                    return app_data->ref_adj;
    }
}

// Update parameter label display - shows all four values with active highlighted
static void update_param_label(app_data_t *app_data) {
    if (!app_data->param_label) return;

    // Get current values from all adjustments
    int sp_ref = (int)gtk_adjustment_get_value(app_data->ref_adj);
    int sp_rng = (int)gtk_adjustment_get_value(app_data->range_adj);
    int wf_ref = (int)gtk_adjustment_get_value(app_data->waterfall_ref_adj);
    int wf_rng = (int)gtk_adjustment_get_value(app_data->waterfall_range_adj);

    // Build markup string with active parameter in cyan, others in white
    char label[256];
    snprintf(label, sizeof(label),
             "<span foreground='%s' weight='bold'>SP.REF %d</span>  "
             "<span foreground='%s' weight='bold'>SP.RNG %d</span>  "
             "<span foreground='%s' weight='bold'>WF.REF %d</span>  "
             "<span foreground='%s' weight='bold'>WF.RNG %d</span>",
             app_data->active_param == PARAM_SPECTRUM_REF ? "cyan" : "white", sp_ref,
             app_data->active_param == PARAM_SPECTRUM_RANGE ? "cyan" : "white", sp_rng,
             app_data->active_param == PARAM_WATERFALL_REF ? "cyan" : "white", wf_ref,
             app_data->active_param == PARAM_WATERFALL_RANGE ? "cyan" : "white", wf_rng);

    gtk_label_set_markup(GTK_LABEL(app_data->param_label), label);
}

// Update zoom label display (shows span and offset in kHz)
// Active control (zoom mode = SPAN, pan mode = OFS) highlighted in cyan
static void update_zoom_label(app_data_t *app_data) {
    if (!app_data->zoom_label) return;

    // Calculate span in kHz
    int span_khz = DEFAULT_SAMPLE_RATE / app_data->zoom_level / 1000;

    // Calculate offset in kHz (pan_offset is in bins)
    int ofs_khz = (int)(app_data->pan_offset * (DEFAULT_SAMPLE_RATE / (float)FFT_SIZE) / 1000);

    // Highlight active control: SPAN in zoom mode, OFS in pan mode
    const char *span_color = (app_data->encoder2_mode == ENCODER2_MODE_ZOOM) ? "cyan" : "white";
    const char *ofs_color = (app_data->encoder2_mode == ENCODER2_MODE_PAN) ? "cyan" : "white";

    char label[128];
    snprintf(label, sizeof(label),
             "<span foreground='%s' weight='bold'>SPAN %d</span>  "
             "<span foreground='%s' weight='bold'>OFS %+d</span>",
             span_color, span_khz, ofs_color, ofs_khz);
    gtk_label_set_markup(GTK_LABEL(app_data->zoom_label), label);
}

// Encoder 1 rotation callback - adjusts active parameter
static void on_encoder1_rotation(int direction, void *user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    GtkAdjustment *adj = get_active_adjustment(app_data);
    if (!adj) return;

    double step = (app_data->active_param == PARAM_SPECTRUM_RANGE ||
                   app_data->active_param == PARAM_WATERFALL_RANGE) ? 5.0 : 1.0;

    double value = gtk_adjustment_get_value(adj);
    double new_value = value + (direction * step);

    // Clamp to adjustment bounds
    double lower = gtk_adjustment_get_lower(adj);
    double upper = gtk_adjustment_get_upper(adj);
    if (new_value < lower) new_value = lower;
    if (new_value > upper) new_value = upper;

    gtk_adjustment_set_value(adj, new_value);
    update_param_label(app_data);
}

// Encoder 1 button callback - cycles through parameters
static void on_encoder1_button(void *user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    // Cycle through 4 parameters
    app_data->active_param = (app_data->active_param + 1) % PARAM_COUNT;

    update_param_label(app_data);
}

// Encoder 2 rotation callback - zooms or pans based on mode
static void on_encoder2_rotation(int direction, void *user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    if (app_data->encoder2_mode == ENCODER2_MODE_ZOOM) {
        // Zoom mode: rotation changes zoom level
        int new_zoom = app_data->zoom_level;

        if (direction > 0) {
            // Zoom out: 16 -> 8 -> 4 -> 2 -> 1 (SPAN increases)
            if (new_zoom > 1) new_zoom /= 2;
        } else {
            // Zoom in: 1 -> 2 -> 4 -> 8 -> 16 (SPAN decreases)
            if (new_zoom < 16) new_zoom *= 2;
        }

        if (new_zoom != app_data->zoom_level) {
            app_data->zoom_level = new_zoom;

            // Reset pan when zoom changes
            app_data->pan_offset = 0;

            // Apply zoom and reset pan to both widgets
            spectrum_widget_set_zoom(SPECTRUM_WIDGET(app_data->spectrum), app_data->zoom_level);
            spectrum_widget_set_pan(SPECTRUM_WIDGET(app_data->spectrum), 0);
            waterfall_widget_set_zoom(WATERFALL_WIDGET(app_data->waterfall), app_data->zoom_level);
            waterfall_widget_set_pan(WATERFALL_WIDGET(app_data->waterfall), 0);

            update_zoom_label(app_data);
            schedule_settings_save(app_data);
        }
    } else {
        // Pan mode: rotation translates horizontally (only when zoom > 1)
        if (app_data->zoom_level == 1) {
            return;  // No pan at 1x zoom
        }

        // Calculate pan step: one grid line per detent (scales with zoom)
        int visible_bins = FFT_SIZE / app_data->zoom_level;
        int pan_step = visible_bins / 10;  // 10 grid lines on display
        if (pan_step < 1) pan_step = 1;

        // Update pan offset (negative direction for intuitive left/right)
        app_data->pan_offset -= direction * pan_step;

        // Clamp to valid range
        int max_pan = (FFT_SIZE - visible_bins) / 2;
        if (app_data->pan_offset < -max_pan) app_data->pan_offset = -max_pan;
        if (app_data->pan_offset > max_pan) app_data->pan_offset = max_pan;

        // Apply pan to both widgets
        spectrum_widget_set_pan(SPECTRUM_WIDGET(app_data->spectrum), app_data->pan_offset);
        waterfall_widget_set_pan(WATERFALL_WIDGET(app_data->waterfall), app_data->pan_offset);

        update_zoom_label(app_data);
        schedule_settings_save(app_data);
    }
}

// Encoder 2 button callback - toggles between zoom and pan mode
static void on_encoder2_button(void *user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    // Toggle mode
    if (app_data->encoder2_mode == ENCODER2_MODE_ZOOM) {
        app_data->encoder2_mode = ENCODER2_MODE_PAN;
    } else {
        app_data->encoder2_mode = ENCODER2_MODE_ZOOM;
    }

    update_zoom_label(app_data);
}
#endif

// Window close handler
static gboolean on_window_close(GtkWindow *window G_GNUC_UNUSED, gpointer user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    // Save settings before closing
    app_settings_t settings;
    settings.spectrum_ref = gtk_adjustment_get_value(app_data->ref_adj);
    settings.spectrum_range = gtk_adjustment_get_value(app_data->range_adj);
    settings.waterfall_ref = gtk_adjustment_get_value(app_data->waterfall_ref_adj);
    settings.waterfall_range = gtk_adjustment_get_value(app_data->waterfall_range_adj);
#ifdef HAVE_GPIOD
    if (app_data->pi_mode) {
        settings.zoom_level = app_data->zoom_level;
        settings.pan_offset = app_data->pan_offset;
    } else {
        settings.zoom_level = 1;
        settings.pan_offset = 0;
    }
#else
    settings.zoom_level = 1;
    settings.pan_offset = 0;
#endif
    settings_save(&settings);

    // Signal USB thread to stop
    atomic_store(&app_data->running, 0);

    // Wait for USB thread
    pthread_join(app_data->usb_thread, NULL);

    return FALSE;  // Allow window to close
}

static void activate(GtkApplication *gtk_app, gpointer user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    // Create main window
    app_data->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app_data->window), "Elad FDM-DUO Spectrum");
    gtk_window_set_default_size(GTK_WINDOW(app_data->window), app_data->window_width, app_data->window_height);

    // Apply CSS styling only in Pi mode (dark theme for embedded display)
    // Otherwise use system theme
    if (app_data->pi_mode) {
        GtkCssProvider *css_provider = gtk_css_provider_new();
        const char *css =
            "window, box, paned, frame, label, button, spinbutton, entry {"
            "  background-color: #000000;"
            "  color: #FFFFFF;"
            "  border: none;"
            "  outline: none;"
            "  box-shadow: none;"
            "}"
            "spinbutton, spinbutton text, entry, entry text {"
            "  background-color: #000000;"
            "  color: #FFFFFF;"
            "  border: none;"
            "  outline: none;"
            "}"
            "* {"
            "  outline-width: 0px;"
            "}"
            "frame > label {"
            "  color: #FFFFFF;"
            "}"
            "frame > border {"
            "  border: none;"
            "}"
            "paned > separator {"
            "  background-color: #333333;"
            "}"
            ".status-indicator {"
            "  font-size: 24px;"
            "}"
            ".connected {"
            "  color: #00FF00;"
            "}"
            ".disconnected {"
            "  color: #888888;"
            "}"
            ".error {"
            "  color: #FF0000;"
            "}";
        gtk_css_provider_load_from_string(css_provider, css);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css_provider);
    }

    // Connect close handler
    g_signal_connect(app_data->window, "close-request", G_CALLBACK(on_window_close), app_data);

    // Main vertical box
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(vbox, 5);
    gtk_widget_set_margin_end(vbox, 5);
    gtk_widget_set_margin_top(vbox, 5);
    gtk_widget_set_margin_bottom(vbox, 5);
    gtk_window_set_child(GTK_WINDOW(app_data->window), vbox);

    // Control bar - centered (will be added at bottom later)
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);

    // Status indicator - colored circle
    app_data->status_icon = gtk_label_new("○");
    gtk_widget_add_css_class(GTK_WIDGET(app_data->status_icon), "status-indicator");
    gtk_widget_add_css_class(GTK_WIDGET(app_data->status_icon), "disconnected");
    gtk_box_append(GTK_BOX(hbox), app_data->status_icon);

    // Load saved settings
    app_settings_t settings;
    settings_load(&settings);

    // Create all adjustments with loaded values
    app_data->ref_adj = gtk_adjustment_new(settings.spectrum_ref, -80.0, 20.0, 5.0, 10.0, 0.0);
    g_signal_connect(app_data->ref_adj, "value-changed", G_CALLBACK(on_spectrum_range_changed), app_data);

    app_data->range_adj = gtk_adjustment_new(settings.spectrum_range, 20.0, 150.0, 10.0, 20.0, 0.0);
    g_signal_connect(app_data->range_adj, "value-changed", G_CALLBACK(on_spectrum_range_changed), app_data);

    app_data->waterfall_ref_adj = gtk_adjustment_new(settings.waterfall_ref, -80.0, 20.0, 5.0, 10.0, 0.0);
    g_signal_connect(app_data->waterfall_ref_adj, "value-changed", G_CALLBACK(on_waterfall_range_changed), app_data);

    app_data->waterfall_range_adj = gtk_adjustment_new(settings.waterfall_range, 20.0, 150.0, 10.0, 20.0, 0.0);
    g_signal_connect(app_data->waterfall_range_adj, "value-changed", G_CALLBACK(on_waterfall_range_changed), app_data);

#ifdef HAVE_GPIOD
    if (app_data->pi_mode) {
        // Pi mode: read-only label showing all parameter values
        app_data->param_label = gtk_label_new(NULL);
        gtk_box_append(GTK_BOX(hbox), app_data->param_label);

        app_data->zoom_label = gtk_label_new(NULL);
        gtk_box_append(GTK_BOX(hbox), app_data->zoom_label);
    } else
#endif
    {
        // Normal mode: separate Ref and Rng spinbuttons
        GtkWidget *ref_label = gtk_label_new("Ref");
        gtk_box_append(GTK_BOX(hbox), ref_label);

        GtkWidget *ref_spin = gtk_spin_button_new(app_data->ref_adj, 1.0, 0);
        gtk_box_append(GTK_BOX(hbox), ref_spin);

        GtkWidget *range_label = gtk_label_new("Rng");
        gtk_box_append(GTK_BOX(hbox), range_label);

        GtkWidget *range_spin = gtk_spin_button_new(app_data->range_adj, 1.0, 0);
        gtk_box_append(GTK_BOX(hbox), range_spin);
    }

    // Paned container for spectrum and waterfall
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(vbox), paned);

    // Spectrum widget (same height as waterfall)
    app_data->spectrum = spectrum_widget_new();
    int display_min_h = (app_data->window_height <= 480) ? 100 : 150;
    gtk_widget_set_size_request(app_data->spectrum, -1, display_min_h);
    spectrum_widget_set_center_freq(SPECTRUM_WIDGET(app_data->spectrum), app_data->center_freq_hz);
    spectrum_widget_set_sample_rate(SPECTRUM_WIDGET(app_data->spectrum), DEFAULT_SAMPLE_RATE);

    // Set initial range from adjustments
    float ref_db = (float)gtk_adjustment_get_value(app_data->ref_adj);
    float range_db = (float)gtk_adjustment_get_value(app_data->range_adj);
    spectrum_widget_set_range(SPECTRUM_WIDGET(app_data->spectrum), ref_db - range_db, ref_db);

    // Set initial overlay
    char freq_str[32];
    snprintf(freq_str, sizeof(freq_str), "%.6f MHz", app_data->center_freq_hz / 1e6);
    spectrum_widget_set_overlay(SPECTRUM_WIDGET(app_data->spectrum), freq_str, "---");

    // Load bandplan for band overlay display
    // Try development path first, then installed path
    if (bandplan_load(&app_data->bandplan, "./resources/bands-r1.json") != 0) {
        if (bandplan_load(&app_data->bandplan, "/usr/share/elad-spectrum/bands-r1.json") != 0) {
            fprintf(stderr, "Bandplan: No bandplan file found, band overlays disabled\n");
        }
    }
    spectrum_widget_set_bandplan(SPECTRUM_WIDGET(app_data->spectrum), &app_data->bandplan);

    app_data->spectrum_frame = gtk_frame_new("VFO A");
    gtk_frame_set_child(GTK_FRAME(app_data->spectrum_frame), app_data->spectrum);
    gtk_paned_set_start_child(GTK_PANED(paned), app_data->spectrum_frame);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), TRUE);

    // Waterfall widget (same height as spectrum)
    app_data->waterfall = waterfall_widget_new();
    gtk_widget_set_size_request(app_data->waterfall, -1, display_min_h);
    waterfall_widget_set_sample_rate(WATERFALL_WIDGET(app_data->waterfall), DEFAULT_SAMPLE_RATE);
    // Use waterfall-specific adjustments for initial range
    float wf_ref_db = (float)gtk_adjustment_get_value(app_data->waterfall_ref_adj);
    float wf_range_db = (float)gtk_adjustment_get_value(app_data->waterfall_range_adj);
    waterfall_widget_set_range(WATERFALL_WIDGET(app_data->waterfall), wf_ref_db - wf_range_db, wf_ref_db);

    GtkWidget *waterfall_frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(waterfall_frame), app_data->waterfall);
    gtk_paned_set_end_child(GTK_PANED(paned), waterfall_frame);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), TRUE);

    // Set paned position (equal split for spectrum and waterfall)
    int paned_pos = (app_data->window_height - 60) / 2;  // 60 for margins and control bar
    gtk_paned_set_position(GTK_PANED(paned), paned_pos);

    // Add control bar at bottom
    gtk_box_append(GTK_BOX(vbox), hbox);

    // Initialize USB device
    app_data->usb = usb_device_new();
    if (!app_data->usb) {
        fprintf(stderr, "Failed to initialize USB\n");
        gtk_label_set_text(GTK_LABEL(app_data->status_icon), "✖");
        gtk_widget_add_css_class(GTK_WIDGET(app_data->status_icon), "error");
    }

    // Initialize CAT control
    app_data->cat = cat_control_new();
    if (app_data->cat) {
        if (cat_control_open(app_data->cat, "/dev/ttyUSB0") == 0) {
            // Query frequency immediately on connect
            long freq;
            elad_mode_t mode;
            int vfo;
            if (cat_control_get_freq_mode(app_data->cat, &freq, &mode, &vfo) == 0) {
                if (freq > 0) {
                    app_data->center_freq_hz = (int)freq;
                    app_data->current_mode = mode;
                    app_data->current_vfo = vfo;

                    // Update spectrum display
                    spectrum_widget_set_center_freq(SPECTRUM_WIDGET(app_data->spectrum), app_data->center_freq_hz);

                    // Update overlay
                    char freq_str[32];
                    snprintf(freq_str, sizeof(freq_str), "%.6f MHz", app_data->center_freq_hz / 1e6);
                    spectrum_widget_set_overlay(SPECTRUM_WIDGET(app_data->spectrum), freq_str,
                                                usb_device_mode_name(app_data->current_mode));

                    // Update frame label
                    gtk_frame_set_label(GTK_FRAME(app_data->spectrum_frame),
                                        vfo == 0 ? "VFO A" : "VFO B");

                    fprintf(stderr, "CAT: Initial frequency %.6f MHz, mode %s, VFO %c\n",
                            freq / 1e6, usb_device_mode_name(mode), vfo == 0 ? 'A' : 'B');
                }
            }
        } else {
            fprintf(stderr, "CAT: Will retry when USB connects\n");
        }
    }

    // Start CAT TCP server
    if (app_data->cat_server_port > 0 && app_data->cat) {
        app_data->cat_server = cat_server_new();
        if (app_data->cat_server) {
            cat_server_set_cat(app_data->cat_server, app_data->cat, &app_data->cat_mutex);
            if (cat_server_start(app_data->cat_server, app_data->cat_server_port,
                                 app_data->cat_listen_addr[0] ? app_data->cat_listen_addr : NULL) != 0) {
                fprintf(stderr, "CAT server: failed to start on port %d\n", app_data->cat_server_port);
            }
        }
    }

    // Initialize FFT processor
    app_data->fft = fft_processor_new(FFT_SIZE);
    if (!app_data->fft) {
        fprintf(stderr, "Failed to initialize FFT\n");
        gtk_label_set_text(GTK_LABEL(app_data->status_icon), "✖");
        gtk_widget_add_css_class(GTK_WIDGET(app_data->status_icon), "error");
    }

#ifdef HAVE_GPIOD
    // Initialize rotary encoders (Pi mode only)
    if (app_data->pi_mode) {
        app_data->zoom_level = settings.zoom_level;
        app_data->pan_offset = settings.pan_offset;
        app_data->active_param = PARAM_SPECTRUM_REF;
        app_data->encoder2_mode = ENCODER2_MODE_ZOOM;

        // Apply saved zoom/pan to widgets
        spectrum_widget_set_zoom(SPECTRUM_WIDGET(app_data->spectrum), app_data->zoom_level);
        spectrum_widget_set_pan(SPECTRUM_WIDGET(app_data->spectrum), app_data->pan_offset);
        waterfall_widget_set_zoom(WATERFALL_WIDGET(app_data->waterfall), app_data->zoom_level);
        waterfall_widget_set_pan(WATERFALL_WIDGET(app_data->waterfall), app_data->pan_offset);

        // Encoder 1 - Parameter control (GPIO 17/27/22)
        app_data->encoder1 = rotary_encoder_new_with_pins(
            ENCODER1_CLK_PIN, ENCODER1_DT_PIN, ENCODER1_SW_PIN);
        if (app_data->encoder1) {
            rotary_encoder_set_rotation_callback(app_data->encoder1,
                                                 on_encoder1_rotation, app_data);
            rotary_encoder_set_button_callback(app_data->encoder1,
                                               on_encoder1_button, app_data);
            rotary_encoder_start_polling(app_data->encoder1);
            fprintf(stderr, "Encoder 1 (parameter) initialized\n");
        } else {
            fprintf(stderr, "Encoder 1 not available\n");
        }

        // Encoder 2 - Zoom/Pan control (GPIO 5/6/13)
        app_data->encoder2 = rotary_encoder_new_with_pins(
            ENCODER2_CLK_PIN, ENCODER2_DT_PIN, ENCODER2_SW_PIN);
        if (app_data->encoder2) {
            rotary_encoder_set_rotation_callback(app_data->encoder2,
                                                 on_encoder2_rotation, app_data);
            rotary_encoder_set_button_callback(app_data->encoder2,
                                               on_encoder2_button, app_data);
            rotary_encoder_start_polling(app_data->encoder2);
            fprintf(stderr, "Encoder 2 (zoom/pan) initialized\n");
        } else {
            fprintf(stderr, "Encoder 2 not available\n");
        }

        // Update labels
        update_param_label(app_data);
        update_zoom_label(app_data);
    }
#endif

    // Start USB thread
    atomic_store(&app_data->running, 1);
    atomic_store(&app_data->usb_connected, 0);
    atomic_store(&app_data->spectrum_ready, 0);

    if (pthread_create(&app_data->usb_thread, NULL, usb_thread_func, app_data) != 0) {
        fprintf(stderr, "Failed to create USB thread\n");
        gtk_label_set_text(GTK_LABEL(app_data->status_icon), "✖");
        gtk_widget_add_css_class(GTK_WIDGET(app_data->status_icon), "error");
    }

    // Start display refresh timer (~30 FPS)
    g_timeout_add(33, refresh_display, app_data);

    // Apply fullscreen if requested
    if (app_data->fullscreen) {
        gtk_window_fullscreen(GTK_WINDOW(app_data->window));
    }

    gtk_window_present(GTK_WINDOW(app_data->window));
}

static void shutdown_app(GtkApplication *gtk_app G_GNUC_UNUSED, gpointer user_data) {
    app_data_t *app_data = (app_data_t *)user_data;

    // Cleanup
#ifdef HAVE_GPIOD
    rotary_encoder_free(app_data->encoder1);
    rotary_encoder_free(app_data->encoder2);
#endif
    cat_server_free(app_data->cat_server);
    fft_processor_free(app_data->fft);
    usb_device_free(app_data->usb);
    cat_control_free(app_data->cat);
    pthread_mutex_destroy(&app_data->cat_mutex);
    bandplan_free(&app_data->bandplan);
    g_mutex_clear(&app_data->spectrum_mutex);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -f, --fullscreen       Start in fullscreen mode\n");
    fprintf(stderr, "  -p, --pi               Set window size to 800x480 (5\" LCD)\n");
    fprintf(stderr, "  -c, --cat-port PORT    Start CAT TCP server on PORT (default: %d)\n", CAT_SERVER_DEFAULT_PORT);
    fprintf(stderr, "  -l, --cat-listen ADDR  CAT server listen address: localhost (default) or any\n");
    fprintf(stderr, "  -h, --help             Show this help message\n");
}

int main(int argc, char *argv[]) {
    // Initialize app data
    memset(&app, 0, sizeof(app));
    g_mutex_init(&app.spectrum_mutex);
    pthread_mutex_init(&app.cat_mutex, NULL);
    app.center_freq_hz = 15300000;  // 15.3 MHz default
    app.fullscreen = FALSE;
    app.pi_mode = FALSE;
    app.window_width = 1024;   // Default size
    app.window_height = 768;
    app.cat_server_port = 0;   // Disabled by default

    // Parse and filter command-line options (before GTK takes over)
    int new_argc = 1;
    char **new_argv = g_malloc(sizeof(char *) * (argc + 1));
    new_argv[0] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fullscreen") == 0) {
            app.fullscreen = TRUE;
            // Don't pass to GTK
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pi") == 0) {
            app.pi_mode = TRUE;
            app.window_width = 800;
            app.window_height = 480;
            // Don't pass to GTK
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--cat-port") == 0) {
            if (i + 1 < argc) {
                app.cat_server_port = atoi(argv[++i]);
                if (app.cat_server_port <= 0 || app.cat_server_port > 65535) {
                    fprintf(stderr, "Invalid port number: %s\n", argv[i]);
                    g_free(new_argv);
                    return 1;
                }
            } else {
                fprintf(stderr, "Missing port number for -c/--cat-port\n");
                g_free(new_argv);
                return 1;
            }
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--cat-listen") == 0) {
            if (i + 1 < argc) {
                const char *addr = argv[++i];
                if (strcmp(addr, "any") == 0 || strcmp(addr, "localhost") == 0) {
                    snprintf(app.cat_listen_addr, sizeof(app.cat_listen_addr), "%s", addr);
                } else {
                    fprintf(stderr, "Invalid listen address: %s (use 'localhost' or 'any')\n", addr);
                    g_free(new_argv);
                    return 1;
                }
            } else {
                fprintf(stderr, "Missing address for -l/--cat-listen\n");
                g_free(new_argv);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            g_free(new_argv);
            return 0;
        } else {
            // Pass unknown options to GTK
            new_argv[new_argc++] = argv[i];
        }
    }
    new_argv[new_argc] = NULL;

    // Create GTK application
    app.app = gtk_application_new("org.elad.spectrum", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.app, "activate", G_CALLBACK(activate), &app);
    g_signal_connect(app.app, "shutdown", G_CALLBACK(shutdown_app), &app);

    // Run application
    int status = g_application_run(G_APPLICATION(app.app), new_argc, new_argv);

    g_free(new_argv);
    g_object_unref(app.app);
    return status;
}
