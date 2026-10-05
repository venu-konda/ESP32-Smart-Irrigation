/* net_mqtt.h - Wi-Fi station, SNTP time sync and MQTT telemetry/commands */
#pragma once
#include "esp_err.h"
/* No-op when CONFIG_IRRIG_ENABLE_NETWORK is off. */
esp_err_t net_start(void);
