#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define CONFIG_DIR ".config/elad-spectrum"
#define CONFIG_FILE "settings.conf"

// Default values
#define DEFAULT_SPECTRUM_REF -30.0
#define DEFAULT_SPECTRUM_RANGE 120.0
#define DEFAULT_WATERFALL_REF -30.0
#define DEFAULT_WATERFALL_RANGE 120.0
#define DEFAULT_ZOOM_LEVEL 1
#define DEFAULT_PAN_OFFSET 0

void settings_init_defaults(app_settings_t *settings) {
    settings->spectrum_ref = DEFAULT_SPECTRUM_REF;
    settings->spectrum_range = DEFAULT_SPECTRUM_RANGE;
    settings->waterfall_ref = DEFAULT_WATERFALL_REF;
    settings->waterfall_range = DEFAULT_WATERFALL_RANGE;
    settings->zoom_level = DEFAULT_ZOOM_LEVEL;
    settings->pan_offset = DEFAULT_PAN_OFFSET;
}

// Get full path to config file
// Returns allocated string that must be freed by caller
static char *get_config_path(void) {
    const char *home = getenv("HOME");
    if (!home) {
        return NULL;
    }

    size_t len = strlen(home) + 1 + strlen(CONFIG_DIR) + 1 + strlen(CONFIG_FILE) + 1;
    char *path = malloc(len);
    if (!path) {
        return NULL;
    }

    snprintf(path, len, "%s/%s/%s", home, CONFIG_DIR, CONFIG_FILE);
    return path;
}

// Get config directory path
static char *get_config_dir(void) {
    const char *home = getenv("HOME");
    if (!home) {
        return NULL;
    }

    size_t len = strlen(home) + 1 + strlen(CONFIG_DIR) + 1;
    char *path = malloc(len);
    if (!path) {
        return NULL;
    }

    snprintf(path, len, "%s/%s", home, CONFIG_DIR);
    return path;
}

// Create config directory if it doesn't exist
static int ensure_config_dir(void) {
    char *dir = get_config_dir();
    if (!dir) {
        return -1;
    }

    // Try to create parent .config first (may already exist)
    const char *home = getenv("HOME");
    if (home) {
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/.config", home);
        mkdir(parent, 0755);  // Ignore error if exists
    }

    // Create our config directory
    int result = mkdir(dir, 0755);
    if (result != 0 && errno != EEXIST) {
        free(dir);
        return -1;
    }

    free(dir);
    return 0;
}

void settings_load(app_settings_t *settings) {
    // Initialize to defaults first
    settings_init_defaults(settings);

    char *path = get_config_path();
    if (!path) {
        return;
    }

    FILE *f = fopen(path, "r");
    free(path);

    if (!f) {
        // File doesn't exist, use defaults
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        double dval;
        int ival;

        // Try parsing as double value
        if (sscanf(line, "spectrum_ref=%lf", &dval) == 1) {
            settings->spectrum_ref = dval;
        } else if (sscanf(line, "spectrum_range=%lf", &dval) == 1) {
            settings->spectrum_range = dval;
        } else if (sscanf(line, "waterfall_ref=%lf", &dval) == 1) {
            settings->waterfall_ref = dval;
        } else if (sscanf(line, "waterfall_range=%lf", &dval) == 1) {
            settings->waterfall_range = dval;
        } else if (sscanf(line, "zoom_level=%d", &ival) == 1) {
            // Validate zoom level (must be power of 2: 1, 2, 4, 8, 16)
            if (ival == 1 || ival == 2 || ival == 4 || ival == 8 || ival == 16) {
                settings->zoom_level = ival;
            }
        } else if (sscanf(line, "pan_offset=%d", &ival) == 1) {
            settings->pan_offset = ival;
        }
    }

    fclose(f);

    // Validate ranges to prevent display glitches from corrupted config
    if (settings->spectrum_range < 1.0) settings->spectrum_range = DEFAULT_SPECTRUM_RANGE;
    if (settings->waterfall_range < 1.0) settings->waterfall_range = DEFAULT_WATERFALL_RANGE;
    if (settings->spectrum_ref < -200.0 || settings->spectrum_ref > 50.0)
        settings->spectrum_ref = DEFAULT_SPECTRUM_REF;
    if (settings->waterfall_ref < -200.0 || settings->waterfall_ref > 50.0)
        settings->waterfall_ref = DEFAULT_WATERFALL_REF;
}

void settings_save(const app_settings_t *settings) {
    if (ensure_config_dir() != 0) {
        fprintf(stderr, "Failed to create config directory\n");
        return;
    }

    char *path = get_config_path();
    if (!path) {
        return;
    }

    FILE *f = fopen(path, "w");
    free(path);

    if (!f) {
        fprintf(stderr, "Failed to open config file for writing\n");
        return;
    }

    fprintf(f, "# EladSpectrum Settings\n");
    fprintf(f, "spectrum_ref=%.1f\n", settings->spectrum_ref);
    fprintf(f, "spectrum_range=%.1f\n", settings->spectrum_range);
    fprintf(f, "waterfall_ref=%.1f\n", settings->waterfall_ref);
    fprintf(f, "waterfall_range=%.1f\n", settings->waterfall_range);
    fprintf(f, "zoom_level=%d\n", settings->zoom_level);
    fprintf(f, "pan_offset=%d\n", settings->pan_offset);

    fclose(f);
}
