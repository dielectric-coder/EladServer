#ifndef ROTARY_ENCODER_H
#define ROTARY_ENCODER_H

#include <glib.h>

// Default GPIO pin assignments (BCM numbering)
// Encoder 1 - Parameter control
#define ENCODER1_CLK_PIN  17   // Rotation clock (A)
#define ENCODER1_DT_PIN   27   // Rotation data (B)
#define ENCODER1_SW_PIN   22   // Push button

// Encoder 2 - Zoom/Pan control
#define ENCODER2_CLK_PIN  5    // Rotation clock (A)
#define ENCODER2_DT_PIN   6    // Rotation data (B)
#define ENCODER2_SW_PIN   13   // Push button

// Active parameter selection (for parameter encoder)
typedef enum {
    ENCODER_PARAM_SPECTRUM_REF = 0,    // Spectrum reference level
    ENCODER_PARAM_SPECTRUM_RANGE = 1,  // Spectrum dynamic range
    ENCODER_PARAM_WATERFALL_REF = 2,   // Waterfall reference level
    ENCODER_PARAM_WATERFALL_RANGE = 3, // Waterfall dynamic range
    ENCODER_PARAM_COUNT = 4
} encoder_param_t;

// Callback function type for rotation events
// direction: +1 for clockwise, -1 for counter-clockwise
typedef void (*encoder_rotation_callback_t)(int direction, void *user_data);

// Callback function type for button press events
typedef void (*encoder_button_callback_t)(void *user_data);

// Opaque encoder handle
typedef struct rotary_encoder rotary_encoder_t;

// Create and initialize rotary encoder with custom GPIO pins
// Returns NULL on failure (e.g., GPIO not available)
rotary_encoder_t *rotary_encoder_new_with_pins(int clk_pin, int dt_pin, int sw_pin);

// Create and initialize rotary encoder with default pins (encoder 1)
// Returns NULL on failure (e.g., GPIO not available)
rotary_encoder_t *rotary_encoder_new(void);

// Free encoder resources
void rotary_encoder_free(rotary_encoder_t *encoder);

// Set rotation callback
void rotary_encoder_set_rotation_callback(rotary_encoder_t *encoder,
                                          encoder_rotation_callback_t callback,
                                          void *user_data);

// Set button press callback
void rotary_encoder_set_button_callback(rotary_encoder_t *encoder,
                                        encoder_button_callback_t callback,
                                        void *user_data);

// Get currently active parameter
encoder_param_t rotary_encoder_get_active_param(rotary_encoder_t *encoder);

// Set active parameter
void rotary_encoder_set_active_param(rotary_encoder_t *encoder, encoder_param_t param);

// Toggle active parameter (called internally on button press)
void rotary_encoder_toggle_param(rotary_encoder_t *encoder);

// Start polling (adds GLib timeout source)
// Returns source ID, or 0 on failure
guint rotary_encoder_start_polling(rotary_encoder_t *encoder);

// Stop polling
void rotary_encoder_stop_polling(rotary_encoder_t *encoder);

#endif // ROTARY_ENCODER_H
