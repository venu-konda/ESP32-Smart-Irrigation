/* net_mqtt.c - Wi-Fi station, SNTP time sync and MQTT telemetry/commands
 *
 * Topics (base = CONFIG_IRRIG_MQTT_BASE_TOPIC):
 *   <base>/status   JSON, published every sample period and on every state change (retained)
 *   <base>/online   "online" / "offline" (last will, retained)
 *   <base>/cmd      JSON commands, e.g.
 *                     {"cmd":"water","seconds":30}
 *                     {"cmd":"stop"}
 *                     {"cmd":"set","key":"on_pct","value":40}
 *                     {"cmd":"save"}
 *                     {"cmd":"auto","value":1}
 *                     {"cmd":"reset_fault"}
 *                     {"cmd":"sample"}
 */
#include "net_mqtt.h"
#include "sdkconfig.h"

#if CONFIG_IRRIG_ENABLE_NETWORK

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "config.h"
#include "irrigation.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "net";

#define TOPIC_STATUS CONFIG_IRRIG_MQTT_BASE_TOPIC "/status"
#define TOPIC_ONLINE CONFIG_IRRIG_MQTT_BASE_TOPIC "/online"
#define TOPIC_CMD    CONFIG_IRRIG_MQTT_BASE_TOPIC "/cmd"

#define BIT_GOT_IP      BIT0
#define BIT_MQTT_UP     BIT1

static EventGroupHandle_t s_events;
static esp_mqtt_client_handle_t s_mqtt;
static bool s_sntp_started;

/* ---- command handling ------------------------------------------------- */

static void handle_command(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) { ESP_LOGW(TAG, "bad JSON command"); return; }
    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd)) { ESP_LOGW(TAG, "command without 'cmd'"); cJSON_Delete(root); return; }

    const char *c = cmd->valuestring;
    if (strcmp(c, "water") == 0) {
        const cJSON *s = cJSON_GetObjectItem(root, "seconds");
        int sec = cJSON_IsNumber(s) ? s->valueint : 30;
        if (sec > 0) irrigation_manual_water((uint32_t)sec);
    } else if (strcmp(c, "stop") == 0) {
        irrigation_manual_water(0);
    } else if (strcmp(c, "set") == 0) {
        const cJSON *k = cJSON_GetObjectItem(root, "key");
        const cJSON *v = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsString(k) && cJSON_IsNumber(v)) {
            esp_err_t err = config_set(k->valuestring, v->valueint);
            ESP_LOGI(TAG, "set %s=%d: %s", k->valuestring, v->valueint, esp_err_to_name(err));
        }
    } else if (strcmp(c, "save") == 0) {
        config_save();
    } else if (strcmp(c, "auto") == 0) {
        const cJSON *v = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsNumber(v)) config_set("auto_mode", v->valueint ? 1 : 0);
    } else if (strcmp(c, "reset_fault") == 0) {
        irrigation_reset_fault();
    } else if (strcmp(c, "sample") == 0) {
        irrigation_request_sample();
    } else {
        ESP_LOGW(TAG, "unknown command '%s'", c);
    }
    cJSON_Delete(root);
    irrigation_request_sample();   /* reflect the change in the next status message */
}

/* ---- status publishing ------------------------------------------------ */

static void publish_status(void)
{
    if (!s_mqtt || !(xEventGroupGetBits(s_events) & BIT_MQTT_UP)) return;
    irrig_status_t st;
    irrigation_get_status(&st);
    irrig_config_t c;
    config_snapshot(&c);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", irrigation_state_name(st.state));
    if (st.have_reading) {
        cJSON_AddNumberToObject(o, "moisture_pct", st.last.percent);
        cJSON_AddNumberToObject(o, "sensor_mv", st.last.millivolts);
    }
    cJSON_AddBoolToObject(o, "pump", st.pump_on);
    cJSON_AddNumberToObject(o, "pump_run_s", st.pump_run_s);
    cJSON_AddNumberToObject(o, "pump_total_s", st.pump_total_s);
    cJSON_AddBoolToObject(o, "tank_ok", st.tank_ok);
    cJSON_AddBoolToObject(o, "auto", st.auto_mode);
    cJSON_AddBoolToObject(o, "in_window", st.in_window);
    cJSON_AddNumberToObject(o, "cycles", st.cycles);
    cJSON_AddNumberToObject(o, "fault_cycles", st.fault_cycles);
    cJSON_AddNumberToObject(o, "on_pct", c.on_pct);
    cJSON_AddNumberToObject(o, "off_pct", c.off_pct);
    cJSON_AddNumberToObject(o, "uptime_s", st.uptime_s);
    time_t now = time(NULL);
    cJSON_AddNumberToObject(o, "time", (double)now);

    char *json = cJSON_PrintUnformatted(o);
    if (json) {
        esp_mqtt_client_publish(s_mqtt, TOPIC_STATUS, json, 0, 0, 1);
        cJSON_free(json);
    }
    cJSON_Delete(o);
}

static void publish_task(void *arg)
{
    (void)arg;
    irrig_state_t last_state = IRR_IDLE;
    bool last_pump = false;
    uint32_t elapsed = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed++;
        irrig_status_t st;
        irrigation_get_status(&st);
        irrig_config_t c;
        config_snapshot(&c);
        bool changed = st.state != last_state || st.pump_on != last_pump;
        if (changed || elapsed >= c.sample_period_s) {
            publish_status();
            elapsed = 0;
            last_state = st.state;
            last_pump = st.pump_on;
        }
    }
}

/* ---- MQTT ------------------------------------------------------------- */

static void mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t ev = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        xEventGroupSetBits(s_events, BIT_MQTT_UP);
        esp_mqtt_client_subscribe(s_mqtt, TOPIC_CMD, 1);
        esp_mqtt_client_publish(s_mqtt, TOPIC_ONLINE, "online", 0, 1, 1);
        publish_status();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        xEventGroupClearBits(s_events, BIT_MQTT_UP);
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len == strlen(TOPIC_CMD) && strncmp(ev->topic, TOPIC_CMD, ev->topic_len) == 0) {
            handle_command(ev->data, ev->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    if (s_mqtt) return;
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_IRRIG_MQTT_URI,
        .session.last_will = {
            .topic = TOPIC_ONLINE,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
        .session.keepalive = 30,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) { ESP_LOGE(TAG, "mqtt init failed"); return; }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_mqtt);
    xTaskCreate(publish_task, "mqtt_pub", 4096, NULL, 3, NULL);
}

/* ---- Wi-Fi + SNTP ----------------------------------------------------- */

static void sntp_start(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;
    setenv("TZ", CONFIG_IRRIG_TZ, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "SNTP started, TZ=%s", CONFIG_IRRIG_TZ);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
        esp_wifi_connect();   /* the driver paces its own retries; never sleep in the event loop */
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_GOT_IP);
        sntp_start();
        mqtt_start();
    }
}

esp_err_t net_start(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL), TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL), TAG, "ip evt");

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, CONFIG_IRRIG_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_IRRIG_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = strlen(CONFIG_IRRIG_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_LOGI(TAG, "connecting to '%s'", CONFIG_IRRIG_WIFI_SSID);
    return ESP_OK;
}

#else  /* network disabled */

esp_err_t net_start(void)
{
    return ESP_OK;
}

#endif
