/* irrigation.c - watering controller (FreeRTOS tasks + state machine)
 *
 * Two tasks:
 *   sensor_task     samples the soil probe on a period (faster while watering)
 *                   and posts readings to the controller queue.
 *   controller_task consumes readings and commands, evaluates the state machine
 *                   once per second, and drives the pump.
 */
#include <time.h>
#include <string.h>
#include "irrigation.h"
#include "config.h"
#include "pump.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "irrig";

#define WATERING_SAMPLE_S 5     /* sampling period while the pump runs */

typedef enum { EV_READING, EV_MANUAL, EV_RESET_FAULT, EV_SAMPLE_NOW } ev_type_t;
typedef struct {
    ev_type_t type;
    soil_reading_t reading;
    uint32_t seconds;
} irrig_event_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_status_lock;
static TaskHandle_t s_sensor_task;
static irrig_status_t s_status;

/* controller-private timing, seconds of uptime */
static uint32_t s_cycle_start, s_cycle_end, s_soak_start, s_manual_end;

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

const char *irrigation_state_name(irrig_state_t s)
{
    switch (s) {
    case IRR_IDLE:         return "IDLE";
    case IRR_WATERING:     return "WATERING";
    case IRR_SOAK:         return "SOAK";
    case IRR_MANUAL:       return "MANUAL";
    case IRR_LOCKOUT_TANK: return "LOCKOUT_TANK";
    case IRR_FAULT:        return "FAULT";
    default:               return "?";
    }
}

static bool tank_present(void)
{
#if CONFIG_IRRIG_TANK_SWITCH_GPIO >= 0
    int lvl = gpio_get_level(CONFIG_IRRIG_TANK_SWITCH_GPIO);
#if CONFIG_IRRIG_TANK_SWITCH_ACTIVE_LOW
    return lvl == 0;
#else
    return lvl == 1;
#endif
#else
    return true;
#endif
}

static bool window_ok(const irrig_config_t *c, bool *time_known)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    *time_known = (tm.tm_year + 1900) >= 2020;
    if (!c->window_enabled) return true;
    if (!*time_known) return true;          /* no clock: never block watering */
    int h = tm.tm_hour;
    if (c->window_start_h <= c->window_end_h) {
        return h >= c->window_start_h && h < c->window_end_h;
    }
    return h >= c->window_start_h || h < c->window_end_h;   /* window wraps midnight */
}

static void set_state(irrig_state_t s)
{
    if (s_status.state != s) {
        ESP_LOGI(TAG, "%s -> %s", irrigation_state_name(s_status.state), irrigation_state_name(s));
        s_status.state = s;
    }
}

/* ---- sensor task ------------------------------------------------------ */

static void sensor_task(void *arg)
{
    (void)arg;
    for (;;) {
        irrig_event_t ev = { .type = EV_READING };
        if (soil_sensor_read(&ev.reading) == ESP_OK) {
            xQueueSend(s_queue, &ev, pdMS_TO_TICKS(100));
        } else {
            ESP_LOGW(TAG, "sensor read failed");
        }
        irrig_config_t c;
        config_snapshot(&c);
        uint32_t period = pump_is_on() ? WATERING_SAMPLE_S : c.sample_period_s;
        /* wake early if someone calls irrigation_request_sample() */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(period * 1000));
    }
}

/* ---- controller ------------------------------------------------------- */

static void handle_event(const irrig_event_t *ev, const irrig_config_t *c)
{
    switch (ev->type) {
    case EV_READING:
        s_status.last = ev->reading;
        s_status.have_reading = true;
        ESP_LOGI(TAG, "moisture %u%% (%u mV) state=%s pump=%s",
                 ev->reading.percent, ev->reading.millivolts,
                 irrigation_state_name(s_status.state), pump_is_on() ? "on" : "off");
        break;

    case EV_MANUAL:
        if (ev->seconds == 0) {
            pump_off();
            if (s_status.state == IRR_MANUAL || s_status.state == IRR_WATERING) {
                s_cycle_end = now_s();
                set_state(IRR_IDLE);
            }
        } else if (s_status.state == IRR_FAULT) {
            ESP_LOGW(TAG, "manual watering refused: in FAULT, run 'fault reset' first");
        } else if (s_status.tank_ok) {
            uint32_t limit = ev->seconds > c->max_pump_s * 3 ? c->max_pump_s * 3 : ev->seconds;
            pump_on(limit);
            s_manual_end = now_s() + limit;
            set_state(IRR_MANUAL);
            if (s_sensor_task) xTaskNotifyGive(s_sensor_task);   /* switch to the fast sampling period now */
        } else {
            ESP_LOGW(TAG, "manual watering refused: tank empty");
        }
        break;

    case EV_RESET_FAULT:
        if (s_status.state == IRR_FAULT) {
            s_status.fault_cycles = 0;
            set_state(IRR_IDLE);
        }
        break;

    case EV_SAMPLE_NOW:
        if (s_sensor_task) xTaskNotifyGive(s_sensor_task);
        break;
    }
}

