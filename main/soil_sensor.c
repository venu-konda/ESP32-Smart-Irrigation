/* soil_sensor.c - capacitive soil-moisture sensor on ADC1 */
#include <stdlib.h>
#include "soil_sensor.h"
#include "config.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "soil";

#define SOIL_CHANNEL   ((adc_channel_t)CONFIG_IRRIG_SOIL_ADC_CHANNEL)
#define SOIL_ATTEN     ADC_ATTEN_DB_12
#define SAMPLES        9
#define SETTLE_MS      250   /* sensor power-up settling time */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;
static SemaphoreHandle_t s_lock;   /* serialises sensor power + ADC between tasks */

static esp_err_t cali_init(void)
{
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = SOIL_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_line_fitting(&cfg, &s_cali);
#elif ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = SOIL_CHANNEL,
        .atten = SOIL_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_curve_fitting(&cfg, &s_cali);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t soil_sensor_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc unit");
    adc_oneshot_chan_cfg_t ch_cfg = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = SOIL_ATTEN };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, SOIL_CHANNEL, &ch_cfg), TAG, "adc channel");

    s_cali_ok = (cali_init() == ESP_OK);
    if (!s_cali_ok) ESP_LOGW(TAG, "no ADC calibration scheme, using linear estimate");

#if CONFIG_IRRIG_SENSOR_POWER_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_IRRIG_SENSOR_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "power pin");
    gpio_set_level(CONFIG_IRRIG_SENSOR_POWER_GPIO, 0);
#endif
    ESP_LOGI(TAG, "ADC1 channel %d ready", CONFIG_IRRIG_SOIL_ADC_CHANNEL);
    return ESP_OK;
}

static void sensor_power(bool on)
{
#if CONFIG_IRRIG_SENSOR_POWER_GPIO >= 0
    gpio_set_level(CONFIG_IRRIG_SENSOR_POWER_GPIO, on ? 1 : 0);
    if (on) vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
#else
    (void)on;
#endif
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)(*(const uint16_t *)a) - (int)(*(const uint16_t *)b);
}

static esp_err_t sample_mv(uint16_t *mv)
{
    int raw = 0;
    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, SOIL_CHANNEL, &raw), TAG, "adc read");
    int v = 0;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &v) == ESP_OK) {
        *mv = (uint16_t)v;
    } else {
        *mv = (uint16_t)((raw * 3300) / 4095);   /* 12-bit, ~3.3 V full scale at 12 dB */
    }
    return ESP_OK;
}

esp_err_t soil_sensor_read_mv(uint16_t *mv)
{
    uint16_t s[SAMPLES];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sensor_power(true);
    esp_err_t err = ESP_OK;
    for (int i = 0; i < SAMPLES && err == ESP_OK; i++) {
        err = sample_mv(&s[i]);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    sensor_power(false);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;
    qsort(s, SAMPLES, sizeof(s[0]), cmp_u16);
    *mv = s[SAMPLES / 2];
    return ESP_OK;
}

esp_err_t soil_sensor_read(soil_reading_t *out)
{
    uint16_t mv;
    ESP_RETURN_ON_ERROR(soil_sensor_read_mv(&mv), TAG, "read");
    irrig_config_t c;
    config_snapshot(&c);

    /* Capacitive sensors output a higher voltage when drier. */
    int span = (int)c.raw_dry - (int)c.raw_wet;
    int pct = 0;
    if (span > 0) {
        pct = (100 * ((int)c.raw_dry - (int)mv)) / span;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    out->millivolts = mv;
    out->percent = (uint8_t)pct;
    out->timestamp_us = esp_timer_get_time();
    return ESP_OK;
}
