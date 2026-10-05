/* irrigation.h - watering controller (FreeRTOS tasks + state machine) */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "soil_sensor.h"

typedef enum {
    IRR_IDLE = 0,       /* monitoring, pump off */
    IRR_WATERING,       /* automatic cycle, pump on */
    IRR_SOAK,           /* pause after a max-run cycle, pump off */
    IRR_MANUAL,         /* operator-requested run, pump on */
    IRR_LOCKOUT_TANK,   /* tank empty, pump forced off */
    IRR_FAULT,          /* repeated cycles without moisture rising; needs reset */
} irrig_state_t;

typedef struct {
    irrig_state_t state;
    soil_reading_t last;      /* most recent reading */
    bool have_reading;
    bool pump_on;
    bool tank_ok;
    bool auto_mode;
    bool in_window;           /* current time is inside the watering window */
    uint32_t cycles;          /* completed automatic cycles since boot */
    uint32_t pump_total_s;    /* pump run time since boot */
    uint32_t pump_run_s;      /* current run, 0 if off */
    uint8_t  fault_cycles;    /* consecutive max-run cycles */
    uint32_t uptime_s;
} irrig_status_t;

/* Create the sensor and controller tasks. */
esp_err_t irrigation_start(void);
void irrigation_get_status(irrig_status_t *out);
const char *irrigation_state_name(irrig_state_t s);
/* Run the pump for `seconds` regardless of moisture (0 = stop now). Refused while the tank is empty. */
esp_err_t irrigation_manual_water(uint32_t seconds);
/* Leave the FAULT state. */
esp_err_t irrigation_reset_fault(void);
/* Force an immediate sensor read; the result arrives through the normal path. */
void irrigation_request_sample(void);
