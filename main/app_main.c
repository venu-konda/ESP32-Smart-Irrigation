/* app_main.c - ESP32 autonomous smart irrigation controller
 *
 * Boot order: NVS -> settings -> pump (safe OFF) -> sensor -> LED -> controller
 * tasks -> console -> network (optional).
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "config.h"
#include "pump.h"
#include "soil_sensor.h"
#include "status_led.h"
#include "irrigation.h"
#include "console_cmds.h"
#include "net_mqtt.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(pump_init());          /* first: guarantees the relay starts OFF */
    ESP_ERROR_CHECK(soil_sensor_init());
    ESP_ERROR_CHECK(status_led_start());
    ESP_ERROR_CHECK(irrigation_start());
    ESP_ERROR_CHECK(console_start());
    ESP_ERROR_CHECK(net_start());

    ESP_LOGI(TAG, "smart irrigation controller running");
    config_print();
}
