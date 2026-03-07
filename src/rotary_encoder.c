#include "rotary_encoder.h"
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>

#define GPIOD_CHIP_PATH "/dev/gpiochip0"
#define DEBOUNCE_TIME_MS 200  // Button debounce time

struct rotary_encoder {
    struct gpiod_chip *chip;
    struct gpiod_line *clk_line;
    struct gpiod_line *dt_line;
    struct gpiod_line *sw_line;

    // GPIO pin numbers (stored for debug output)
    int clk_pin;
    int dt_pin;
    int sw_pin;

    int last_clk_state;
    gint64 last_button_time;  // For debouncing
    encoder_param_t active_param;

    encoder_rotation_callback_t rotation_callback;
    void *rotation_user_data;

    encoder_button_callback_t button_callback;
    void *button_user_data;

    guint poll_source_id;
};

rotary_encoder_t *rotary_encoder_new_with_pins(int clk_pin, int dt_pin, int sw_pin) {
    rotary_encoder_t *encoder = calloc(1, sizeof(rotary_encoder_t));
    if (!encoder) {
        return NULL;
    }

    // Store pin numbers
    encoder->clk_pin = clk_pin;
    encoder->dt_pin = dt_pin;
    encoder->sw_pin = sw_pin;

    // Open GPIO chip
    encoder->chip = gpiod_chip_open(GPIOD_CHIP_PATH);
    if (!encoder->chip) {
        fprintf(stderr, "Encoder: Failed to open GPIO chip %s\n", GPIOD_CHIP_PATH);
        free(encoder);
        return NULL;
    }

    // Get GPIO lines
    encoder->clk_line = gpiod_chip_get_line(encoder->chip, clk_pin);
    encoder->dt_line = gpiod_chip_get_line(encoder->chip, dt_pin);
    encoder->sw_line = gpiod_chip_get_line(encoder->chip, sw_pin);

    if (!encoder->clk_line || !encoder->dt_line || !encoder->sw_line) {
        fprintf(stderr, "Encoder: Failed to get GPIO lines %d/%d/%d\n", clk_pin, dt_pin, sw_pin);
        gpiod_chip_close(encoder->chip);
        free(encoder);
        return NULL;
    }

    // Request lines as inputs with pull-up
    struct gpiod_line_request_config config = {
        .consumer = "elad-spectrum-encoder",
        .request_type = GPIOD_LINE_REQUEST_DIRECTION_INPUT,
        .flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP
    };

    if (gpiod_line_request(encoder->clk_line, &config, 0) < 0 ||
        gpiod_line_request(encoder->dt_line, &config, 0) < 0 ||
        gpiod_line_request(encoder->sw_line, &config, 0) < 0) {
        fprintf(stderr, "Encoder: Failed to request GPIO lines %d/%d/%d\n", clk_pin, dt_pin, sw_pin);
        gpiod_chip_close(encoder->chip);
        free(encoder);
        return NULL;
    }

    // Initialize state
    encoder->last_clk_state = gpiod_line_get_value(encoder->clk_line);
    encoder->last_button_time = 0;
    encoder->active_param = ENCODER_PARAM_SPECTRUM_REF;
    encoder->poll_source_id = 0;

    fprintf(stderr, "Encoder: Initialized on GPIO %d/%d/%d\n", clk_pin, dt_pin, sw_pin);

    return encoder;
}

rotary_encoder_t *rotary_encoder_new(void) {
    return rotary_encoder_new_with_pins(ENCODER1_CLK_PIN, ENCODER1_DT_PIN, ENCODER1_SW_PIN);
}

void rotary_encoder_free(rotary_encoder_t *encoder) {
    if (!encoder) return;

    rotary_encoder_stop_polling(encoder);

    if (encoder->clk_line) gpiod_line_release(encoder->clk_line);
    if (encoder->dt_line) gpiod_line_release(encoder->dt_line);
    if (encoder->sw_line) gpiod_line_release(encoder->sw_line);
    if (encoder->chip) gpiod_chip_close(encoder->chip);

    free(encoder);
}

void rotary_encoder_set_rotation_callback(rotary_encoder_t *encoder,
                                          encoder_rotation_callback_t callback,
                                          void *user_data) {
    if (!encoder) return;
    encoder->rotation_callback = callback;
    encoder->rotation_user_data = user_data;
}

void rotary_encoder_set_button_callback(rotary_encoder_t *encoder,
                                        encoder_button_callback_t callback,
                                        void *user_data) {
    if (!encoder) return;
    encoder->button_callback = callback;
    encoder->button_user_data = user_data;
}

encoder_param_t rotary_encoder_get_active_param(rotary_encoder_t *encoder) {
    if (!encoder) return ENCODER_PARAM_SPECTRUM_REF;
    return encoder->active_param;
}

void rotary_encoder_set_active_param(rotary_encoder_t *encoder, encoder_param_t param) {
    if (!encoder) return;
    encoder->active_param = param;
}

void rotary_encoder_toggle_param(rotary_encoder_t *encoder) {
    if (!encoder) return;
    encoder->active_param = (encoder->active_param + 1) % ENCODER_PARAM_COUNT;
}

// Poll callback - called from GLib main loop
static gboolean encoder_poll_callback(gpointer user_data) {
    rotary_encoder_t *encoder = (rotary_encoder_t *)user_data;

    // Read current state
    int clk_state = gpiod_line_get_value(encoder->clk_line);
    int dt_state = gpiod_line_get_value(encoder->dt_line);
    int sw_state = gpiod_line_get_value(encoder->sw_line);

    // Detect rotation on CLK falling edge
    if (clk_state != encoder->last_clk_state && clk_state == 0) {
        // Determine direction from DT state
        int direction = (dt_state != clk_state) ? 1 : -1;

        if (encoder->rotation_callback) {
            encoder->rotation_callback(direction, encoder->rotation_user_data);
        }
    }
    encoder->last_clk_state = clk_state;

    // Detect button press (active low with debounce)
    if (sw_state == 0) {
        gint64 now = g_get_monotonic_time() / 1000;  // Convert to ms
        if (now - encoder->last_button_time > DEBOUNCE_TIME_MS) {
            encoder->last_button_time = now;

            if (encoder->button_callback) {
                encoder->button_callback(encoder->button_user_data);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

guint rotary_encoder_start_polling(rotary_encoder_t *encoder) {
    if (!encoder || encoder->poll_source_id != 0) {
        return 0;
    }

    // Poll every 5ms for responsive encoder tracking
    encoder->poll_source_id = g_timeout_add(5, encoder_poll_callback, encoder);
    return encoder->poll_source_id;
}

void rotary_encoder_stop_polling(rotary_encoder_t *encoder) {
    if (!encoder || encoder->poll_source_id == 0) {
        return;
    }

    g_source_remove(encoder->poll_source_id);
    encoder->poll_source_id = 0;
}
