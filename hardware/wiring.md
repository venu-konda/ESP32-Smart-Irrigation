# Wiring

Default pins (change under `idf.py menuconfig` → Smart Irrigation → Pins):

| Signal | ESP32 pin | Connects to | Notes |
|---|---|---|---|
| Soil moisture analog | GPIO34 (ADC1_CH6) | Sensor AOUT | Input-only pin, no pull-ups. ADC1 keeps working while Wi-Fi is on; ADC2 does not |
| Sensor power | GPIO25 | Sensor VCC | Sensor draws ~5 mA, within GPIO limits. Set to -1 to power the sensor permanently from 3V3 |
| Pump relay | GPIO26 | Relay IN (or MOSFET gate via 220 Ω) | Default active-high; enable "active-low" in menuconfig for low-trigger relay modules |
| Tank float switch | GPIO27 | Switch, other side to GND | Internal pull-up; closed (LOW) = water present. Set to -1 if there is no switch |
| Status LED | GPIO2 | On-board LED on most DevKits | |
| GND | GND | Sensor GND, relay GND, switch, pump supply GND | One common ground |

```
                      ESP32 DevKitC
                    ┌───────────────┐
  Sensor AOUT ──────┤ GPIO34        │
  Sensor VCC  ──────┤ GPIO25        │
  Sensor GND  ──────┤ GND           │
                    │               │
  Relay IN    ──────┤ GPIO26        │     ┌────────┐   12 V
  Relay VCC   ──────┤ 5V            │     │ Relay  ├────┤│ pump
  Relay GND   ──────┤ GND           │     └────────┘
                    │               │
  Float switch ─────┤ GPIO27        │  (other side of the switch to GND)
                    │ GPIO2 (LED)   │
                    └───────────────┘
```

Power: the pump must have its own supply. Never drive it from the ESP32 5 V
pin. Share only the ground. If the ESP32 resets when the pump starts, add a
100 µF capacitor on its 5 V input or move the ESP32 to a separate supply.

Sensor placement: bury the probe up to the marked line, at the root depth of
the plant, 10–15 cm from the drip point so it measures soil rather than the
wet spot under the outlet.
