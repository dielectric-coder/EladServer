#include "fft_processor.h"
#include <fftw3.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define SPECTRUM_AVERAGING 3

struct fft_processor {
    int fft_size;
    int sample_count;

    // FFTW data
    double *fft_in;
    fftw_complex *fft_out;
    fftw_plan plan;

    // Window coefficients (Blackman-Harris)
    double *window;

    // Output spectrum in dB
    float *spectrum_db;

    // Averaging accumulator
    float *spectrum_accum;
    int avg_count;

    // RSSI (peak power in center passband)
    float rssi_db;

    // Mutex protecting spectrum_db and rssi_db (written by USB thread, read by GTK thread)
    pthread_mutex_t output_mutex;
};

// Generate Blackman-Harris window coefficients
static void generate_window(double *window, int size) {
    const double a0 = 0.35875;
    const double a1 = 0.48829;
    const double a2 = 0.14128;
    const double a3 = 0.01168;
    const double pi = 3.141592653589793238462643383279502884197169399375105820974944592;

    for (int i = 0; i < size; i++) {
        double x = (double)i / (double)(size - 1);
        window[i] = a0 - a1 * cos(2.0 * pi * x) + a2 * cos(4.0 * pi * x) - a3 * cos(6.0 * pi * x);
    }
}

fft_processor_t *fft_processor_new(int fft_size) {
    if (fft_size < 2) return NULL;  // Prevent division by zero in window generation

    fft_processor_t *fft = calloc(1, sizeof(fft_processor_t));
    if (!fft) return NULL;

    fft->fft_size = fft_size;
    pthread_mutex_init(&fft->output_mutex, NULL);
    fft->sample_count = 0;

    // Allocate FFTW arrays
    // For complex-to-complex FFT: input is interleaved I/Q
    fft->fft_in = fftw_malloc(sizeof(double) * fft_size * 2);
    fft->fft_out = fftw_malloc(sizeof(fftw_complex) * fft_size);

    if (!fft->fft_in || !fft->fft_out) {
        fft_processor_free(fft);
        return NULL;
    }

    // Create FFTW plan for complex-to-complex transform
    fft->plan = fftw_plan_dft_1d(fft_size, (fftw_complex *)fft->fft_in, fft->fft_out,
                                  FFTW_FORWARD, FFTW_MEASURE);
    if (!fft->plan) {
        fft_processor_free(fft);
        return NULL;
    }

    // Allocate and generate window
    fft->window = malloc(sizeof(double) * fft_size);
    if (!fft->window) {
        fft_processor_free(fft);
        return NULL;
    }
    generate_window(fft->window, fft_size);

    // Allocate output spectrum
    fft->spectrum_db = malloc(sizeof(float) * fft_size);
    if (!fft->spectrum_db) {
        fft_processor_free(fft);
        return NULL;
    }

    // Allocate averaging accumulator
    fft->spectrum_accum = calloc(fft_size, sizeof(float));
    if (!fft->spectrum_accum) {
        fft_processor_free(fft);
        return NULL;
    }
    fft->avg_count = 0;

    return fft;
}

void fft_processor_free(fft_processor_t *fft) {
    if (!fft) return;

    pthread_mutex_destroy(&fft->output_mutex);

    if (fft->plan) {
        fftw_destroy_plan(fft->plan);
    }
    if (fft->fft_in) {
        fftw_free(fft->fft_in);
    }
    if (fft->fft_out) {
        fftw_free(fft->fft_out);
    }
    free(fft->window);
    free(fft->spectrum_db);
    free(fft->spectrum_accum);
    free(fft);
}

// Convert 32-bit signed integer (4 bytes, little-endian) to float
static inline float convert_32bit_sample(const uint8_t *data) {
    int32_t value = (int32_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                              ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));
    // Normalize to [-1.0, 1.0]
    return (float)value / 2147483648.0f;
}

bool fft_processor_process(fft_processor_t *fft, const uint8_t *usb_data, int length) {
    if (!fft || !usb_data) return false;

    // FDM-DUO sends 32-bit IQ samples: 4 bytes I, 4 bytes Q = 8 bytes per sample
    const int bytes_per_sample = 8;
    int num_samples = length / bytes_per_sample;
    bool fft_completed = false;

    for (int i = 0; i < num_samples; i++) {
        const uint8_t *sample_data = usb_data + (i * bytes_per_sample);
        float i_sample = convert_32bit_sample(sample_data);
        float q_sample = convert_32bit_sample(sample_data + 4);

        // Apply window and store as complex (interleaved real/imag)
        int idx = fft->sample_count;
        double w = fft->window[idx];
        fft->fft_in[idx * 2] = i_sample * w;      // Real part
        fft->fft_in[idx * 2 + 1] = q_sample * w;  // Imaginary part

        fft->sample_count++;

        // Check if we have enough samples for FFT
        if (fft->sample_count >= fft->fft_size) {
            // Execute FFT
            fftw_execute(fft->plan);

            // Convert to magnitude in dB with FFT shift and accumulate
            int half = fft->fft_size / 2;

            for (int j = 0; j < fft->fft_size; j++) {
                // FFT shift: swap first and second half
                int src_idx = (j + half) % fft->fft_size;
                double re = fft->fft_out[src_idx][0];
                double im = fft->fft_out[src_idx][1];
                double mag = sqrt(re * re + im * im) / fft->fft_size;

                // Convert to dB, with floor to avoid -inf
                if (mag < 1e-10) mag = 1e-10;
                float db = 20.0f * log10f((float)mag);

                // Accumulate for averaging
                fft->spectrum_accum[j] += db;
            }
            fft->avg_count++;

            // Check if we have enough frames for averaging
            if (fft->avg_count >= SPECTRUM_AVERAGING) {
                float peak_db = -200.0f;
                int center_start = half - 16;  // ~3kHz passband centered
                int center_end = half + 16;

                // Compute average and find RSSI
                pthread_mutex_lock(&fft->output_mutex);
                for (int j = 0; j < fft->fft_size; j++) {
                    fft->spectrum_db[j] = fft->spectrum_accum[j] / SPECTRUM_AVERAGING;
                    fft->spectrum_accum[j] = 0.0f;  // Reset accumulator

                    // Track peak in center passband for RSSI
                    if (j >= center_start && j < center_end) {
                        if (fft->spectrum_db[j] > peak_db) {
                            peak_db = fft->spectrum_db[j];
                        }
                    }
                }
                fft->rssi_db = peak_db;
                pthread_mutex_unlock(&fft->output_mutex);
                fft->avg_count = 0;
                fft_completed = true;
            }

            // Reset for next FFT - continue processing remaining samples
            fft->sample_count = 0;
        }
    }

    return fft_completed;
}

void fft_processor_get_spectrum_db(fft_processor_t *fft, float *output) {
    if (!fft || !output) return;
    pthread_mutex_lock(&fft->output_mutex);
    memcpy(output, fft->spectrum_db, sizeof(float) * fft->fft_size);
    pthread_mutex_unlock(&fft->output_mutex);
}

int fft_processor_get_size(fft_processor_t *fft) {
    return fft ? fft->fft_size : 0;
}

float fft_processor_get_rssi(fft_processor_t *fft) {
    if (!fft) return -200.0f;
    pthread_mutex_lock(&fft->output_mutex);
    float val = fft->rssi_db;
    pthread_mutex_unlock(&fft->output_mutex);
    return val;
}
