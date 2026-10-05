/* pump.c - relay-driven water pump with a hardware-timer failsafe */
#include "pump.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "pump";

static esp_timer_handle_t s_failsafe;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_on;
static volatile bool s_tripped;
static int64_t s_started_us;
static uint32_t s_total_s;

static inline void relay_write(bool on)
{
#if CONFIG_IRRIG_PUMP_ACTIVE_LOW
    gpio_set_level(CONFIG_IRRIG_PUMP_GPIO, on ? 0 : 1);
#else
    gpio_set_level(CONFIG_IRRIG_PUMP_GPIO, on ? 1 : 0);
#endif
}

static void failsafe_cb(void *arg)
{
    (void)arg;
    if (s_on) {
        pump_off();
        s_tripped = true;
        ESP_LOGE(TAG, "failsafe timer expired, pump forced OFF");
    }
}

esp_err_t pump_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_IRRIG_PUMP_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    relay_write(false);

    const esp_timer_create_args_t args = { .callback = failsafe_cb, .name = "pump_failsafe" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_failsafe), TAG, "timer");
#ifdef CONFIG_IRRIG_PUMP_ACTIVE_LOW
    ESP_LOGI(TAG, "relay on GPIO%d (active-low)", CONFIG_IRRIG_PUMP_GPIO);
#else
    ESP_LOGI(TAG, "relay on GPIO%d (active-high)", CONFIG_IRRIG_PUMP_GPIO);
#endif
    return ESP_OK;
}

void pump_on(uint32_t max_seconds)
{
    if (max_seconds == 0) return;
    esp_timer_stop(s_failsafe);                       /* disarm first so a pending expiry cannot race us */
    portENTER_CRITICAL(&s_mux);
    bool was_on = s_on;
    if (!was_on) {
        s_on = true;
        s_started_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_mux);
    relay_write(true);
    esp_timer_start_once(s_failsafe, (uint64_t)max_seconds * 1000000ULL + 2000000ULL);
    if (!was_on) ESP_LOGI(TAG, "ON (limit %lus)", (unsigned long)max_seconds);
}

void pump_off(void)
{
    relay_write(false);
    esp_timer_stop(s_failsafe);
    portENTER_CRITICAL(&s_mux);
    if (s_on) {
        s_on = false;
        s_total_s += (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "OFF");
        return;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool pump_is_on(void)
{
    return s_on;
}

uint32_t pump_run_seconds(void)
{
    if (!s_on) return 0;
    return (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
}

uint32_t pump_total_seconds(void)
{
    return s_total_s + pump_run_seconds();
}

bool pump_failsafe_tripped(void)
{
    portENTER_CRITICAL(&s_mux);
    bool t = s_tripped;
    s_tripped = false;
    portEXIT_CRITICAL(&s_mux);
    return t;
}
