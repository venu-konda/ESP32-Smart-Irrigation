/* pump.h - relay-driven water pump with a hardware-timer failsafe */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t pump_init(void);
/* Switch the pump on for at most max_seconds. A one-shot timer turns it off
 * even if the controlling task stalls. */
void pump_on(uint32_t max_seconds);
void pump_off(void);
bool pump_is_on(void);
/* Seconds the pump has been running in the current cycle (0 if off). */
uint32_t pump_run_seconds(void);
/* Total seconds the pump has run since boot. */
uint32_t pump_total_seconds(void);
/* True once if the failsafe timer had to switch the pump off; cleared on read. */
bool pump_failsafe_tripped(void);
