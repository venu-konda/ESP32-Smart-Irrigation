idf_component_register(
    SRCS "app_main.c"
         "config.c"
         "soil_sensor.c"
         "pump.c"
         "irrigation.c"
         "status_led.c"
         "console_cmds.c"
         "net_mqtt.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_adc nvs_flash esp_timer console esp_wifi esp_event esp_netif mqtt json lwip)
