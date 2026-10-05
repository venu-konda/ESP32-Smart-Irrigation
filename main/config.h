/* config.h - persistent controller settings (NVS-backed) */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define IRRIG_CONFIG_VERSION 1

typedef struct {
    uint16_t version;          /* struct layout version stored in NVS */
    uint16_t on_pct;           /* start watering when moisture % drops below this */
    uint16_t off_pct;          /* stop watering when moisture % reaches this */
    uint16_t max_pump_s;       /* longest single pump run, seconds */
    uint16_t soak_s;           /* pause after a max-run cycle before re-evaluating */
    uint16_t min_interval_s;   /* minimum gap between automatic cycles */
    uint16_t sample_period_s;  /* sensor sampling period while idle */
    uint16_t raw_dry;          /* sensor millivolts in dry air (calibration) */
    uint16_t raw_wet;          /* sensor millivolts in water (calibration) */
    uint8_t  window_start_h;   /* watering window start hour, local time */
    uint8_t  window_end_h;     /* watering window end hour (exclusive) */
    uint8_t  window_enabled;   /* 1 = only water inside the window */
    uint8_t  auto_mode;        /* 1 = automatic control enabled */
    uint8_t  max_fault_cycles; /* consecutive max-run cycles before FAULT */
} irrig_config_t;

/* Load settings from NVS, or defaults if none are stored. */
esp_err_t config_init(void);
/* Copy the current settings into *out (thread-safe). */
void config_snapshot(irrig_config_t *out);
/* Set one field by name, e.g. "on_pct". Validates ranges. Not persisted until config_save(). */
esp_err_t config_set(const char *key, int value);
/* Get one field by name. */
esp_err_t config_get(const char *key, int *value);
/* Store the dry/wet calibration points. */
esp_err_t config_set_calibration(uint16_t raw_dry, uint16_t raw_wet);
/* Persist to NVS. */
esp_err_t config_save(void);
/* Restore defaults in RAM (call config_save() to persist). */
void config_reset_defaults(void);
/* Print all keys and values to the log/console. */
void config_print(void);
/* NULL-terminated list of key names, for help text. */
const char *const *config_keys(void);