static void step(const irrig_config_t *c)
{
    uint32_t now = now_s();
    bool time_known;
    s_status.in_window = window_ok(c, &time_known);
    s_status.auto_mode = c->auto_mode != 0;

    if (pump_failsafe_tripped()) {
        ESP_LOGE(TAG, "pump failsafe tripped, entering FAULT");
        set_state(IRR_FAULT);
    }

    /* Tank protection overrides everything except an existing FAULT. */
    if (!s_status.tank_ok && s_status.state != IRR_FAULT) {
        if (pump_is_on()) pump_off();
        if (s_status.state != IRR_LOCKOUT_TANK) {
            ESP_LOGW(TAG, "tank empty, pump locked out");
            set_state(IRR_LOCKOUT_TANK);
        }
    }

    switch (s_status.state) {
    case IRR_LOCKOUT_TANK:
        if (s_status.tank_ok) set_state(IRR_IDLE);
        break;

    case IRR_FAULT:
        if (pump_is_on()) pump_off();
        break;

    case IRR_MANUAL:
        if (now >= s_manual_end || !pump_is_on()) {
            pump_off();
            s_cycle_end = now;
            set_state(IRR_IDLE);
        }
        break;

    case IRR_IDLE:
        if (!c->auto_mode || !s_status.have_reading) break;
        if (s_status.last.percent >= c->on_pct) break;
        if (!s_status.in_window) break;
        if (s_status.cycles > 0 || s_cycle_end > 0) {
            if (now - s_cycle_end < c->min_interval_s) break;
        }
        pump_on(c->max_pump_s);
        s_cycle_start = now;
        set_state(IRR_WATERING);
        if (s_sensor_task) xTaskNotifyGive(s_sensor_task);   /* switch to the fast sampling period now */
        break;

    case IRR_WATERING:
        if (s_status.last.percent >= c->off_pct) {
            pump_off();
            s_status.cycles++;
            s_status.fault_cycles = 0;
            s_cycle_end = now;
            ESP_LOGI(TAG, "target reached after %lus", (unsigned long)(now - s_cycle_start));
            set_state(IRR_IDLE);
        } else if (now - s_cycle_start >= c->max_pump_s || !pump_is_on()) {
            pump_off();
            s_status.cycles++;
            s_status.fault_cycles++;
            s_cycle_end = now;
            if (s_status.fault_cycles >= c->max_fault_cycles) {
                ESP_LOGE(TAG, "%u cycles without reaching %u%%: sensor or water supply problem",
                         s_status.fault_cycles, c->off_pct);
                set_state(IRR_FAULT);
            } else {
                s_soak_start = now;
                set_state(IRR_SOAK);
            }
        }
        break;

    case IRR_SOAK:
        if (now - s_soak_start >= c->soak_s) set_state(IRR_IDLE);
        break;
    }

    s_status.pump_on = pump_is_on();
    s_status.pump_run_s = pump_run_seconds();
    s_status.pump_total_s = pump_total_seconds();
    s_status.uptime_s = now;
}

static void controller_task(void *arg)
{
    (void)arg;
    irrig_event_t ev;
    for (;;) {
        bool got = xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(1000)) == pdTRUE;
        irrig_config_t c;
        config_snapshot(&c);
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        s_status.tank_ok = tank_present();
        if (got) handle_event(&ev, &c);
        step(&c);
        xSemaphoreGive(s_status_lock);
    }
}

/* ---- public API ------------------------------------------------------- */

esp_err_t irrigation_start(void)
{
#if CONFIG_IRRIG_TANK_SWITCH_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_IRRIG_TANK_SWITCH_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "tank switch gpio");
#endif
    s_queue = xQueueCreate(8, sizeof(irrig_event_t));
    s_status_lock = xSemaphoreCreateMutex();
    if (!s_queue || !s_status_lock) return ESP_ERR_NO_MEM;
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = IRR_IDLE;
    s_status.tank_ok = tank_present();

    if (xTaskCreate(controller_task, "irrig_ctrl", 4096, NULL, 6, NULL) != pdPASS) return ESP_FAIL;
    if (xTaskCreate(sensor_task, "irrig_sense", 3072, NULL, 5, &s_sensor_task) != pdPASS) return ESP_FAIL;
    ESP_LOGI(TAG, "controller started");
    return ESP_OK;
}

void irrigation_get_status(irrig_status_t *out)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_lock);
}

esp_err_t irrigation_manual_water(uint32_t seconds)
{
    irrig_event_t ev = { .type = EV_MANUAL, .seconds = seconds };
    return xQueueSend(s_queue, &ev, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t irrigation_reset_fault(void)
{
    irrig_event_t ev = { .type = EV_RESET_FAULT };
    return xQueueSend(s_queue, &ev, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void irrigation_request_sample(void)
{
    irrig_event_t ev = { .type = EV_SAMPLE_NOW };
    xQueueSend(s_queue, &ev, 0);
}
