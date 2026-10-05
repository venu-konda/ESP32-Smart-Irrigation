/* console_cmds.c - UART command console (esp_console REPL)
 *
 *   status                 show controller state and last reading
 *   get [key]              show one or all settings
 *   set <key> <value>      change a setting in RAM
 *   save                   persist settings to NVS
 *   defaults               restore default settings in RAM
 *   cal dry|wet            record the current sensor voltage as the dry/wet point
 *   cal show               show raw sensor voltage
 *   pump on <seconds>      run the pump manually
 *   pump off               stop the pump
 *   auto on|off            enable/disable automatic control
 *   fault reset            leave the FAULT state
 *   sample                 take a reading now
 *   reboot
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console_cmds.h"
#include "config.h"
#include "irrigation.h"
#include "soil_sensor.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    irrig_status_t st;
    irrigation_get_status(&st);
    printf("state        : %s\n", irrigation_state_name(st.state));
    if (st.have_reading) {
        printf("moisture     : %u %% (%u mV)\n", st.last.percent, st.last.millivolts);
    } else {
        printf("moisture     : no reading yet\n");
    }
    printf("pump         : %s (run %lus, total %lus)\n", st.pump_on ? "ON" : "off",
           (unsigned long)st.pump_run_s, (unsigned long)st.pump_total_s);
    printf("tank         : %s\n", st.tank_ok ? "ok" : "EMPTY");
    printf("auto mode    : %s\n", st.auto_mode ? "on" : "off");
    printf("in window    : %s\n", st.in_window ? "yes" : "no");
    printf("cycles       : %lu, consecutive max-run cycles: %u\n",
           (unsigned long)st.cycles, st.fault_cycles);
    printf("uptime       : %lus\n", (unsigned long)st.uptime_s);
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    if (argc < 2) { config_print(); return 0; }
    int v;
    if (config_get(argv[1], &v) != ESP_OK) { printf("unknown key '%s'\n", argv[1]); return 1; }
    printf("%s = %d\n", argv[1], v);
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: set <key> <value>\nkeys:");
        for (const char *const *k = config_keys(); *k; k++) printf(" %s", *k);
        printf("\n");
        return 1;
    }
    char *end;
    long v = strtol(argv[2], &end, 10);
    if (*end) { printf("value must be an integer\n"); return 1; }
    esp_err_t err = config_set(argv[1], (int)v);
    if (err == ESP_ERR_NOT_FOUND) { printf("unknown key '%s'\n", argv[1]); return 1; }
    if (err == ESP_ERR_INVALID_ARG) { printf("value out of range\n"); return 1; }
    printf("%s = %ld (run 'save' to persist)\n", argv[1], v);
    return 0;
}

static int cmd_save(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = config_save();
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_defaults(int argc, char **argv)
{
    (void)argc; (void)argv;
    config_reset_defaults();
    printf("defaults restored in RAM (run 'save' to persist)\n");
    return 0;
}

static int cmd_cal(int argc, char **argv)
{
    uint16_t mv;
    if (soil_sensor_read_mv(&mv) != ESP_OK) { printf("sensor read failed\n"); return 1; }
    if (argc < 2 || strcmp(argv[1], "show") == 0) {
        printf("sensor: %u mV\n", mv);
        return 0;
    }
    int dry, wet;
    config_get("raw_dry", &dry);
    config_get("raw_wet", &wet);
    if (strcmp(argv[1], "dry") == 0) dry = mv;
    else if (strcmp(argv[1], "wet") == 0) wet = mv;
    else { printf("usage: cal dry|wet|show\n"); return 1; }
    esp_err_t err = config_set_calibration((uint16_t)dry, (uint16_t)wet);
    if (err != ESP_OK) {
        printf("rejected: dry (%d mV) must exceed wet (%d mV) by at least 100 mV\n", dry, wet);
        return 1;
    }
    printf("calibration: dry=%d mV wet=%d mV (run 'save' to persist)\n", dry, wet);
    return 0;
}

static int cmd_pump(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        irrigation_manual_water(0);
        printf("pump off\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "on") == 0) {
        long s = strtol(argv[2], NULL, 10);
        if (s <= 0) { printf("seconds must be > 0\n"); return 1; }
        irrigation_manual_water((uint32_t)s);
        printf("pump on for %lds (capped at 3 x max_pump_s)\n", s);
        return 0;
    }
    printf("usage: pump on <seconds> | pump off\n");
    return 1;
}

static int cmd_auto(int argc, char **argv)
{
    if (argc < 2) { printf("usage: auto on|off\n"); return 1; }
    int v = strcmp(argv[1], "on") == 0 ? 1 : strcmp(argv[1], "off") == 0 ? 0 : -1;
    if (v < 0) { printf("usage: auto on|off\n"); return 1; }
    config_set("auto_mode", v);
    printf("auto mode %s (run 'save' to persist)\n", v ? "on" : "off");
    return 0;
}

static int cmd_fault(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "reset") != 0) { printf("usage: fault reset\n"); return 1; }
    irrigation_reset_fault();
    printf("fault reset requested\n");
    return 0;
}

static int cmd_sample(int argc, char **argv)
{
    (void)argc; (void)argv;
    irrigation_request_sample();
    printf("sampling...\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_restart();
    return 0;
}

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "irrig>";
    repl_cfg.max_cmdline_length = 128;
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl), "console", "repl");
    esp_console_register_help_command();

    const esp_console_cmd_t cmds[] = {
        { .command = "status",   .help = "Show controller state and last reading", .func = cmd_status },
        { .command = "get",      .help = "get [key]: show settings",               .func = cmd_get },
        { .command = "set",      .help = "set <key> <value>: change a setting",    .func = cmd_set },
        { .command = "save",     .help = "Persist settings to NVS",                .func = cmd_save },
        { .command = "defaults", .help = "Restore default settings in RAM",        .func = cmd_defaults },
        { .command = "cal",      .help = "cal dry|wet|show: sensor calibration",   .func = cmd_cal },
        { .command = "pump",     .help = "pump on <seconds> | pump off",           .func = cmd_pump },
        { .command = "auto",     .help = "auto on|off: automatic control",         .func = cmd_auto },
        { .command = "fault",    .help = "fault reset: leave FAULT state",         .func = cmd_fault },
        { .command = "sample",   .help = "Take a sensor reading now",              .func = cmd_sample },
        { .command = "reboot",   .help = "Restart the controller",                 .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), "console", "register");
    }
    return esp_console_start_repl(repl);
}
