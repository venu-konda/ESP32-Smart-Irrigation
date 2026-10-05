/* config.c - persistent controller settings (NVS-backed) */
#include <string.h>
#include <stdio.h>
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "config";
#define NVS_NAMESPACE "irrig"
#define NVS_KEY "cfg"

static irrig_config_t s_cfg;
static SemaphoreHandle_t s_lock;

static const irrig_config_t s_defaults = {
    .version          = IRRIG_CONFIG_VERSION,
    .on_pct           = 35,
    .off_pct          = 60,
    .max_pump_s       = 120,
    .soak_s           = 600,
    .min_interval_s   = 1800,
    .sample_period_s  = 30,
    .raw_dry          = 2650,   /* typical capacitive v1.2 sensor at 3.3 V, dry air */
    .raw_wet          = 1150,   /* same sensor submerged */
    .window_start_h   = 5,
    .window_end_h     = 9,
    .window_enabled   = 1,
    .auto_mode        = 1,
    .max_fault_cycles = 3,
};

typedef struct {
    const char *name;
    size_t offset;
    uint8_t size;      /* 1 or 2 bytes */
    int min, max;
} key_desc_t;

#define KEY(n, f, lo, hi) { n, offsetof(irrig_config_t, f), sizeof(((irrig_config_t *)0)->f), lo, hi }
static const key_desc_t s_keys[] = {
    KEY("on_pct",           on_pct,           0, 99),
    KEY("off_pct",          off_pct,          1, 100),
    KEY("max_pump_s",       max_pump_s,       5, 3600),
    KEY("soak_s",           soak_s,           0, 36000),
    KEY("min_interval_s",   min_interval_s,   0, 65535),
    KEY("sample_period_s",  sample_period_s,  2, 3600),
    KEY("raw_dry",          raw_dry,          0, 3600),
    KEY("raw_wet",          raw_wet,          0, 3600),
    KEY("window_start_h",   window_start_h,   0, 23),
    KEY("window_end_h",     window_end_h,     0, 24),
    KEY("window_enabled",   window_enabled,   0, 1),
    KEY("auto_mode",        auto_mode,        0, 1),
    KEY("max_fault_cycles", max_fault_cycles, 1, 20),
};
#define NKEYS (sizeof(s_keys) / sizeof(s_keys[0]))
static const char *s_key_names[NKEYS + 1];

static const key_desc_t *find_key(const char *name)
{
    for (size_t i = 0; i < NKEYS; i++) {
        if (strcmp(s_keys[i].name, name) == 0) return &s_keys[i];
    }
    return NULL;
}

static int read_field(const irrig_config_t *c, const key_desc_t *k)
{
    const uint8_t *p = (const uint8_t *)c + k->offset;
    if (k->size == 2) { uint16_t v; memcpy(&v, p, 2); return v; }
    return *p;
}

static void write_field(irrig_config_t *c, const key_desc_t *k, int v)
{
    uint8_t *p = (uint8_t *)c + k->offset;
    if (k->size == 2) { uint16_t u = (uint16_t)v; memcpy(p, &u, 2); }
    else { *p = (uint8_t)v; }
}

esp_err_t config_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    for (size_t i = 0; i < NKEYS; i++) s_key_names[i] = s_keys[i].name;
    s_key_names[NKEYS] = NULL;

    s_cfg = s_defaults;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        irrig_config_t stored;
        size_t len = sizeof(stored);
        err = nvs_get_blob(h, NVS_KEY, &stored, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(stored) && stored.version == IRRIG_CONFIG_VERSION) {
            /* Never trust a blob blindly: clamp every field to its range. */
            int clamped = 0;
            for (size_t i = 0; i < NKEYS; i++) {
                int v = read_field(&stored, &s_keys[i]);
                if (v < s_keys[i].min) { write_field(&stored, &s_keys[i], s_keys[i].min); clamped++; }
                if (v > s_keys[i].max) { write_field(&stored, &s_keys[i], s_keys[i].max); clamped++; }
            }
            if (stored.off_pct <= stored.on_pct) { stored.on_pct = stored.off_pct - 1; clamped++; }
            s_cfg = stored;
            if (clamped) ESP_LOGW(TAG, "%d stored value(s) were out of range and clamped", clamped);
            ESP_LOGI(TAG, "settings loaded from NVS");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "stored settings missing or incompatible (%s), using defaults", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "no stored settings (%s), using defaults", esp_err_to_name(err));
    }
    return ESP_OK;
}

void config_snapshot(irrig_config_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t config_set(const char *key, int value)
{
    const key_desc_t *k = find_key(key);
    if (!k) return ESP_ERR_NOT_FOUND;
    if (value < k->min || value > k->max) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    write_field(&s_cfg, k, value);
    /* keep thresholds consistent: off must stay above on (ranges guarantee on<=99, off>=1) */
    if (s_cfg.off_pct <= s_cfg.on_pct) {
        if (strcmp(key, "on_pct") == 0) s_cfg.off_pct = s_cfg.on_pct + 1;
        else s_cfg.on_pct = s_cfg.off_pct - 1;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_get(const char *key, int *value)
{
    const key_desc_t *k = find_key(key);
    if (!k) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *value = read_field(&s_cfg, k);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_set_calibration(uint16_t raw_dry, uint16_t raw_wet)
{
    if (raw_dry <= raw_wet + 100) return ESP_ERR_INVALID_ARG;  /* need a usable span */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.raw_dry = raw_dry;
    s_cfg.raw_wet = raw_wet;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    irrig_config_t copy;
    config_snapshot(&copy);
    err = nvs_set_blob(h, NVS_KEY, &copy, sizeof(copy));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "settings saved");
    return err;
}

void config_reset_defaults(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = s_defaults;
    xSemaphoreGive(s_lock);
}

void config_print(void)
{
    irrig_config_t c;
    config_snapshot(&c);
    for (size_t i = 0; i < NKEYS; i++) {
        printf("  %-18s = %d\n", s_keys[i].name, read_field(&c, &s_keys[i]));
    }
}

const char *const *config_keys(void)
{
    return s_key_names;
}
