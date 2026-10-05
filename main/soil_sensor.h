/* soil_sensor.h - capacitive soil-moisture sensor on ADC1 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint16_t millivolts;   /* median sensor voltage, mV */
    uint8_t  percent;      /* 0 = calibrated dry, 100 = calibrated wet */
    int64_t  timestamp_us; /* esp_timer time of the reading */
} soil_reading_t;

esp_err_t soil_sensor_init(void);
/* Power the sensor (if a power pin is configured), take several samples and
 * return the median converted to percent using the stored calibration. */
esp_err_t soil_sensor_read(soil_reading_t *out);
/* Raw millivolts only, used by the calibration commands. */
esp_err_t soil_sensor_read_mv(uint16_t *mv);
