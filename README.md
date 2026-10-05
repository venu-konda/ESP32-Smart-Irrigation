# ESP32 Smart Irrigation Controller

Autonomous soil-moisture-based irrigation on an ESP32, written in C on
ESP-IDF with FreeRTOS. A capacitive probe measures soil moisture, a relay
drives a pump, and a small state machine waters only when the soil is dry,
only inside an allowed time window, only when the tank has water, and never
for longer than a configured limit. Settings persist in flash, a UART console
configures and calibrates the system, and an optional Wi-Fi link publishes
status over MQTT and accepts commands.

[![Build firmware](https://github.com/venu-konda/ESP32-Smart-Irrigation/actions/workflows/build.yml/badge.svg)](https://github.com/venu-konda/ESP32-Smart-Irrigation/actions/workflows/build.yml)

## Features

- **Calibrated moisture sensing**: ADC1 with the ESP-IDF calibration scheme,
  median of 9 samples, dry/wet calibration points stored in NVS, sensor
  powered only while sampling to limit electrode corrosion.
- **Hysteresis control**: pump starts below `on_pct`, stops at `off_pct`.
- **Pump safety**: maximum run time per cycle, soak pause after a timed-out
  cycle, minimum interval between cycles, tank-empty lockout from a float
  switch, and a hardware timer that switches the pump off even if the
  control task stalls.
- **Fault detection**: if several consecutive cycles end on the time limit
  without reaching `off_pct`, the controller enters `FAULT` (dry sensor, empty
  line, broken pump) and waits for a reset instead of flooding or running dry.
- **Watering window**: water only between two hours of local time, with the
  clock set by SNTP. Without a clock the window is ignored rather than
  blocking irrigation.
- **Persistent settings** in NVS, with defaults applied on first boot.
- **UART console** for status, settings, calibration, manual pump control and
  fault reset.
- **MQTT telemetry and control** (optional): retained JSON status, online/offline
  availability with last-will, and a JSON command topic.
- **Status LED** patterns for idle, watering, tank empty and fault.

## Hardware

See [hardware/wiring.md](hardware/wiring.md) for the pinout and diagram and
[hardware/BOM.md](hardware/BOM.md) for parts. Default pins:

| Signal | GPIO |
|---|---|
| Soil moisture sensor (analog) | 34 (ADC1_CH6) |
| Sensor power | 25 |
| Pump relay | 26 |
| Tank float switch | 27 |
| Status LED | 2 |

All pins and polarities are configurable in `menuconfig`.

## Building and flashing

Requirements: [ESP-IDF v5.1 or newer](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)
(v5.3 is what CI builds with) and an ESP32 board.

```sh
git clone https://github.com/venu-konda/ESP32-Smart-Irrigation.git
cd ESP32-Smart-Irrigation
idf.py set-target esp32
idf.py menuconfig          # Smart Irrigation -> pins, Wi-Fi, MQTT, timezone
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Without a local toolchain, the official Docker image builds it:

```sh
docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.3 idf.py build
```

To run without networking, disable **Smart Irrigation → Enable Wi-Fi, SNTP
time and MQTT telemetry** in `menuconfig`. Everything else works standalone.

The defaults assume a 4 MB flash with the "single factory app, large"
partition layout; the full build with networking is about 1 MB. On a 2 MB
board, set **Serial flasher config → Flash size** to 2 MB and either disable
networking or add a custom partition table.

## First-time setup

1. Flash and open the monitor. The console prompt is `irrig>`.
2. Calibrate the sensor:
   ```
   cal dry        # probe in dry air
   cal wet        # probe in a glass of water, up to the marked line
   save
   ```
3. Check the reading in soil: `status`. Adjust `on_pct` / `off_pct` for your
   plant (defaults 35 / 60).
4. Test the pump: `pump on 5`.
5. Set the window and limits, then `save`:
   ```
   set window_start_h 6
   set window_end_h 9
   set max_pump_s 90
   save
   ```

## How it works

### Tasks

| Task | Priority | Role |
|---|---|---|
| `irrig_sense` | 5 | Powers the sensor, takes 9 ADC samples, posts the median to a queue. Period is `sample_period_s` while idle and 5 s while the pump runs |
| `irrig_ctrl` | 6 | Consumes readings and commands from the queue, evaluates the state machine once per second, drives the pump |
| `status_led` | 2 | Blinks the LED according to the state |
| `mqtt_pub` | 3 | Publishes status every sample period and on every state or pump change (network build only) |
| console REPL | – | esp_console over UART0 |
| Wi-Fi / MQTT | – | ESP-IDF's own event-loop and client tasks |

Readings and commands travel through one FreeRTOS queue, so the controller
is the only task that touches the pump. A mutex protects the status snapshot
read by the console, LED and MQTT tasks; settings are copied under a mutex
before each evaluation.

### State machine

```
            tank empty (any state except FAULT)
   ┌──────────────────────────────────────────────┐
   │                                              ▼
   │   moisture < on_pct             ┌──────────────────┐  tank ok
   │   && in window                  │   LOCKOUT_TANK   │────────┐
   │   && interval elapsed           └──────────────────┘        │
   │   && auto on                                                 ▼
┌──┴───┐ ───────────────────────────────────────────────▶ ┌──────────┐
│ IDLE │                                                  │ WATERING │
└──────┘ ◀────────────────────────────────────────────── └────┬─────┘
   ▲  ▲      moisture >= off_pct  (cycle complete)             │
   │  │                                                        │ max_pump_s elapsed
   │  │   soak_s elapsed   ┌──────┐                            │
   │  └────────────────────│ SOAK │◀───────────────────────────┤ fault_cycles < max
   │                       └──────┘                            │
   │      fault reset      ┌───────┐                           │
   └───────────────────────│ FAULT │◀──────────────────────────┘ fault_cycles >= max
                           └───────┘
   MANUAL: `pump on N` from console/MQTT runs the pump for N seconds
   (capped at 3 × max_pump_s) from IDLE, SOAK or WATERING, then returns to IDLE.
   It is refused in LOCKOUT_TANK and in FAULT (run `fault reset` first).
   Any manual run, or `pump off` during WATERING, restarts the min_interval_s timer.
```

A cycle that reaches `off_pct` resets the fault counter. One that hits
`max_pump_s` increments it and goes through `SOAK` so water can spread to the
probe before the next attempt.

### Settings

| Key | Default | Meaning |
|---|---|---|
| `on_pct` | 35 | Start watering below this moisture % |
| `off_pct` | 60 | Stop watering at this moisture % |
| `max_pump_s` | 120 | Longest single pump run, seconds |
| `soak_s` | 600 | Pause after a timed-out cycle |
| `min_interval_s` | 1800 | Minimum gap between automatic cycles |
| `sample_period_s` | 30 | Sensor period while idle |
| `raw_dry` / `raw_wet` | 2650 / 1150 | Calibration points, millivolts |
| `window_start_h` / `window_end_h` | 5 / 9 | Watering window, local hours (end exclusive, may wrap midnight; equal values mean never) |
| `window_enabled` | 1 | Enforce the window |
| `auto_mode` | 1 | Automatic control on/off |
| `max_fault_cycles` | 3 | Timed-out cycles before FAULT |

Changes made with `set` take effect immediately and persist after `save`.

### Console commands

```
status                 controller state, last reading, pump, tank, counters
get [key]              show one or all settings
set <key> <value>      change a setting
save                   write settings to NVS
defaults               restore defaults (then save)
cal dry|wet|show       calibration
pump on <s> | pump off manual pump control
auto on|off            automatic control
fault reset            leave FAULT
sample                 read the sensor now
reboot
```

### MQTT

Base topic `garden/irrigation` by default.

| Topic | Direction | Payload |
|---|---|---|
| `<base>/status` | publish, retained | JSON: `state`, `moisture_pct`, `sensor_mv`, `pump`, `pump_run_s`, `pump_total_s`, `tank_ok`, `auto`, `in_window`, `cycles`, `fault_cycles`, `on_pct`, `off_pct`, `uptime_s`, `time` |
| `<base>/online` | publish, retained | `online`, or `offline` via last will |
| `<base>/cmd` | subscribe | `{"cmd":"water","seconds":30}`, `{"cmd":"stop"}`, `{"cmd":"set","key":"on_pct","value":40}`, `{"cmd":"save"}`, `{"cmd":"auto","value":0}`, `{"cmd":"reset_fault"}`, `{"cmd":"sample"}` |

`tools/mqtt_logger.py` subscribes to the status topic and appends every
message to a CSV file for plotting or spreadsheets:

```sh
pip install paho-mqtt
python3 tools/mqtt_logger.py --broker 192.168.1.10 --out irrigation_log.csv
```

## Project layout

```
main/
  app_main.c       boot sequence
  config.[ch]      settings struct, key table, NVS load/save
  soil_sensor.[ch] ADC + calibration, median filter, sensor power switching
  pump.[ch]        relay output, run-time accounting, failsafe timer
  irrigation.[ch]  sensor task, controller task, state machine
  status_led.[ch]  LED patterns
  console_cmds.[ch] esp_console commands
  net_mqtt.[ch]    Wi-Fi station, SNTP, MQTT client (compiled out when disabled)
  Kconfig.projbuild  menuconfig options
hardware/          wiring and bill of materials
tools/             MQTT CSV logger
.github/workflows  CI build on ESP-IDF v5.3
```

## Limitations and ideas

- One zone. Multi-zone support would need a sensor/pump pair and a state
  machine instance per zone; the current code uses module-level state.
- No weather or forecast input. A rain sensor on a spare GPIO could gate the
  `IDLE → WATERING` transition the same way the tank switch does.
- The watering window relies on SNTP. A DS3231 RTC would keep time offline.
- Over-the-air updates are not included; ESP-IDF's `esp_https_ota` drops in
  with a partition table change.

## License

MIT. See [LICENSE](LICENSE).
