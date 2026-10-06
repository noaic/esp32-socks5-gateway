#pragma once
#include "esp_err.h"
#include "gateway_probe.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum { GW_ACTION_NONE, GW_ACTION_RECONNECT, GW_ACTION_RELAY_RESET, GW_ACTION_REBOOT, GW_ACTION_DIAGNOSE, GW_ACTION_INTERNET, GW_ACTION_RESET_ADMIN, GW_ACTION_RESET_NETWORK } gateway_action_t;
typedef struct {
    bool available, running, led_enabled, led_ready;
    uint32_t job, stack_min;
    gateway_action_t action;
    int result, led_error;
    uint8_t led_color;
    gateway_probe_result_t probe;
    bool internet_auto, job_auto;
    uint8_t internet_target;
    uint64_t next_check_ms;
    gateway_internet_result_t internet_probe;
} gateway_control_snapshot_t;
esp_err_t gateway_control_start(void);
esp_err_t gateway_control_submit(gateway_action_t action);
void gateway_control_snapshot(gateway_control_snapshot_t *out);
esp_err_t gateway_control_set_led(bool enabled);
esp_err_t gateway_control_set_internet(bool enabled,unsigned target);
