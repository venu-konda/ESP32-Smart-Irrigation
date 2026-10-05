/* status_led.c - blink patterns that mirror the controller state
 *
 *   IDLE / SOAK      one short blink every 3 s
 *   WATERING/MANUAL  on
 *   LOCKOUT_TANK     two blinks every 2 s
 *   FAULT            fast blink
 */
#include "status_led.h"
#include "irrigation.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void led(bool on) { gpio_set_level(CONFIG_IRRIG_STATUS_LED_GPIO, on ? 1 : 0); }

static void led_task(void *arg)
{
    (void)arg;
    irrig_status_t st;
    for (;;) {
        irrigation_get_status(&st);
        switch (st.state) {
        case IRR_WATERING:
        case IRR_MANUAL:
            led(true);  vTaskDelay(pdMS_TO_TICKS(200));
            break;
        case IRR_FAULT:
            led(true);  vTaskDelay(pdMS_TO_TICKS(100));
            led(false); vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case IRR_LOCKOUT_TANK:
            led(true);  vTaskDelay(pdMS_TO_TICKS(100));
            led(false); vTaskDelay(pdMS_TO_TICKS(150));
            led(true);  vTaskDelay(pdMS_TO_TICKS(100));
            led(false); vTaskDelay(pdMS_TO_TICKS(1650));
            break;
        default:
            led(true);  vTaskDelay(pdMS_TO_TICKS(60));
            led(false); vTaskDelay(pdMS_TO_TICKS(2940));
            break;
        }
    }
}

esp_err_t status_led_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_IRRIG_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    led(false);
    return xTaskCreate(led_task, "status_led", 2048, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
